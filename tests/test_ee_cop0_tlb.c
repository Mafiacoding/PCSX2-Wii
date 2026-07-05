/*
 * test_ee_cop0_tlb.c - host-native test for ee_core.c's COP0 TLB
 * (TLBR/TLBWI/TLBWR/TLBP) and the KUSEG address-translation path
 * wired into ee_mem_ptr(). Ported from PCSX2's COP0.cpp (register
 * round-tripping) and R5900.h's tlbs struct (VPN2/PFN/Mask math for
 * translation).
 *
 * Both pieces were found necessary while chasing real BIOS boot
 * further past the COP0 PRId fix (see docs/STATUS.md's "round 5"):
 * the real BIOS calls TLBWI very early to install a kernel-stack
 * mapping, then uses a KUSEG (0x00000000-0x7FFFFFFF) stack pointer
 * that this project's old flat "phys = addr & 0x1FFFFFFF" shortcut
 * could not resolve correctly (KUSEG is a genuinely MAPPED segment,
 * unlike kseg0/kseg1).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_lui(int rt, uint16_t imm)        { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_mtc0(int rt, int rd) { return (0x10 << 26) | (0x04 << 21) | (rt << 16) | (rd << 11); }
static uint32_t enc_mfc0(int rt, int rd) { return (0x10 << 26) | (0x00 << 21) | (rt << 16) | (rd << 11); }
static uint32_t enc_cop0_co(int funct)   { return (0x10 << 26) | (0x10 << 21) | funct; }
static uint32_t enc_sw(int rt, int rs, int16_t imm) { return (0x2B << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lw(int rt, int rs, int16_t imm) { return (0x23 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

#define TLBWI 0x02
#define TLBR  0x01
#define TLBWR 0x06
#define TLBP  0x08

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    uint8_t *prog = bios.data;
    ee_state_t *st;
    int pc;

    /* --- TLBWI: write PageMask/EntryHi/EntryLo0/EntryLo1 (via MTC0)
     * into TLB entry 0 (Index=0 by default), then confirm via direct
     * struct access. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    wle32(prog+pc, enc_lui(8, 0x7000));   pc+=4; /* $t0 = 0x70000000 (EntryHi: VPN2 for KUSEG 0x70000000) */
    wle32(prog+pc, enc_mtc0(8, 10));      pc+=4; /* MTC0 $t0, EntryHi (cop0[10]) */
    wle32(prog+pc, enc_lui(9, 0x8000));   pc+=4;
    wle32(prog+pc, enc_ori(9, 9, 0x0007));pc+=4; /* $t1 = 0x80000007 (EntryLo0: V=1,D=1,C=1(=011b? just needs V bit set),PFN=0x200000>>12... simplified: just needs V(bit1)=1) */
    wle32(prog+pc, enc_mtc0(9, 2));       pc+=4; /* MTC0 $t1, EntryLo0 (cop0[2]) */
    wle32(prog+pc, enc_ori(10, 0, 0x0007));pc+=4; /* $t2 = 0x00000007 (EntryLo1) */
    wle32(prog+pc, enc_mtc0(10, 3));      pc+=4; /* MTC0 $t2, EntryLo1 (cop0[3]) */
    wle32(prog+pc, enc_cop0_co(TLBWI));   pc+=4; /* TLBWI (Index=0) */
    wle32(prog+pc, enc_break());          pc+=4;

    if (ee_core_init(&bios) != 0) { printf("init failed\n"); return 1; }
    st = ee_core_get_state();
    ee_core_run(&bios);
    CHECK(st->halted == 1, "TLBWI test: core halted on BREAK");
    CHECK(st->tlb[0].entry_hi == 0x70000000u, "TLBWI: tlb[0].entry_hi == EntryHi that was written");
    CHECK(st->tlb[0].entry_lo0 == 0x80000007u, "TLBWI: tlb[0].entry_lo0 == EntryLo0 that was written");
    CHECK(st->tlb[0].entry_lo1 == 0x00000007u, "TLBWI: tlb[0].entry_lo1 == EntryLo1 that was written");

    /* --- TLBR: read entry 0 back into PageMask/EntryHi/EntryLo0/EntryLo1,
     * confirm via MFC0. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    wle32(prog+pc, enc_cop0_co(TLBR));     pc+=4; /* TLBR from Index=0 (default 0) */
    wle32(prog+pc, enc_mfc0(8, 10));       pc+=4; /* $t0 = EntryHi */
    wle32(prog+pc, enc_break());           pc+=4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    st->tlb[0].entry_hi = 0x70000000u;
    st->tlb[0].entry_lo0 = 0x80000007u;
    st->tlb[0].entry_lo1 = 0x00000007u;
    st->tlb[0].page_mask = 0;
    ee_core_run(&bios);
    CHECK((uint32_t)st->gpr[8].ud0 == 0x70000000u, "TLBR: EntryHi read back correctly via MFC0 after TLBR");

    /* --- TLBP: probe for a matching entry, Index should be set to the
     * matching entry's number; probing for a VPN2 with no match should
     * set Index's sign bit (0x80000000). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    wle32(prog+pc, enc_lui(8, 0x7000));    pc+=4;
    wle32(prog+pc, enc_mtc0(8, 10));       pc+=4; /* EntryHi = 0x70000000 (matches tlb[3]) */
    wle32(prog+pc, enc_cop0_co(TLBP));     pc+=4;
    wle32(prog+pc, enc_mfc0(9, 0));        pc+=4; /* $t1 = Index after probe */
    wle32(prog+pc, enc_break());           pc+=4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    st->tlb[3].entry_hi = 0x70000000u;
    st->tlb[3].entry_lo0 = 0x80000007u;
    st->tlb[3].entry_lo1 = 0x00000007u;
    ee_core_run(&bios);
    CHECK((uint32_t)st->gpr[9].ud0 == 3u, "TLBP: finds the matching entry (index 3) via VPN2 match");

    /* --- TLBP with no match: Index gets the 0x80000000 sign-bit
     * "not found" marker. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    wle32(prog+pc, enc_lui(8, 0x1234));    pc+=4;
    wle32(prog+pc, enc_mtc0(8, 10));       pc+=4; /* EntryHi = 0x12340000, matches nothing */
    wle32(prog+pc, enc_cop0_co(TLBP));     pc+=4;
    wle32(prog+pc, enc_mfc0(9, 0));        pc+=4;
    wle32(prog+pc, enc_break());           pc+=4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    st->tlb[3].entry_hi = 0x70000000u;
    st->tlb[3].entry_lo0 = 0x80000007u;
    st->tlb[3].entry_lo1 = 0x00000007u;
    ee_core_run(&bios);
    CHECK(st->gpr[9].ud0 == 0xFFFFFFFF80000000ULL, "TLBP: no match sets Index to 0x80000000 (read back sign-extended via MFC0)");

    /* --- KUSEG address translation via ee_mem_ptr(): install a 4KB
     * entry covering 0x70000000-0x70002000 (VPN2 for 0x70000000, one
     * pair of 4KB pages), then SW/LW through a KUSEG address in that
     * range and confirm it lands in the SAME physical RAM cell as the
     * kseg0-mapped equivalent (0x80000000 | phys). EntryLo0.PFN is set
     * so the even page (0x70000000-0x70000FFF) maps to physical RAM
     * offset 0x2000000-ish (near a small, in-bounds test area). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    wle32(prog+pc, enc_lui(4, 0x7000));    pc+=4; /* $a0 = 0x70000000 (KUSEG addr, even page) */
    wle32(prog+pc, enc_lui(5, 0x1234));    pc+=4; /* $a1 = 0x12340000 (value to store) */
    wle32(prog+pc, enc_sw(5, 4, 0x100));   pc+=4; /* SW $a1, 0x100($a0) -> KUSEG 0x70000100 */
    wle32(prog+pc, enc_lw(6, 4, 0x100));   pc+=4; /* LW $a2, 0x100($a0) -> read it back via the SAME KUSEG path */
    wle32(prog+pc, enc_break());           pc+=4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    /* EntryLo0.PFN (bits 6-25) picks physical page 0x1000 (i.e.
     * physical address 0x1000 << 12 = 0x01000000) for the even page;
     * V(bit1)=1 required to be considered valid-ish by our simplified
     * model (we don't check V, but set it anyway to be realistic). */
    st->tlb[0].entry_hi  = 0x70000000u;
    st->tlb[0].entry_lo0 = (0x1000u << 6) | 0x2u; /* PFN=0x1000 -> phys 0x01000000, V=1 */
    st->tlb[0].entry_lo1 = 0x2u;
    st->tlb[0].page_mask = 0;
    ee_core_run(&bios);
    CHECK(st->halted == 1, "KUSEG translation test: core halted on BREAK");
    CHECK(st->gpr[6].ud0 == 0x0000000012340000ULL,
          "KUSEG SW/LW round-trip through a TLB-mapped 0x70000100 address works (value survives, sign-extended)");
    CHECK(*(uint32_t*)(st->ram + 0x01000100) == 0x12340000u,
          "KUSEG write actually landed at the translated physical address (0x01000000 + 0x100 offset)");

    /* --- KUSEG address with NO matching TLB entry: real hardware
     * raises a TLB Refill exception (this project didn't model
     * exception delivery at all when this test was first written -
     * see docs/STATUS.md's "round 5" - it just read as 0; the
     * "round 6 continued" exception-delivery work replaced that
     * placeholder with the real thing, see ee_raise_exception()/
     * ee_raise_tlb_exception()). Single-step (not ee_core_run()) since
     * this synthetic program has no real exception handler installed
     * at the vector address - running to a BREAK that will never come
     * would hang forever. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    wle32(prog+pc, enc_lui(4, 0x7FFF));    pc+=4; /* $a0 = 0x7FFF0000 (no TLB entry covers this) */
    wle32(prog+pc, enc_lw(6, 4, 0x0));     pc+=4; /* LW from unmapped KUSEG -> should raise TLBL */
    wle32(prog+pc, enc_break());           pc+=4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    uint32_t lw_pc = st->pc; /* BIOS_RESET_VECTOR - the LW is the very first instruction */
    ee_core_step(); /* LUI */
    ee_core_step(); /* LW - faults */
    CHECK(st->gpr[6].ud0 == 0, "KUSEG TLB miss: faulting LW did not fabricate a value (register left untouched)");
    CHECK((st->cop0[13] & 0x7Cu) == (2u << 2), "KUSEG TLB miss: Cause.ExcCode == TLBL (2)");
    CHECK((st->cop0[13] & 0x80000000u) == 0, "KUSEG TLB miss: Cause.BD == 0 (LW was not in a branch delay slot)");
    CHECK(st->cop0[14] == lw_pc + 4, "KUSEG TLB miss: EPC points at the faulting LW instruction");
    CHECK(st->cop0[8] == 0x7FFF0000u, "KUSEG TLB miss: BadVAddr == the faulting virtual address");
    CHECK((st->cop0[12] & 0x2u) != 0, "KUSEG TLB miss: Status.EXL got set");
    CHECK(st->pc == 0xBFC00200u, "KUSEG TLB miss: pc vectored to the TLB Refill handler (BEV=1 at reset, so the uncached ROM vector, offset 0)");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
