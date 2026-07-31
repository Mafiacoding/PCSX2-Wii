#ifndef PCSX2_WII_IOP_ASYNCIO_H
#define PCSX2_WII_IOP_ASYNCIO_H

#include <stdint.h>

/*
 * iop_asyncio.h - clean-room async I/O request queue + fixed-size
 * channel dispatch table (Round 153, task #307).
 *
 * BACKGROUND: Round 152 took direct control of the user's live
 * PCSX2 reference session (desktop access, granted by the user) and
 * repeatedly paused/read the real IOP CPU during genuine, ongoing
 * CD/DVD streaming (an actual FMV playing off a real GT3 disc - not
 * this project's own diskless boot). Two independent live samples
 * showed the real IOP genuinely idle (a literal `j`-to-self spin
 * loop) for 9+ seconds at a stretch, then a later sample caught it
 * mid-burst inside a real driver routine: a request gets appended to
 * a queue anchored at a fixed kernel global, a "busy count" global is
 * checked, and - when busy - a rate/throughput calculation runs;
 * when idle, a queue-kick routine dequeues the head request,
 * allocates a channel/slot index bounded to 128, indexes an 8-byte-
 * stride per-channel table, and dispatches onward (see docs/STATUS.md
 * 192nd finding for the full live-trace writeup).
 *
 * WHAT THIS FILE IS: a clean-room reimplementation of that
 * ARCHITECTURE only - "requests get appended to a queue; a bounded
 * channel table dispatches them; a busy-count/burst gate paces
 * servicing" - not a transcription of the real kernel's memory
 * layout, global addresses, struct field offsets, or any literal
 * byte/instruction sequence observed live. Per this project's
 * standing clean-room policy (see CLAUDE.md), only the architectural/
 * behavioral shape informs this implementation; this project's own
 * global addresses, struct layout, and channel-table indexing below
 * are original. The 128-channel count matches the real live trace's
 * observed bound (`slti v0, s1, 0x0080`) and is kept here for
 * plausible compatibility headroom, not because this project
 * independently re-derived that specific number from any other
 * source.
 *
 * WHAT THIS FILE IS NOT: a claim that any specific real PS2 command
 * (Setloc included) is documented to route through this exact
 * mechanism during this project's own diskless boot sequence. Rounds
 * 150-152 traced the real reference session's runtime (in-game/FMV)
 * disc-streaming path; this project's own diskless boot's real
 * trigger chain for the initial CD-ROM-readiness TestEvent wait
 * remains only partially understood (see docs/STATUS.md 190th-192nd
 * findings). See iop_cdrom_legacy.c's cdrom_boot_kick_complete() for
 * this project's honestly-labeled, non-cited use of this queue to
 * unblock that specific boot-time wait.
 */

#define IOP_ASYNCIO_MAX_CHANNELS 128

typedef void (*iop_asyncio_callback_t)(void *user_data);

typedef enum {
    IOP_ASYNCIO_DEV_CDROM = 0
} iop_asyncio_device_t;

void iop_asyncio_init(void);

/* Enqueues a request; returns the allocated channel index
 * (0..IOP_ASYNCIO_MAX_CHANNELS-1) on success, -1 if the channel
 * table is full (all 128 channels currently active). */
int iop_asyncio_enqueue(iop_asyncio_device_t device,
                         iop_asyncio_callback_t callback,
                         void *user_data);

/* Services at most one pending request per call - models the real
 * live-observed "kick idle queue, dispatch one request, return"
 * burst pattern rather than draining the whole queue in one shot.
 * Call once per IOP scheduler tick (see iop_core_step()). */
void iop_asyncio_service(void);

/* Diagnostics / tests. */
int iop_asyncio_busy_count(void);

#endif
