/*
 * iop_hle_bios.c - IOP BIOS syscall trap. See iop_hle_bios.h for the
 * full scope explanation (this is a generic PS1-style A0/B0/C0
 * dispatch mechanism, NOT a port of PCSX2's real, much more involved
 * IopBios.cpp - and specific function-number semantics are
 * deliberately not guessed at, with one narrow, well-evidenced
 * exception - InstallExceptionHandlers, C0h/0x07 - see below and the
 * header comment).
 */
#include "core/hw/iop_hle_bios.h"
#include "core/hw/iop_excb.h"
#include <string.h>
#include <stdio.h>

static iop_hle_bios_state_t g_hle;

/* The well-known real-hardware "general exception vector" trampoline
 * - see the header comment's InstallExceptionHandlers note. Fixed
 * shape (LUI $k0,0 / ADDIU $k0,$k0,imm / JR $k0 / NOP); only the
 * ADDIU immediate (the jump target) varies by BIOS revision, which is
 * exactly why this is located in the actual loaded ROM rather than
 * hardcoded from the public documentation's example values. */
#define EXC_TEMPLATE_WORD0 0x3C1A0000u /* LUI  $k0, 0x0000        */
#define EXC_TEMPLATE_WORD2 0x03400008u /* JR   $k0                */
#define EXC_TEMPLATE_WORD3 0x00000000u /* NOP (branch delay slot) */
#define EXC_VECTOR_ADDR    0x00000080u /* hardware-mandated general exception vector */
#define EXC_GARBAGE_MIRROR 0x00000000u /* psx-spx's documented "Garbage Area" echo   */

static uint8_t  g_exc_template[16];
static uint8_t  g_exc_template_scanned; /* 1 once we've looked, whether or not found */

void iop_hle_bios_init(void)
{
    memset(&g_hle, 0, sizeof(g_hle));
    memset(g_exc_template, 0, sizeof(g_exc_template));
    g_exc_template_scanned = 0;
    /* Round 29: B(00h) alloc_kernel_memory(size) real bump allocator -
     * starts at the documented Kernel Memory region base (psx-spx's
     * "0000E000h 2000h Kernel Memory; ExCBs, EvCBs, and TCBs allocated
     * via B(00h)"), see iop_hle_bios.h's IOP_HLE_B0_ALLOC_KERNEL_MEMORY
     * comment for the full rationale. */
    g_hle.kmem_bump_next = IOP_EXCB_ARRAY_ADDR;
}

iop_hle_bios_state_t *iop_hle_bios_get_state(void) { return &g_hle; }

static const char *table_name(uint32_t pc)
{
    switch (pc) {
        case IOP_HLE_TABLE_A0: return "A0";
        case IOP_HLE_TABLE_B0: return "B0";
        case IOP_HLE_TABLE_C0: return "C0";
        default: return "?";
    }
}

/* Little-endian 32-bit read straight out of the ROM byte buffer (the
 * BIOS image is a raw little-endian PS2 ROM dump, same convention as
 * bios_loader.c's ROMDIR parsing). */
static uint32_t rom_read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Scans the loaded BIOS ROM for the distinctive, fixed 3-of-4-words
 * signature of the real general-exception-vector trampoline (see the
 * header comment). Returns 1 and fills out[16] with the real 16 bytes
 * found (verbatim, including whatever revision-specific jump target
 * this exact dump uses) if found; returns 0 (out left untouched) if
 * not - e.g. a differently-structured BIOS revision, or no BIOS
 * loaded. Only the first match is used; in practice this signature is
 * expected to appear exactly once in a real dump (confirmed for the
 * SCPH-10000 dump used during this project's real-BIOS testing - see
 * docs/STATUS.md). */
static int find_exception_handler_template(const bios_image_t *bios, uint8_t out[16])
{
    if (!bios || !bios->data || bios->size < 16)
        return 0;

    uint32_t limit = bios->size - 16;
    for (uint32_t off = 0; off <= limit; off += 4) {
        const uint8_t *p = bios->data + off;
        if (rom_read32(p) != EXC_TEMPLATE_WORD0)
            continue;
        if (rom_read32(p + 8) != EXC_TEMPLATE_WORD2)
            continue;
        if (rom_read32(p + 12) != EXC_TEMPLATE_WORD3)
            continue;
        memcpy(out, p, 16);
        return 1;
    }
    return 0;
}

