/*
 * system.c - interleaved EE/IOP scheduler. See system.h for the
 * rationale and current known simplifications (EE_IOP_STEP_RATIO
 * instructions per slice on the EE side per 1 on the IOP side - a
 * ratio-aware, but still not cycle-accurate, approximation of real
 * hardware's ~8:1 clock difference; see system.h).
 */
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/gs.h"
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

/* Round 448 (task #247 continued): real, confirmed root cause of the
 * checkpoint/resume SIGSEGV that iop_heap.c's fix and the SIF-bridge
 * re-bind did not resolve on their own. Bisected via a host-native
 * backtrace (driver_r313.c's own crash handler, once its earlier
 * secondary-fault issue was worked around by testing smaller slice
 * counts): the fault is INSIDE __printf_chk, called from this file's
 * plain printf() calls below - confirming driver_r313.c's own long-
 * standing, previously-unexplained comment ("stdio reliably crashes
 * on any call made after this checkpoint format's raw restore, for
 * reasons not fully root-caused" - see driver_r313.c's r313_safe_
 * printf()) applies here too. r313_safe_printf() already worked
 * around this in the test driver by formatting via vsnprintf() into
 * a stack buffer and writing it with a raw write() syscall, entirely
 * bypassing FILE-stream/PLT-mediated stdio machinery - this is the
 * exact same technique, ported into system.c itself since
 * system_run_interleaved() is real, shared source (used by main.c on
 * the actual Wii/Dolphin target too, not just the test driver) and
 * its diagnostic prints are worth keeping working everywhere, not
 * just deleting them. This does not change any real Wii/Dolphin
 * behavior - the printed bytes are identical, only the mechanism
 * that emits them changed. */
static void system_safe_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf)) n = sizeof(buf);
        ssize_t w = write(1, buf, (size_t)n);
        (void)w;
    }
}

/* Real EE clock (~294.912 MHz) vs real IOP clock (~36.864 MHz) is
 * roughly 8:1 - already documented as this project's target ratio in
 * system.h/docs/ROADMAP.md before this was implemented. This does NOT
 * make the scheduler cycle-accurate (different MIPS instructions take
 * different real cycle counts on both cores, none of which is
 * modeled) - it just steps the EE 8 real instructions for every 1 IOP
 * instruction per slice, instead of the previous 1:1, so a given wall-
 * clock-equivalent slice count gives each core roughly the right
 * SHARE of total instructions executed. An honest, noted
 * approximation, not a claim of real timing fidelity. */
#define EE_IOP_STEP_RATIO 8

/* Task #172 continued: adapts iop_mem_write8()'s real signature to
 * the generic (void *ctx, addr, val) shape ee_core.c's optional SIF
 * DMA-copy bridge expects - see ee_core.h's ee_core_set_iop_write8_
 * bridge() comment for why this indirection exists (keeping ee_core.c
 * free of a hard link-time dependency on iop_core.c). */
static void system_iop_write8_adapter(void *ctx, uint32_t addr, uint8_t val)
{
    iop_mem_write8((iop_state_t *)ctx, addr, val);
}

int system_init(const bios_image_t *ee_bios, const bios_image_t *iop_bios)
{
    if (ee_core_init(ee_bios) != 0) {
        printf("[!] system_init: EE core init failed\n");
        return -1;
    }
    if (iop_core_init(iop_bios) != 0) {
        printf("[!] system_init: IOP core init failed\n");
        return -1;
    }
    ee_core_set_iop_write8_bridge(iop_core_get_state(), system_iop_write8_adapter);
    return 0;
}

/* Round 448 (task #247 continued): see system.h's citation on why
 * this exists separately from system_init(). */
void system_rebind_iop_bridge(void)
{
    ee_core_set_iop_write8_bridge(iop_core_get_state(), system_iop_write8_adapter);
}

