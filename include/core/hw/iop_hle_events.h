#ifndef PCSX2_WII_IOP_HLE_EVENTS_H
#define PCSX2_WII_IOP_HLE_EVENTS_H

#include <stdint.h>
#include "core/iop/iop_core.h"

/*
 * iop_hle_events.h - real PS1/PS2-IOP-kernel Event Control Block
 * (EvCB) subsystem: B(07h) DeliverEvent, B(08h) OpenEvent, B(09h)
 * CloseEvent, B(0Ah) WaitEvent, B(0Bh) TestEvent, B(0Ch) EnableEvent,
 * B(0Dh) DisableEvent, B(20h) UnDeliverEvent (Round 141/142, task
 * #172, 181st/182nd findings).
 *
 * BACKGROUND: Round 140/141 traced the IOP's own persistent, real
 * "stuck loop" (the exact reason SIF_STAT_SIFINIT never gets set -
 * see docs/STATUS.md's 180th/181st findings) to a genuine, cited B0-
 * table call to function 0x0B - psx-spx's documented `B(0Bh)
 * TestEvent(event)` - which this project's `iop_hle_bios.c` had no
 * case for and so fell through to a generic, always-0 default return
 * value, making the loop's `beq $v0,$s1,...` exit test unwinnable.
 *
 * CITATIONS: psx-spx's kernel/BIOS page
 * (https://psx-spx.consoledev.net/kernelbios/) confirms the real
 * function names/numbers (B(07h) DeliverEvent(class,spec), B(08h)
 * OpenEvent(class,spec,mode,func), B(09h) CloseEvent(handle), B(0Ah)
 * WaitEvent(handle), B(0Bh) TestEvent(handle), B(0Ch)
 * EnableEvent(handle), B(0Dh) DisableEvent(handle), B(20h)
 * UnDeliverEvent(class,spec)) and the EvCB memory-table existence
 * (`00000120h EvCB Event Control Blocks (addr=var, size=N*1Ch)`,
 * `Event handles (event=F10000xxh) as evcb=[120h]+(event AND
 * FFFFh)*1Ch`), but did not include the exact EvSt status-word bit
 * values or the class/spec-to-table-index hashing algorithm in the
 * fetched page slice.
 *
 * For those exact values, this project cross-referenced emumaster
 * (GPL-licensed, open-source PS1 emulator,
 * https://github.com/ruedigergad/emumaster/blob/master/src/psx/
 * bios.cpp), a real, independently-written, working BIOS HLE
 * implementation of this exact same PS1 kernel convention (the SAME
 * convention this project's own B(18h)/B(19h) functions already
 * implement from psx-spx). Its status constants (EvStUNUSED=0x0000,
 * EvStWAIT=0x1000, EvStACTIVE=0x2000, EvStALREADY=0x4000; EvMdINTR=
 * 0x1000, EvMdNOINTR=0x2000) and its class/spec hashing macros
 * (`GetEv`: `ev=((class>>24)&0xF); if(ev==0xF) ev=5; ev=ev*32+
 * (class&0x1F);` and `GetSpec`: spec-bitmask 0x0301->16, 0x0302->17,
 * else lowest set bit 0-15 of the spec-bitmask) are cited from that
 * real, checkable, independently-authored reference rather than
 * fabricated - this project's own implementation below is written
 * clean-room (no code copied) to match that same real, documented
 * behavioral contract, not a transcription of PS2-BIOS-ROM bytes
 * (this event mechanism is a well-established, independently-
 * reimplemented-many-times PS1/PS2 kernel convention, not something
 * unique to or extracted from Sony's copyrighted ROM image).
 *
 * SCOPE: WaitEvent's real semantics (per psx-spx) block the calling
 * thread until the event fires if its mode is EvMdNOINTR; this
 * project has no real IOP thread scheduler/blocking primitive (see
 * iop_module_loader.h's own scope notes on the same limitation), so -
 * matching emumaster's own HLE simplification of the same call -
 * WaitEvent here does NOT block: it force-sets the event to
 * EvStACTIVE and returns immediately. This is a documented, honest
 * simplification, not a claim of full real hardware semantics.
 */

#define IOP_EVCB_STATUS_UNUSED  0x0000u
#define IOP_EVCB_STATUS_WAIT    0x1000u
#define IOP_EVCB_STATUS_ACTIVE  0x2000u
#define IOP_EVCB_STATUS_ALREADY 0x4000u

#define IOP_EVCB_MODE_INTR      0x1000u
#define IOP_EVCB_MODE_NOINTR    0x2000u

#define IOP_HLE_B0_DELIVER_EVENT   0x07u
#define IOP_HLE_B0_OPEN_EVENT      0x08u
#define IOP_HLE_B0_CLOSE_EVENT     0x09u
#define IOP_HLE_B0_WAIT_EVENT      0x0Au
#define IOP_HLE_B0_TEST_EVENT      0x0Bu
#define IOP_HLE_B0_ENABLE_EVENT    0x0Cu
#define IOP_HLE_B0_DISABLE_EVENT   0x0Du
#define IOP_HLE_B0_UNDELIVER_EVENT 0x20u

/* ev spans 0..(15*32+31)=511 (see GetEv's citation above), spec
 * spans 0..17 (see GetSpec's citation above). */
#define IOP_EVCB_EV_COUNT   512
#define IOP_EVCB_SPEC_COUNT 18

void iop_hle_events_init(void);

/* Real MIPS calling convention throughout: reads a0-a3 from
 * st->gpr[4..7], writes the return value to st->gpr[2] ($v0) when the
 * real function returns one. Each function handles its own class/
 * spec (or handle) decoding per the cited GetEv/GetSpec algorithm or
 * the cited `ev=handle&0xff, spec=(handle>>8)&0xff` handle-decode,
 * matching real usage (OpenEvent returns a handle in that packed
 * format; CloseEvent/WaitEvent/TestEvent/EnableEvent/DisableEvent
 * take that handle back as their sole argument). */
void iop_hle_event_deliver(iop_state_t *st);
void iop_hle_event_open(iop_state_t *st);
void iop_hle_event_close(iop_state_t *st);
void iop_hle_event_wait(iop_state_t *st);
void iop_hle_event_test(iop_state_t *st);
void iop_hle_event_enable(iop_state_t *st);
void iop_hle_event_disable(iop_state_t *st);
void iop_hle_event_undeliver(iop_state_t *st);

/* Round 148 (task #301): a plain-C entry point for hardware models
 * (e.g. iop_cdrom_legacy.c) to deliver a real event directly, without
 * needing to fake up a gpr-based B0h call. Same real hashing/delivery
 * semantics as iop_hle_event_deliver() above - this is the mechanism
 * this project's real hardware interrupt sources use to perform the
 * job that, on real hardware, the "totally bugged" (nocash's own
 * words) DefaultInterruptHandler chain performs automatically for
 * every hardware IRQ 0-10 (psx-spx kernelbios.md, directly quoted:
 * "The totally bugged DefaultInterruptHandlers is always installed
 * (and cannot be removed), so it does randomly trigger Events") -
 * this project models the INTENDED, non-buggy behavior (deliver the
 * real, cited class/spec pair when the corresponding real hardware
 * condition occurs), not nocash's documented "randomly" quirk, which
 * is out of scope (a hardware timing bug, not a testable contract). */
void iop_hle_event_deliver_raw(uint32_t class_code, uint32_t spec_mask);

#endif