/* InstallExceptionHandlers (C0h/0x07), real behavior instead of the
 * generic default - see the header comment for the full rationale and
 * citation. Writes the real, dump-specific 16-byte trampoline to the
 * hardware exception vector (RAM address 0x80) and mirrors it to
 * address 0 (psx-spx's documented "Garbage Area" echo). Only does
 * anything if the signature was actually found in the loaded ROM;
 * otherwise this is a silent no-op and the call still gets the usual
 * generic default return value. */
static void try_install_exception_handlers(iop_state_t *st)
{
    if (!g_exc_template_scanned) {
        g_hle.exception_handler_template_found =
            (uint8_t)find_exception_handler_template(st->bios, g_exc_template);
        g_exc_template_scanned = 1;
    }

    if (!g_hle.exception_handler_template_found)
        return;

    for (int i = 0; i < 16; i += 4) {
        uint32_t word = rom_read32(g_exc_template + i);
        iop_mem_write32(st, EXC_VECTOR_ADDR + i, word);
        iop_mem_write32(st, EXC_GARBAGE_MIRROR + i, word);
    }
    g_hle.exception_handler_installed = 1;
}


/* -- Real A0-table pure-computation calls added this round -- see the
 * header comment for full scope/citation. All operate only on IOP
 * RAM (via iop_mem_read8/write8, so they're correct regardless of
 * alignment) and CPU registers ($a0-$a3 = gpr[4..7], the standard
 * MIPS o32 argument registers real calling code already uses to
 * reach these same functions) - never on any unmodeled internal BIOS
 * kernel structure. */

static uint32_t iop_strlen(iop_state_t *st, uint32_t src)
{
    uint32_t n = 0;
    while (iop_mem_read8(st, src + n) != 0)
        n++;
    return n;
}

static void iop_memcpy_bytes(iop_state_t *st, uint32_t dst, uint32_t src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        iop_mem_write8(st, dst + i, iop_mem_read8(st, src + i));
}

/* Handles the A0-table function numbers this round implements for
 * real. Returns 1 and leaves the correct value in st->gpr[2] ($v0) if
 * `function` was one of these; returns 0 (caller falls back to the
 * generic default) otherwise. */
