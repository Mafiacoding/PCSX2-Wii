/*
 * test_iop_hle_events.c - host-native test for Round 142's real B0-
 * table Event Control Block subsystem (source/hw/iop_hle_events.c) -
 * DeliverEvent/OpenEvent/CloseEvent/WaitEvent/TestEvent/EnableEvent/
 * DisableEvent/UnDeliverEvent. See iop_hle_events.h for the full
 * psx-spx + emumaster citation trail.
 *
 * Same technique as test_iop_hle_bios_functions.c: calls
 * iop_hle_bios_try_handle() directly with hand-set registers rather
 * than hand-encoding MIPS programs.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/iop/iop_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t call_b0(iop_state_t *st, uint32_t function,
                         uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t ra)
{
    st->gpr[9]  = function;
    st->gpr[4]  = a0;
    st->gpr[5]  = a1;
    st->gpr[6]  = a2;
    st->gpr[7]  = a3;
    st->gpr[31] = ra;
    iop_hle_bios_try_handle(st, IOP_HLE_TABLE_B0);
    return st->gpr[2];
}

int main(void)
{
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    iop_core_init(&bios);
    iop_state_t *st = iop_core_get_state();

    /* Round 141's traced boot loop: TestEvent(handle) on a never-
     * opened/never-delivered event must return 0 (this is exactly
     * this project's OLD generic-default behavior too - confirming
     * the real implementation doesn't regress the "not yet occurred"
     * case, only adds the "has occurred" case that was previously
     * unreachable no matter what). */
    uint32_t v0 = call_b0(st, IOP_HLE_B0_TEST_EVENT, 0x1234u, 0, 0, 0, 0x80010000u);
    CHECK(v0 == 0, "TestEvent on unopened handle returns 0");
    CHECK(st->pc == 0x80010000u, "TestEvent returns control to ra");

    /* Real lifecycle: OpenEvent -> handle; event starts EvStWAIT, so
     * TestEvent must still be 0 (not yet ACTIVE, let alone ALREADY). */
    uint32_t handle = call_b0(st, IOP_HLE_B0_OPEN_EVENT,
                               0xF2000010u /* class */, 0x0004u /* spec (VBLANK-style bit 2) */,
                               IOP_EVCB_MODE_NOINTR, 0, 0x80010004u);
    CHECK(handle != 0xFFFFFFFFu, "OpenEvent returns a handle");
    v0 = call_b0(st, IOP_HLE_B0_TEST_EVENT, handle, 0, 0, 0, 0x80010008u);
    CHECK(v0 == 0, "TestEvent right after OpenEvent (EvStWAIT) returns 0");

    /* EnableEvent -> EvStACTIVE; DeliverEvent(same class,spec) should
     * now transition it to EvStALREADY (cited: DeliverEvent only acts
     * on ACTIVE events); TestEvent should then see it, return 1, and
     * auto-clear back to ACTIVE (real psx-spx/emumaster semantics). */
    call_b0(st, IOP_HLE_B0_ENABLE_EVENT, handle, 0, 0, 0, 0x8001000Cu);
    call_b0(st, IOP_HLE_B0_DELIVER_EVENT, 0xF2000010u, 0x0004u, 0, 0, 0x80010010u);
    v0 = call_b0(st, IOP_HLE_B0_TEST_EVENT, handle, 0, 0, 0, 0x80010014u);
    CHECK(v0 == 1, "TestEvent sees the delivered event and returns 1");

    /* Real semantics: TestEvent auto-clears ALREADY->ACTIVE, so an
     * immediate second call (with no new DeliverEvent) must be 0
     * again - proves it doesn't just latch true forever. */
    v0 = call_b0(st, IOP_HLE_B0_TEST_EVENT, handle, 0, 0, 0, 0x80010018u);
    CHECK(v0 == 0, "TestEvent auto-clears after consuming the event");

    /* CloseEvent -> EvStUNUSED; DeliverEvent on a class/spec whose
     * event is UNUSED (not ACTIVE) must be a real no-op (cited: only
     * acts on ACTIVE) - re-delivering after close must not silently
     * resurrect the event. */
    call_b0(st, IOP_HLE_B0_CLOSE_EVENT, handle, 0, 0, 0, 0x8001001Cu);
    call_b0(st, IOP_HLE_B0_DELIVER_EVENT, 0xF2000010u, 0x0004u, 0, 0, 0x80010020u);
    v0 = call_b0(st, IOP_HLE_B0_TEST_EVENT, handle, 0, 0, 0, 0x80010024u);
    CHECK(v0 == 0, "DeliverEvent after CloseEvent is a real no-op");

    /* DisableEvent moves ACTIVE back to WAIT - re-open, enable,
     * disable, then confirm delivery no longer takes effect (matches
     * DeliverEvent's cited ACTIVE-only guard). */
    uint32_t h2 = call_b0(st, IOP_HLE_B0_OPEN_EVENT, 0xF4000001u, 0x0002u,
                           IOP_EVCB_MODE_NOINTR, 0, 0x80010028u);
    call_b0(st, IOP_HLE_B0_ENABLE_EVENT, h2, 0, 0, 0, 0x8001002Cu);
    call_b0(st, IOP_HLE_B0_DISABLE_EVENT, h2, 0, 0, 0, 0x80010030u);
    call_b0(st, IOP_HLE_B0_DELIVER_EVENT, 0xF4000001u, 0x0002u, 0, 0, 0x80010034u);
    v0 = call_b0(st, IOP_HLE_B0_TEST_EVENT, h2, 0, 0, 0, 0x80010038u);
    CHECK(v0 == 0, "DisableEvent prevents delivery from taking effect");

    /* WaitEvent's documented non-blocking simplification: forces
     * ACTIVE and returns 1 immediately (see header comment). */
    v0 = call_b0(st, IOP_HLE_B0_WAIT_EVENT, h2, 0, 0, 0, 0x8001003Cu);
    CHECK(v0 == 1, "WaitEvent (non-blocking) returns 1");
    call_b0(st, IOP_HLE_B0_DELIVER_EVENT, 0xF4000001u, 0x0002u, 0, 0, 0x80010040u);
    v0 = call_b0(st, IOP_HLE_B0_TEST_EVENT, h2, 0, 0, 0, 0x80010044u);
    CHECK(v0 == 1, "delivery takes effect again after WaitEvent re-activates");

    /* UnDeliverEvent: cited to only act on ALREADY+NOINTR, moving it
     * back to ACTIVE (i.e. "cancel this pending delivery"). Set up a
     * fresh ALREADY state, then confirm UnDeliverEvent reverts it so
     * a subsequent TestEvent sees 0 again (rather than 1, if
     * UnDeliverEvent had been a no-op). */
    uint32_t h3 = call_b0(st, IOP_HLE_B0_OPEN_EVENT, 0xF0000011u, 0x0001u,
                           IOP_EVCB_MODE_NOINTR, 0, 0x80010048u);
    call_b0(st, IOP_HLE_B0_ENABLE_EVENT, h3, 0, 0, 0, 0x8001004Cu);
    call_b0(st, IOP_HLE_B0_DELIVER_EVENT, 0xF0000011u, 0x0001u, 0, 0, 0x80010050u);
    call_b0(st, IOP_HLE_B0_UNDELIVER_EVENT, 0xF0000011u, 0x0001u, 0, 0, 0x80010054u);
    v0 = call_b0(st, IOP_HLE_B0_TEST_EVENT, h3, 0, 0, 0, 0x80010058u);
    CHECK(v0 == 0, "UnDeliverEvent reverts ALREADY back to ACTIVE before TestEvent sees it");

    printf("\n%d failures\n", failures);
    return failures ? 1 : 0;
}
