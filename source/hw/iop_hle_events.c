#include <string.h>
#include "core/hw/iop_hle_events.h"

/* See iop_hle_events.h for full citation trail (psx-spx function
 * names/numbers + EvCB memory-table existence, emumaster's real,
 * working, open-source HLE for the exact EvSt/EvMd bit values and
 * the class/spec-to-index hashing algorithm this project's own
 * OpenEvent/DeliverEvent/etc. below re-derive clean-room). */

typedef struct {
    uint32_t status;
    uint32_t mode;
    uint32_t fhandler;
} iop_evcb_t;

static iop_evcb_t g_evcb[IOP_EVCB_EV_COUNT][IOP_EVCB_SPEC_COUNT];

void iop_hle_events_init(void)
{
    memset(g_evcb, 0, sizeof(g_evcb)); /* status=IOP_EVCB_STATUS_UNUSED (0) for all */
}

/* Cited (emumaster bios.cpp `GetEv()`): maps a real 32-bit event
 * class code (e.g. 0xF2000010-style values seen in real PS1/PS2
 * kernel usage) down to a compact 0..511 table index. */
static int hash_ev(uint32_t class_code)
{
    int ev = (int)((class_code >> 24) & 0xFu);
    if (ev == 0xF) ev = 5;
    ev = ev * 32 + (int)(class_code & 0x1Fu);
    return ev;
}

/* Cited (emumaster bios.cpp `GetSpec()`): maps a real spec bitmask
 * (e.g. 0x0004 for VBLANK) down to a compact 0..17 table index -
 * 0x0301/0x0302 are special-cased to 16/17, otherwise the lowest set
 * bit position (0-15) of the bitmask is used. */
static int hash_spec(uint32_t spec_mask)
{
    if (spec_mask == 0x0301u) return 16;
    if (spec_mask == 0x0302u) return 17;
    for (int i = 0; i < 16; i++)
        if (spec_mask & (1u << i)) return i;
    return 0;
}

static int clamp_ev(int ev)   { return (ev < 0 || ev >= IOP_EVCB_EV_COUNT)   ? 0 : ev; }
static int clamp_spec(int sp) { return (sp < 0 || sp >= IOP_EVCB_SPEC_COUNT) ? 0 : sp; }

/* Cited (emumaster bios.cpp `DeliverEvent()` inline helper): only
 * acts if the target EvCB is currently ACTIVE; the interrupt-mode
 * (EvMdINTR) branch calls the registered handler function directly
 * on real hardware - this project has no softcall/re-entrant-call
 * mechanism into IOP code from HLE (see iop_module_loader.h's own
 * "trampoline, not real execution" scope notes for the same class of
 * limitation), so that branch is a documented no-op here; the
 * EvMdNOINTR branch (the one this project's traced boot loop
 * actually depends on) is implemented for real. */
static void deliver_ev_spec(int ev, int spec)
{
    ev = clamp_ev(ev);
    spec = clamp_spec(spec);
    iop_evcb_t *e = &g_evcb[ev][spec];
    if (e->status != IOP_EVCB_STATUS_ACTIVE)
        return;
    if (e->mode == IOP_EVCB_MODE_INTR) {
        /* Not modeled - see header comment. */
        return;
    }
    e->status = IOP_EVCB_STATUS_ALREADY;
}

void iop_hle_event_deliver(iop_state_t *st)
{
    int ev = hash_ev(st->gpr[4]);
    int spec = hash_spec(st->gpr[5]);
    deliver_ev_spec(ev, spec);
    st->gpr[2] = 0;
}

void iop_hle_event_open(iop_state_t *st)
{
    int ev = clamp_ev(hash_ev(st->gpr[4]));
    int spec = clamp_spec(hash_spec(st->gpr[5]));
    iop_evcb_t *e = &g_evcb[ev][spec];
    e->status = IOP_EVCB_STATUS_WAIT;
    e->mode = st->gpr[6];
    e->fhandler = st->gpr[7];
    st->gpr[2] = (uint32_t)ev | ((uint32_t)spec << 8);
}

void iop_hle_event_close(iop_state_t *st)
{
    int ev = clamp_ev((int)(st->gpr[4] & 0xFFu));
    int spec = clamp_spec((int)((st->gpr[4] >> 8) & 0xFFu));
    g_evcb[ev][spec].status = IOP_EVCB_STATUS_UNUSED;
    st->gpr[2] = 1;
}

void iop_hle_event_wait(iop_state_t *st)
{
    /* Non-blocking simplification - see header comment. */
    int ev = clamp_ev((int)(st->gpr[4] & 0xFFu));
    int spec = clamp_spec((int)((st->gpr[4] >> 8) & 0xFFu));
    g_evcb[ev][spec].status = IOP_EVCB_STATUS_ACTIVE;
    st->gpr[2] = 1;
}

void iop_hle_event_test(iop_state_t *st)
{
    int ev = clamp_ev((int)(st->gpr[4] & 0xFFu));
    int spec = clamp_spec((int)((st->gpr[4] >> 8) & 0xFFu));
    iop_evcb_t *e = &g_evcb[ev][spec];
    if (e->status == IOP_EVCB_STATUS_ALREADY) {
        e->status = IOP_EVCB_STATUS_ACTIVE;
        st->gpr[2] = 1;
    } else {
        st->gpr[2] = 0;
    }
}

void iop_hle_event_enable(iop_state_t *st)
{
    int ev = clamp_ev((int)(st->gpr[4] & 0xFFu));
    int spec = clamp_spec((int)((st->gpr[4] >> 8) & 0xFFu));
    g_evcb[ev][spec].status = IOP_EVCB_STATUS_ACTIVE;
    st->gpr[2] = 1;
}

void iop_hle_event_disable(iop_state_t *st)
{
    int ev = clamp_ev((int)(st->gpr[4] & 0xFFu));
    int spec = clamp_spec((int)((st->gpr[4] >> 8) & 0xFFu));
    g_evcb[ev][spec].status = IOP_EVCB_STATUS_WAIT;
    st->gpr[2] = 1;
}

void iop_hle_event_undeliver(iop_state_t *st)
{
    int ev = hash_ev(st->gpr[4]);
    int spec = hash_spec(st->gpr[5]);
    ev = clamp_ev(ev);
    spec = clamp_spec(spec);
    iop_evcb_t *e = &g_evcb[ev][spec];
    if (e->status == IOP_EVCB_STATUS_ALREADY && e->mode == IOP_EVCB_MODE_NOINTR)
        e->status = IOP_EVCB_STATUS_ACTIVE;
    st->gpr[2] = 0;
}
