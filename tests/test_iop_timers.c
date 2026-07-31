/*
 * test_iop_timers.c - host-native test for source/hw/iop_timers.c
 *
 * Task #214/#215 continuation (85th/86th findings): this file used
 * to test a plain register-storage stub (real counting/gating/
 * target-IRQ behavior was explicitly out of scope). That gap is now
 * closed - see iop_timers.h's extensive citation trail - so this
 * test now covers: address decoding (unchanged from before), the
 * real MODE-write masking formula ((value & 0x63FF) | 0x0400, COUNT
 * reset to 0 on any MODE write), and the real tick/target-match/
 * overflow/IRQ-raise behavior (iop_timers_tick()).
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "hw/iop_intc.c"
#include "hw/iop_timers.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

int main(void)
{
    iop_intc_init();
    iop_timers_init();

    uint32_t v;

    /* T0: COUNT 0x1F801100, MODE 0x1F801104, TARGET 0x1F801108.
     * Write TARGET and MODE first, THEN COUNT last, since a real
     * MODE write always resets COUNT to 0 (see iop_timers.c's write
     * handler) - writing COUNT last avoids that reset clobbering the
     * value this test is trying to check. */
    iop_timers_mmio_write32(0x1F801108u, 0x33333333u);
    iop_timers_mmio_write32(0x1F801104u, 0x22222222u);
    iop_timers_mmio_write32(0x1F801100u, 0x11111111u);
    iop_timers_mmio_read32(0x1F801100u, &v); CHECK(v == 0x11111111u, "T0 COUNT roundtrip (written after MODE)");
    iop_timers_mmio_read32(0x1F801104u, &v);
    CHECK(v == ((0x22222222u & 0x63FFu) | IOP_CNT_MODE_INTR_ENABLE), "T0 MODE roundtrip (real write-mask formula)");
    iop_timers_mmio_read32(0x1F801108u, &v); CHECK(v == 0x33333333u, "T0 TARGET roundtrip");

    /* Real behavior: a MODE write always resets COUNT to 0. */
    iop_timers_mmio_write32(0x1F801100u, 0x55555555u);
    iop_timers_mmio_write32(0x1F801104u, 0x00000000u);
    iop_timers_mmio_read32(0x1F801100u, &v);
    CHECK(v == 0u, "T0 COUNT resets to 0 on MODE write (real hardware behavior)");

    /* T1 must be unaffected by T0's writes (address decoding isolation) */
    iop_timers_mmio_read32(0x1F801110u, &v);
    CHECK(v == 0u, "T1 COUNT unaffected by T0's writes");

    /* T3 lives in the second address window (0x1F801480) - confirm
     * both windows work independently */
    iop_timers_mmio_write32(0x1F801488u, 0xBBBBBBBBu);
    iop_timers_mmio_write32(0x1F801484u, 0x00000000u); /* MODE=0 keeps count untouched-by-write below */
    iop_timers_mmio_write32(0x1F801480u, 0xAAAAAAAAu);
    iop_timers_mmio_read32(0x1F801480u, &v); CHECK(v == 0xAAAAAAAAu, "T3 COUNT roundtrip (second address window)");
    iop_timers_mmio_read32(0x1F801488u, &v); CHECK(v == 0xBBBBBBBBu, "T3 TARGET roundtrip (second address window)");

    /* T5, the last counter (0x1F8014A0) */
    iop_timers_mmio_write32(0x1F8014A4u, 0xCCCCCCCCu);
    iop_timers_mmio_read32(0x1F8014A4u, &v);
    CHECK(v == ((0xCCCCCCCCu & 0x63FFu) | IOP_CNT_MODE_INTR_ENABLE), "T5 MODE roundtrip (real write-mask formula)");

    /* An address inside T0's 12-byte window that ISN'T one of the 3
     * known register offsets (e.g. base+0x02) must not be silently
     * treated as COUNT/MODE/TARGET. */
    CHECK(iop_timers_mmio_read32(0x1F801102u, &v) == 0,
          "address inside T0's window but not a known register offset: not claimed on read");
    CHECK(iop_timers_mmio_write32(0x1F801102u, 0x1u) == 0,
          "address inside T0's window but not a known register offset: not claimed on write");

    /* An address between T2 and T3 (the gap between the two hardware
     * windows) must not be claimed either. */
    CHECK(iop_timers_mmio_read32(0x1F801200u, &v) == 0,
          "address in the gap between the T0-T2 and T3-T5 windows: not claimed");

    /* --- Real tick/IRQ behavior (task #214/#215) --- */

    /* Overflow, one-shot: T0 (16-bit counter, overflow cap 0x10000),
     * overflIntr enabled, repeatIntr NOT set. */
    iop_timers_init();
    iop_intc_init();
    iop_timers_mmio_write32(0x1F801108u, 0xFFFFu); /* TARGET far above where we'll stop, so only overflow fires */
    iop_timers_mmio_write32(0x1F801104u, IOP_CNT_MODE_OVERFL_INTR); /* overflIntr=1, targetIntr=0, repeatIntr=0 */
    iop_timers_mmio_write32(0x1F801100u, 0xFFFEu); /* COUNT one tick away from the 0x10000 overflow cap */
    iop_timers_tick(); /* count -> 0xFFFF, no overflow yet (cap check is count > 0xFFFF) */
    iop_intc_state_t *intc = iop_intc_get_state();
    CHECK((intc->istat & (1u << 4)) == 0, "T0 overflow IRQ (bit4) not yet raised before crossing the cap");
    iop_timers_tick(); /* count -> 0x10000, now > 0xFFFF: real overflow */
    CHECK((intc->istat & (1u << 4)) != 0, "T0 overflow IRQ (bit4) raised exactly at the real 0x10000 cap");
    iop_timers_mmio_read32(0x1F801100u, &v);
    CHECK(v == 0u, "T0 COUNT wraps to 0 after a real overflow (0x10000 - (0xFFFF+1) = 0)");

    /* One-shot semantics: intrEnable should now be cleared (no repeat
     * mode), so ticking further past another cycle must NOT re-raise
     * the same IRQ bit until MODE is rewritten. */
    intc->istat = 0; /* acknowledge, as real IOP code would via I_STAT's write-0-to-clear */
    for (int i = 0; i < 0x10001; i++) iop_timers_tick(); /* drive it through a full second cycle */
    CHECK((intc->istat & (1u << 4)) == 0, "T0 one-shot overflow IRQ does not re-fire after MODE hasn't been rewritten");

    /* Periodic (zeroReturn + repeatIntr + targetIntr) target-match
     * IRQ - the real common OS-tick-timer pattern - T1 (bit5). */
    iop_timers_init();
    iop_intc_init();
    intc = iop_intc_get_state();
    iop_timers_mmio_write32(0x1F801118u, 100u); /* T1 TARGET = 100 */
    iop_timers_mmio_write32(0x1F801114u,
        IOP_CNT_MODE_TARGET_INTR | IOP_CNT_MODE_REPEAT_INTR | IOP_CNT_MODE_ZERO_RETURN);
    int fires = 0;
    for (int i = 0; i < 1000; i++) {
        iop_timers_tick();
        if (intc->istat & (1u << 5)) {
            fires++;
            intc->istat &= ~(1u << 5); /* acknowledge (real write-0-to-clear semantics) */
        }
    }
    CHECK(fires == 10, "T1 periodic zeroReturn+repeat target IRQ fires exactly every 100 ticks over 1000 ticks (10 times)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