static int try_handle_a0_real_function(iop_state_t *st, uint32_t function)
{
    uint32_t a0 = st->gpr[4], a1 = st->gpr[5], a2 = st->gpr[6];

    switch (function) {
    case IOP_HLE_A0_ABS: {
        int32_t v = (int32_t)a0;
        st->gpr[2] = (uint32_t)(v < 0 ? -v : v);
        return 1;
    }
    case IOP_HLE_A0_LABS: {
        int32_t v = (int32_t)a0;
        st->gpr[2] = (uint32_t)(v < 0 ? -v : v);
        return 1;
    }
    case IOP_HLE_A0_STRLEN:
        st->gpr[2] = iop_strlen(st, a0);
        return 1;
    case IOP_HLE_A0_STRCPY: {
        uint32_t len = iop_strlen(st, a1);
        iop_memcpy_bytes(st, a0, a1, len + 1); /* +1: include NUL terminator */
        st->gpr[2] = a0; /* standard strcpy contract: returns dst */
        return 1;
    }
    case IOP_HLE_A0_STRNCPY: {
        /* Standard C strncpy semantics: copy up to maxlen bytes from
         * src (stopping early at src's NUL), zero-pad the remainder
         * of the destination up to maxlen - and do NOT add a
         * terminator if src's length >= maxlen. */
        uint32_t maxlen = a2;
        uint32_t i = 0;
        int hit_nul = 0;
        for (; i < maxlen; i++) {
            uint8_t c = hit_nul ? 0 : iop_mem_read8(st, a1 + i);
            if (c == 0) hit_nul = 1;
            iop_mem_write8(st, a0 + i, c);
        }
        st->gpr[2] = a0;
        return 1;
    }
    case IOP_HLE_A0_STRCAT: {
        uint32_t dstlen = iop_strlen(st, a0);
        uint32_t srclen = iop_strlen(st, a1);
        iop_memcpy_bytes(st, a0 + dstlen, a1, srclen + 1);
        st->gpr[2] = a0;
        return 1;
    }
    case IOP_HLE_A0_STRNCAT: {
        uint32_t maxlen = a2;
        uint32_t dstlen = iop_strlen(st, a0);
        uint32_t i;
        for (i = 0; i < maxlen; i++) {
            uint8_t c = iop_mem_read8(st, a1 + i);
            if (c == 0) break;
            iop_mem_write8(st, a0 + dstlen + i, c);
        }
        iop_mem_write8(st, a0 + dstlen + i, 0); /* always NUL-terminates, per standard strncat */
        st->gpr[2] = a0;
        return 1;
    }
    case IOP_HLE_A0_STRCMP: {
        uint32_t i = 0;
        for (;;) {
            uint8_t c1 = iop_mem_read8(st, a0 + i);
            uint8_t c2 = iop_mem_read8(st, a1 + i);
            if (c1 != c2 || c1 == 0) {
                st->gpr[2] = (uint32_t)((int32_t)c1 - (int32_t)c2);
                break;
            }
            i++;
        }
        return 1;
    }
    case IOP_HLE_A0_STRNCMP: {
        uint32_t maxlen = a2;
        uint32_t i = 0;
        uint32_t result = 0;
        for (; i < maxlen; i++) {
            uint8_t c1 = iop_mem_read8(st, a0 + i);
            uint8_t c2 = iop_mem_read8(st, a1 + i);
            if (c1 != c2 || c1 == 0) {
                result = (uint32_t)((int32_t)c1 - (int32_t)c2);
                break;
            }
        }
        st->gpr[2] = result;
        return 1;
    }
    case IOP_HLE_A0_BCOPY:
        /* NOTE: bcopy's argument order is (src, dst, len) - reversed
         * from memcpy's (dst, src, len) - see psx-spx's A(27h) entry. */
        iop_memcpy_bytes(st, a1, a0, a2);
        return 1;
    case IOP_HLE_A0_BZERO:
        for (uint32_t i = 0; i < a1; i++)
            iop_mem_write8(st, a0 + i, 0);
        return 1;
    case IOP_HLE_A0_MEMCPY:
        iop_memcpy_bytes(st, a0, a1, a2);
        st->gpr[2] = a0; /* standard memcpy contract: returns dst */
        return 1;
    case IOP_HLE_A0_MEMSET:
        for (uint32_t i = 0; i < a2; i++)
            iop_mem_write8(st, a0 + i, (uint8_t)a1);
        st->gpr[2] = a0; /* standard memset contract: returns dst */
        return 1;
    case IOP_HLE_A0_MEMMOVE:
        /* psx-spx annotates A(2Ch) memmove as ";Bugged" on real
         * hardware - implemented here as a plain forward byte-copy
         * (NOT overlap-safe, same as memcpy) to match that documented
         * real behavior rather than silently upgrading it to correct
         * modern libc semantics no real IOP BIOS ever had. */
        iop_memcpy_bytes(st, a0, a1, a2);
        st->gpr[2] = a0;
        return 1;
    case IOP_HLE_A0_INITHEAP:
        /* Bookkeeping only, same "scaffold, not a port" caveat as
         * iop_hle_modules.c - no real allocator backs this. */
        g_hle.heap_addr = a0;
        g_hle.heap_size = a1;
        g_hle.heap_initialized = 1;
        return 1;
    case IOP_HLE_A0_FLUSHCACHE:
        /* Correct no-op: this project has no IOP cache model. */
        return 1;
    default:
        return 0;
    }
}

/* -- Round 29 continued: real B(00h)-backed bump allocator, shared by
 * the B0-table alloc_kernel_memory case and this file's own internal
 * needs (the syscall-handler trampoline + its ExCB chain node below).
 * Bounded by the same documented 0x2000-byte Kernel Memory region as
 * before; returns 0 on overflow, matching real malloc-style failure. */