/* Round 940 (task #925, explicit user directive - "Schritt 3: Die
 * ultimative Fallback - Die PMODE/DISP2 Gewaltsimulation"):
 * DELIBERATE SYNTHETIC DIAGNOSTIC OVERRIDE, NOT REAL HARDWARE
 * MODELING OR A CLAIM THAT THE REAL BIOS EVER DOES THIS. Round 939
 * showed PMODE staying 0x00 across an 8.24-BILLION-instruction
 * diskless boot (converged idle steady-state, not slow organic
 * progress). Round 940's fresh BIOS disassembly additionally found
 * that even with Schritt 1's SIF_SMFLAG bit-30 hook forcing the
 * dispatcher's first gate, a second independent gate
 * (*(0x80023EF8) != 0, real EE RAM content) still blocks the real
 * SIF2 dispatch path in every observed checkpoint - so the real
 * BIOS is not expected to configure PMODE/DISPLAY2 itself within any
 * reasonable instruction budget on the current tree. Per the user's
 * explicit instruction ("Wenn das BIOS PMODE nicht anfasst, tun wir
 * es im Emulator-Code selbst"), once the EE has executed more than
 * SIF_R940_FORCE_DISPLAY_THRESHOLD instructions AND PMODE is still
 * unconfigured (0), we hard-write PMODE=0x03 (circuits 1+2 enabled),
 * SMODE2=0x3 (interlace+frame-mode bits set) and standard/plausible
 * NTSC 640x448 values into DISPFB2/DISPLAY2, purely so libogc's own
 * display-open logic on real Wii/Dolphin has a nonzero PMODE to
 * react to and SOMETHING (BIOS/OSDSYS content, garbage, or a flat
 * color field) reaches the screen - any visible pixel is more debug
 * data than more billions of instructions of confirmed black screen.
 * This only fires ONCE, and never overwrites a PMODE the real BIOS
 * configured on its own (checked immediately before writing) - if
 * the SIF hooks above (or a future real fix) ever let the BIOS reach
 * its own real SetGsCrt/PMODE write first, this block is a silent
 * no-op forever after. Left unconditional/always-compiled (no build
 * flag) per the user's explicit "erzwingen" (force) directive - see
 * Round 940 STATUS.md for the full before/after evidence and the
 * exact register-value derivation (DX=636 DY=50 MAGH=0 MAGV=0
 * DW=639 DH=447, the standard ps2sdk/PCSX2 640x448 NTSC layout). */
#define SIF_R940_FORCE_DISPLAY_THRESHOLD 25000000ull

