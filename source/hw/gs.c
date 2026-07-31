/*
 * gs.c - GS privileged register skeleton. See include/core/hw/gs.h
 * for scope notes. Addresses cross-checked against PCSX2's
 * pcsx2/Hw.h (GS_PMODE .. GS_SIGLBLID).
 */

#include "core/hw/gs.h"
#include <string.h>
#include <stddef.h>

static gs_state_t g_gs;

/* Round 126 (task #172/#247, 166th finding): GS_IMR real reset value.
 * Real PS2 hardware resets GS_IMR with ALL interrupt-source mask bits
 * SET (masked/disabled) - well-established real hardware behavior
 * (every real GS driver/BIOS must explicitly unmask a source before
 * using it; this is the same "start masked, software opts in" pattern
 * this project already fixed on the IOP side for I_MASK/EnableIntr,
 * see docs/STATUS.md 88th/89th findings). This project models GS_CSR/
 * GS_IMR's five real interrupt sources (SIGNAL/FINISH/HSYNC/VSYNC/
 * EDWRITE) at bit positions 0-4 (this project's own established
 * simplified convention - see ee_core.c's GS_CSR_VSYNC_BIT/
 * GS_IMR_VSMSK_BIT comment, Round 87/127th finding), rather than real
 * hardware's actual bit 8-12 positions, for internal consistency with
 * the rest of this codebase's GS_CSR/GS_IMR handling.
 *
 * Found via task #247's own deep live-hardware-comparison investigation
 * (163rd-166th findings): gs_init()'s previous plain memset-to-zero
 * left GS_IMR fully UNMASKED from the very first instruction of boot,
 * causing ee_check_gs_vsync() (Round 87) to raise the real EE_INTC
 * GS interrupt (cause 0) on the very first frame - before any real
 * kernel software has registered a handler for it. The resulting
 * jalr through that still-unregistered (legitimately empty at this
 * point, confirmed identical on real hardware via live memory read)
 * per-cause dispatch table slot dereferences a null function pointer,
 * causing the null-pointer/PC-escapes-to-zero cascade that is task
 * #247's whole Status.EXL=1 lockup (163rd/164th/165th findings) -
 * traced end-to-end this round via live disassembly of the real
 * kernel's interrupt dispatch trampoline (0x80002840-0x80002878,
 * 0x00081FE0 thread-entry trampoline) plus this project's own
 * instrumented boot trace, cross-referenced against a live real GT3
 * session's memory (confirming the dispatch table's cause-0 slot is
 * legitimately empty on real hardware too - the bug isn't the empty
 * slot, it's firing the interrupt before anything could have
 * registered a handler for it). */
#define GS_IMR_RESET_ALL_MASKED 0x1Full /* bits 0-4: SIGNAL/FINISH/HSYNC/VSYNC/EDWRITE, all masked */

void gs_init(void)
{
    memset(&g_gs, 0, sizeof(g_gs));
    g_gs.imr = GS_IMR_RESET_ALL_MASKED;
}
gs_state_t *gs_get_state(void) { return &g_gs; }

typedef struct { uint32_t addr; uint64_t *reg; } gs_reg_map_t;

static uint64_t *reg_for_addr(uint32_t addr)
{
    static const struct { uint32_t addr; size_t offset; } map[] = {
        { 0x12000000u, offsetof(gs_state_t, pmode) },
        { 0x12000010u, offsetof(gs_state_t, smode1) },
        { 0x12000020u, offsetof(gs_state_t, smode2) },
        { 0x12000030u, offsetof(gs_state_t, srfsh) },
        { 0x12000040u, offsetof(gs_state_t, synch1) },
        { 0x12000050u, offsetof(gs_state_t, synch2) },
        { 0x12000060u, offsetof(gs_state_t, syncv) },
        { 0x12000070u, offsetof(gs_state_t, dispfb1) },
        { 0x12000080u, offsetof(gs_state_t, display1) },
        { 0x12000090u, offsetof(gs_state_t, dispfb2) },
        { 0x120000A0u, offsetof(gs_state_t, display2) },
        { 0x120000B0u, offsetof(gs_state_t, extbuf) },
        { 0x120000C0u, offsetof(gs_state_t, extdata) },
        { 0x120000D0u, offsetof(gs_state_t, extwrite) },
        { 0x120000E0u, offsetof(gs_state_t, bgcolor) },
        { 0x12001000u, offsetof(gs_state_t, csr) },
        { 0x12001010u, offsetof(gs_state_t, imr) },
        { 0x12001040u, offsetof(gs_state_t, busdir) },
        { 0x12001080u, offsetof(gs_state_t, siglblid) },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].addr == addr)
            return (uint64_t *)((uint8_t *)&g_gs + map[i].offset);
    }
    return NULL;
}

int gs_mmio_read64(uint32_t addr, uint64_t *out_val)
{
    if (addr < 0x12000000u || addr > 0x12001FFFu)
        return 0;
    uint64_t *reg = reg_for_addr(addr);
    *out_val = reg ? *reg : 0;
    return 1;
}

int gs_mmio_write64(uint32_t addr, uint64_t val)
{
    if (addr < 0x12000000u || addr > 0x12001FFFu)
        return 0;
    uint64_t *reg = reg_for_addr(addr);
    if (reg) {
        if (addr == 0x12001000u) {
            /* GS_CSR write-1-to-clear semantics for the interrupt
             * status bits (real hardware clears bits where the
             * written value has a 1, rather than a plain store) -
             * approximated here since we don't generate real GS
             * interrupts yet, but recorded faithfully so debug reads
             * of CSR aren't nonsensical. GS_IMR (0x12001010) and
             * everything else are plain read/write registers. */
            g_gs.csr &= ~val;
        } else {
            *reg = val;
        }
    }
    return 1;
}