static uint32_t kmem_alloc(uint32_t size)
{
    uint32_t aligned = (size + 3u) & ~3u;
    if ((uint64_t)g_hle.kmem_bump_next + aligned >
        (uint64_t)IOP_EXCB_ARRAY_ADDR + IOP_KMEM_REGION_SIZE) {
        return 0;
    }
    uint32_t addr = g_hle.kmem_bump_next;
    g_hle.kmem_bump_next += aligned;
    return addr;
}

/* Round 29 continued: real, position-independent MIPS machine code
 * implementing psx-spx's word-for-word documented SYS(01h)/SYS(02h)
 * behavior - see IOP_HLE_C0_ENQUEUESYSCALLHANDLER's header comment
 * for the full design rationale and citation trail. Hand-assembled
 * (Keystone), round-trip-verified (Capstone) against the ACTUAL,
 * live-disassembled real dispatcher/ReturnFromException code found
 * this round via the user's real SCPH-10000 dump - see
 * docs/STATUS.md's "Round 29 continued" section for the full
 * disassembly listing this was cross-checked against. Logic:
 *   1. Read Cause (cop0 r13); if ExcCode != Syscall(8), return 0 in
 *      $v0 via a plain `jr $ra` (matches the real dispatcher's own
 *      "func1 returns r2==0 -> try the next chain element" contract
 *      - live-disassembled at ROM-resident 0xe08-0xe28).
 *   2. Otherwise, re-derive the current TCB base exactly as the real
 *      dispatcher's own entry code does (`addiu $k0,zero,0x100 / lw
 *      $k0,8($k0) / lw $k0,($k0) / addi $k0,$k0,8`, byte-identical to
 *      the real code at ROM-resident 0xc90-0xca4) and read the
 *      ORIGINAL (pre-exception) $a0 back out of it at offset +0x10
 *      (where the real entry code saved it, `sw $a0,0x10($k0)` at
 *      0xd30) - this is the real SYS(nnh) function number.
 *   3. a0==1 (EnterCriticalSection): clear SR bits 2 and 10 (0x404),
 *      $v0 = 1 iff both bits were set beforehand, else 0 - exactly
 *      psx-spx's documented return-value rule.
 *      a0==2 (ExitCriticalSection): set SR bits 2 and 10; psx-spx
 *      documents no meaningful return value.
 *      Any other a0: an honest, explicitly-scoped gap (psx-spx lists
 *      further SYS(nnh) functions - e.g. ChangeThreadSubFunction -
 *      this project does not fabricate their behavior) - falls
 *      through to ReturnFromException as a safe no-op rather than
 *      risking an infinite re-entry loop.
 *   4. All three syscall-handled paths end by jumping directly to the
 *      REAL ReturnFromException entry point (0x00000f30, confirmed
 *      via live disassembly this round), matching real hardware's own
 *      documented behavior for chain elements that fully handle their
 *      exception ("the handler may execute ReturnFromException to
 *      abort further exception handling").
 * Assembled once from this source (kept here as the definitive,
 * human-readable record of what the bytes below implement):
 *
 *   mfc0  $t0, $13
 *   andi  $t0, $t0, 0x7c
 *   addiu $t1, $zero, 0x20
 *   bne   $t0, $t1, NOT_SYSCALL
 *   nop
 *   addiu $k0, $zero, 0x100
 *   lw    $k0, 8($k0)
 *   lw    $k0, 0($k0)
 *   addi  $k0, $k0, 8
 *   lw    $a0, 0x10($k0)
 *   addiu $t1, $zero, 1
 *   beq   $a0, $t1, DO_ENTER
 *   nop
 *   addiu $t1, $zero, 2
 *   beq   $a0, $t1, DO_EXIT
 *   nop
 *   b     RETURN_FROM_EXCEPTION
 *   nop
 * DO_ENTER:
 *   mfc0  $t0, $12
 *   andi  $t1, $t0, 0x404
 *   xori  $t1, $t1, 0x404
 *   sltiu $v0, $t1, 1
 *   lui   $t2, 0xFFFF
 *   ori   $t2, $t2, 0xFBFB
 *   and   $t0, $t0, $t2
 *   mtc0  $t0, $12
 *   b     RETURN_FROM_EXCEPTION
 *   nop
 * DO_EXIT:
 *   mfc0  $t0, $12
 *   ori   $t0, $t0, 0x404
 *   mtc0  $t0, $12
 *   addiu $v0, $zero, 1
 *   b     RETURN_FROM_EXCEPTION
 *   nop
 * NOT_SYSCALL:
 *   addiu $v0, $zero, 0
 *   jr    $ra
 *   nop
 * RETURN_FROM_EXCEPTION:
 *   j     0xf30
 *   nop
 */