static void system_r940_force_display_if_needed(ee_state_t *ee)
{
    static int forced_once = 0;
    if (forced_once)
        return;
    if (ee->instructions_executed < SIF_R940_FORCE_DISPLAY_THRESHOLD)
        return;

    gs_state_t *gs = gs_get_state();
    forced_once = 1; /* only ever attempt this once, regardless of outcome */
    if (gs->pmode != 0)
        return; /* real BIOS already configured display itself - do not stomp it */

    /* Round 941 (task #925 continued, real-hardware evidence from the
     * user's own Dolphin run of the Round 940 JIT-on build): the
     * debug HUD correctly reported "configured by BIOS/game" and EE
     * instructions were well past this threshold, yet Dolphin's own
     * D3D12 stats showed Draw calls: 0 and the screen stayed black.
     * Root cause, found by reading main.c's real blit call site
     * (run_real_boot_flow(), ~line 509): `int en1 = pmode & 0x1;` /
     * `active_dispfb = en1 ? gs->dispfb1 : gs->dispfb2;` - EN1 is
     * PREFERRED over EN2 whenever both are set (an intentional,
     * real-hardware-accurate choice from Round 212's own fix, cited
     * right above that code: "EN1 preferred if both are somehow set,
     * matching real hardware's Circuit-1-is-primary convention").
     * Round 940 set PMODE=0x03, enabling BOTH EN1 and EN2, but only
     * ever wrote DISPFB2/DISPLAY2 (Circuit 2) - DISPFB1/DISPLAY1 were
     * left at their real, never-configured value of 0. So main.c's
     * circuit-selection logic picked Circuit 1 (EN1 set) every time,
     * decoded DISPFB1=0 via gs_decode_dispfb() into bw_pixels=0 (see
     * gs_wii_output.c: fbw_field = (dispfb>>9)&0x3F, 0 when dispfb is
     * 0), and main.c's `if (bw_pixels > 0)` guard silently skipped
     * the entire gs_blit_psmct32_to_xfb() call - GS memory was never
     * blitted to the screen at all, regardless of what Schritt 3
     * wrote into Circuit 2. This fully explains the user's exact
     * symptom (HUD says "configured", draw calls/pixels are zero)
     * without needing to touch gs_wii_output.c or invent any new
     * hypothesis. Fix: force PMODE=0x02 (EN2 only, EN1 left 0) -
     * this is not a new guess, it is the exact real-hardware
     * convention this project already confirmed and documented in
     * Round 212 (real PCSX2 debugger screenshot of the GT3 BIOS
     * splash showing PMODE=0x66, EN1=0/EN2=1, DISPFB2 populated,
     * DISPFB1 zero) - so this fix makes the synthetic override match
     * the one real-hardware PMODE pattern this project has direct
     * evidence for, instead of an arbitrary "enable both" guess. */
    gs->pmode    = 0x02u;             /* enable GS circuit 2 ONLY (matches Round 212's real-hardware-confirmed EN1=0/EN2=1 pattern) */
    gs->smode2   = 0x3u;               /* INT=1 (interlace), FFMD=1 (frame mode) */
    gs->dispfb2  = 0x1400u;            /* FBP=0, FBW=10 (640/64), PSM=0 (PSMCT32) */
    gs->display2 = 0x001bf27f0003227cull; /* DX=636 DY=50 MAGH=0 MAGV=0 DW=639 DH=447 */
    system_safe_printf("\n[R941-FORCE] instr=%llu: BIOS never configured PMODE - "
           "forcing PMODE=0x02 (Circuit2-only, Round 941 fix)/SMODE2=0x3/DISPFB2=0x%04x/DISPLAY2=0x%016llx "
           "(Round 940/941 synthetic diagnostic override, NOT real hardware fidelity)\n",
           (unsigned long long)ee->instructions_executed, (unsigned)gs->dispfb2,
           (unsigned long long)gs->display2);
}

int system_run_interleaved(uint64_t max_slices)
{
    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    uint64_t slice = 0;
    for (;;) {
        for (int i = 0; i < EE_IOP_STEP_RATIO; i++) {
            if (!ee->halted)
                ee_core_step();
        }
        if (!iop->halted)
            iop_core_step();

        system_r940_force_display_if_needed(ee);

        if (ee->halted && iop->halted) {
            system_safe_printf("\n[+] system_run_interleaved: both cores halted after %llu slice(s)\n",
                   (unsigned long long)slice);
            system_safe_printf("    EE  halted at pc=0x%08lX after %llu instructions: %s\n",
                   (unsigned long)ee->pc, (unsigned long long)ee->instructions_executed,
                   ee->halt_reason[0] ? ee->halt_reason : "(unknown)");
            system_safe_printf("    IOP halted at pc=0x%08lX after %llu instructions: %s\n",
                   (unsigned long)iop->pc, (unsigned long long)iop->instructions_executed,
                   iop->halt_reason[0] ? iop->halt_reason : "(unknown)");
            return 1;
        }

        slice++;
        if (max_slices != 0 && slice >= max_slices) {
            system_safe_printf("\n[!] system_run_interleaved: hit slice cap (%llu) before both cores halted\n",
                   (unsigned long long)max_slices);
            system_safe_printf("    EE  halted=%d pc=0x%08lX\n", ee->halted, (unsigned long)ee->pc);
            system_safe_printf("    IOP halted=%d pc=0x%08lX\n", iop->halted, (unsigned long)iop->pc);
            return 0;
        }
    }
}