static const uint32_t g_syscall_handler_code[] = {
    0x40086800u, 0x3108007Cu, 0x24090020u, 0x15090024u,
    0x00000000u, 0x00000000u, 0x241A0100u, 0x8F5A0008u,
    0x8F5A0000u, 0x235A0008u, 0x8F440010u, 0x24090001u,
    0x10890009u, 0x00000000u, 0x00000000u, 0x24090002u,
    0x10890010u, 0x00000000u, 0x00000000u, 0x10000018u,
    0x00000000u, 0x00000000u, 0x40086000u, 0x31090404u,
    0x39290404u, 0x2D220001u, 0x3C0AFFFFu, 0x354AFBFBu,
    0x010A4024u, 0x40886000u, 0x1000000Du, 0x00000000u,
    0x00000000u, 0x40086000u, 0x35080404u, 0x40886000u,
    0x24020001u, 0x10000006u, 0x00000000u, 0x00000000u,
    0x24020000u, 0x03E00008u, 0x00000000u, 0x00000000u,
    0x080003CCu, 0x00000000u, 0x00000000u,
};
#define SYSCALL_HANDLER_CODE_WORDS \
    (sizeof(g_syscall_handler_code) / sizeof(g_syscall_handler_code[0]))

/* Installs (once) the real trampoline above into the Kernel Memory
 * region, then enqueues a real ExCB chain node (via the already-real,
 * already-tested SysEnqIntRP mechanism from Round 22) at the given
 * priority whose "first function" pointer targets it - exactly the
 * real chain-element format psx-spx documents for C(02h)/SysEnqIntRP
 * (00h=next, 04h=second function, 08h=first function, 0Ch=unused). */
static void install_syscall_handler(iop_state_t *st, uint32_t priority)
{
    if (g_hle.syscall_handler_code_addr == 0) {
        uint32_t code_addr = kmem_alloc((uint32_t)(SYSCALL_HANDLER_CODE_WORDS * 4u));
        if (code_addr == 0)
            return; /* real hardware: Kernel Memory exhausted - honest failure, no crash */
        for (uint32_t i = 0; i < SYSCALL_HANDLER_CODE_WORDS; i++)
            iop_mem_write32(st, code_addr + i * 4u, g_syscall_handler_code[i]);
        g_hle.syscall_handler_code_addr = code_addr;
    }

    uint32_t node_addr = kmem_alloc(16u);
    if (node_addr == 0)
        return;
    iop_mem_write32(st, node_addr + 0x04u, 0u); /* no second function */
    iop_mem_write32(st, node_addr + 0x08u, g_hle.syscall_handler_code_addr);
    iop_mem_write32(st, node_addr + 0x0Cu, 0u);
    iop_excb_sys_enq_int_rp(st, priority, node_addr);
    g_hle.syscall_handler_installs++;
}

int iop_hle_bios_try_handle(iop_state_t *st, uint32_t pc)
{
    if (pc != IOP_HLE_TABLE_A0 && pc != IOP_HLE_TABLE_B0 && pc != IOP_HLE_TABLE_C0)
        return 0;

    uint32_t function = st->gpr[9];  /* $t1 - the real PS1/PS2 BIOS call convention register */
    uint32_t ra        = st->gpr[31]; /* $ra - where to return to */

    g_hle.calls_seen++;
    g_hle.last_table    = pc;
    g_hle.last_function = function;
    snprintf(g_hle.last_call_desc, sizeof(g_hle.last_call_desc),
             "%s table, function 0x%02X", table_name(pc), (unsigned int)function);

    if (pc == IOP_HLE_TABLE_C0 && function == 0x07u) {
        try_install_exception_handlers(st);
        g_hle.known_calls_handled++;
    } else if (pc == IOP_HLE_TABLE_C0 && function == IOP_HLE_C0_SYSENQINTRP) {
        /* Round 22: real SysEnqIntRP(priority, struc) - see
         * iop_excb.h. Standard MIPS calling convention: $a0=priority,
         * $a1=struc (same convention already used by this file's own
         * A0-table real functions, e.g. INITHEAP's a0/a1 reads
         * above). No return value on real hardware, matching this. */
        iop_excb_sys_enq_int_rp(st, st->gpr[4], st->gpr[5]);
        g_hle.known_calls_handled++;
    } else if (pc == IOP_HLE_TABLE_C0 && function == IOP_HLE_C0_SYSDEQINTRP) {
        /* Round 22: real (bug-preserving) SysDeqIntRP(priority, struc). */
        iop_excb_sys_deq_int_rp(st, st->gpr[4], st->gpr[5]);
        g_hle.known_calls_handled++;
    } else if (pc == IOP_HLE_TABLE_A0 &&
               (function == IOP_HLE_A0_EXIT || function == IOP_HLE_A0__EXIT)) {
        /* EXIT/_EXIT: real hardware never returns from this call -
         * halt with an honest, descriptive reason instead of
         * silently returning 0 and letting the caller run past a
         * call that was never meant to return. */
        g_hle.exited = 1;
        g_hle.exit_code = st->gpr[4]; /* $a0 = exitcode */
        g_hle.known_calls_handled++;
        st->halted = 1;
        snprintf(st->halt_reason, sizeof(st->halt_reason),
                 "IOP BIOS EXIT/_EXIT called with exitcode=%d (A0 function 0x%02X)",
                 (int)(int32_t)g_hle.exit_code, (unsigned int)function);
        st->pc      = ra;
        st->next_pc = ra + 4;
        return 1;
    } else if (pc == IOP_HLE_TABLE_A0 && try_handle_a0_real_function(st, function)) {
        g_hle.known_calls_handled++;
        st->pc      = ra;
        st->next_pc = ra + 4;
        return 1;
    } else if (pc == IOP_HLE_TABLE_B0 && function == IOP_HLE_B0_ALLOC_KERNEL_MEMORY) {
        /* Round 29: real B(00h) alloc_kernel_memory(size) - see the
         * IOP_HLE_B0_ALLOC_KERNEL_MEMORY header comment for the full
         * root-cause story (real BIOS ROM code, confirmed via live
         * Capstone disassembly, calls this via a thunk-table tail
         * call to unblock its own genuine ExCB/EvCB/TCB setup).
         * Standard MIPS calling convention: $a0=size, return address
         * in $v0 (0 on failure, matching real hardware's malloc-style
         * convention already used by this file's INITHEAP handling
         * above). A real bump allocator, 4-byte aligned (matching
         * every other word-based struct layout already handled in
         * this file), bounded by the documented 0x2000-byte Kernel
         * Memory region. */
        uint32_t size = st->gpr[4];
        g_hle.kmem_alloc_calls++;
        st->gpr[2] = kmem_alloc(size);
        if (st->gpr[2] == 0)
            g_hle.kmem_alloc_failures++;
        g_hle.known_calls_handled++;
        st->pc      = ra;
        st->next_pc = ra + 4;
        return 1;
    } else if (pc == IOP_HLE_TABLE_B0 && function == IOP_HLE_B0_RESET_ENTRY_INT) {
        /* Round 29 continued: real B(18h) ResetEntryInt() - see the
         * IOP_HLE_B0_RESET_ENTRY_INT header comment. The default
         * jmp_buf STRUCT (ra/sp/fp/s0-7/gp) is already correctly
         * resident at IOP_JMPBUF_DEFAULT_STRUCT_ADDR (confirmed via
         * live disassembly - its ra field already equals the real
         * ReturnFromException address, its sp field already equals
         * the real exception-stacktop-minus-4 value); the only thing
         * missing is the POINTER variable that tells the dispatcher's
         * post-priority-chain fallback where to find it. Real
         * hardware: "Returns the address of that structure." */
        iop_mem_write32(st, IOP_JMPBUF_DEFAULT_PTR_ADDR, IOP_JMPBUF_DEFAULT_STRUCT_ADDR);
        st->gpr[2] = IOP_JMPBUF_DEFAULT_STRUCT_ADDR;
        g_hle.reset_entry_int_calls++;
        g_hle.known_calls_handled++;
        st->pc      = ra;
        st->next_pc = ra + 4;
        return 1;
    } else if (pc == IOP_HLE_TABLE_B0 && function == IOP_HLE_B0_HOOK_ENTRY_INT) {
        /* Round 29 continued (5th finding): real B(19h) HookEntryInt(addr)
         * - see the IOP_HLE_B0_HOOK_ENTRY_INT header comment for the
         * full root-cause story (live call-tracing found the real
         * BIOS pairs this with A(13h) setjmp(buf) using the SAME
         * address, to install its own fallback resume point instead
         * of the kernel default). Standard MIPS calling convention:
         * $a0=addr. */
        uint32_t addr = st->gpr[4];
        iop_mem_write32(st, IOP_JMPBUF_DEFAULT_PTR_ADDR, addr);
        st->gpr[2] = addr;
        g_hle.hook_entry_int_calls++;
        g_hle.known_calls_handled++;
        st->pc      = ra;
        st->next_pc = ra + 4;
        return 1;
    } else if (pc == IOP_HLE_TABLE_A0 && function == IOP_HLE_A0_SETJMP) {
        /* Round 29 continued (5th finding): real A(13h) setjmp(buf) -
         * see the IOP_HLE_A0_SETJMP header comment. Saves the real
         * 12-word ra/sp/fp/s0-7/gp struct (the same layout this
         * project already reverse-engineered from the kernel's own
         * default struct at 0x00006C34) into the caller-supplied
         * buffer, then returns 0 (standard C setjmp "direct call"
         * semantics). Standard MIPS calling convention: $a0=buf. */
        uint32_t buf = st->gpr[4];
        iop_mem_write32(st, buf + IOP_JMPBUF_OFF_RA, st->gpr[31]); /* $ra */
        iop_mem_write32(st, buf + IOP_JMPBUF_OFF_SP, st->gpr[29]); /* $sp */
        iop_mem_write32(st, buf + IOP_JMPBUF_OFF_FP, st->gpr[30]); /* $fp/$s8 */
        for (int i = 0; i < 8; i++)
            iop_mem_write32(st, buf + IOP_JMPBUF_OFF_S0 + (uint32_t)i * 4u,
                             st->gpr[16 + i]); /* $s0..$s7 */
        iop_mem_write32(st, buf + IOP_JMPBUF_OFF_GP, st->gpr[28]); /* $gp */
        st->gpr[2] = 0;
        g_hle.setjmp_calls++;
        g_hle.known_calls_handled++;
        st->pc      = ra;
        st->next_pc = ra + 4;
        return 1;
    } else if (pc == IOP_HLE_TABLE_C0 && function == IOP_HLE_C0_ENQUEUESYSCALLHANDLER) {
        /* Round 29 continued: real C(01h) EnqueueSyscallHandler(priority)
         * - see the IOP_HLE_C0_ENQUEUESYSCALLHANDLER header comment
         * for the full design rationale (real, position-independent
         * MIPS trampoline implementing psx-spx's word-for-word
         * documented SYS(01h)/SYS(02h) behavior, ending at the real,
         * live-disassembled ReturnFromException address). */
        install_syscall_handler(st, st->gpr[4]);
        g_hle.known_calls_handled++;
        st->pc      = ra;
        st->next_pc = ra + 4;
        return 1;
    }

    /* No other specific function-number behavior is implemented (see
     * the header comment for why) - every other call gets a generic
     * default return value of 0 in $v0 (r2), matching real MIPS
     * calling convention for a single-word return value, and control
     * returns to the caller exactly as if a real `JR $ra` had
     * executed. */
    st->gpr[2] = 0; /* $v0 = 0 */

    st->pc      = ra;
    st->next_pc = ra + 4;

    return 1;
}
