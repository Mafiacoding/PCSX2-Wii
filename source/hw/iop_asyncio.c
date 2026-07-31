/*
 * iop_asyncio.c - see include/core/hw/iop_asyncio.h for the full
 * citation trail and clean-room scope note (Round 153, task #307).
 */
#include "core/hw/iop_asyncio.h"
#include <string.h>

typedef struct {
    int                     active;
    iop_asyncio_device_t    device;
    iop_asyncio_callback_t  callback;
    void                   *user_data;
} iop_asyncio_channel_t;

/* Named g_asyncio (not the bare "g" this codebase's other hw source
 * files conventionally use for their static state) since this file
 * gets #included directly alongside iop_cdrom_legacy.c in
 * tests/test_iop_cdrom_legacy.c - a bare "g" would collide. */
static struct {
    iop_asyncio_channel_t channels[IOP_ASYNCIO_MAX_CHANNELS];
    int busy_count;
    int next_alloc;   /* round-robin channel allocator cursor */
    int next_service;  /* round-robin service cursor - ensures fair,
                         * FIFO-ish servicing across channels rather
                         * than always favoring low indices. */
} g_asyncio;

void iop_asyncio_init(void)
{
    memset(&g_asyncio, 0, sizeof(g_asyncio));
}

int iop_asyncio_enqueue(iop_asyncio_device_t device,
                         iop_asyncio_callback_t callback,
                         void *user_data)
{
    for (int tries = 0; tries < IOP_ASYNCIO_MAX_CHANNELS; tries++) {
        int idx = (g_asyncio.next_alloc + tries) % IOP_ASYNCIO_MAX_CHANNELS;
        if (!g_asyncio.channels[idx].active) {
            g_asyncio.channels[idx].active    = 1;
            g_asyncio.channels[idx].device    = device;
            g_asyncio.channels[idx].callback  = callback;
            g_asyncio.channels[idx].user_data = user_data;
            g_asyncio.next_alloc = (idx + 1) % IOP_ASYNCIO_MAX_CHANNELS;
            g_asyncio.busy_count++;
            return idx;
        }
    }
    return -1; /* all 128 channels active - table full */
}

void iop_asyncio_service(void)
{
    if (g_asyncio.busy_count == 0)
        return;

    for (int tries = 0; tries < IOP_ASYNCIO_MAX_CHANNELS; tries++) {
        int idx = (g_asyncio.next_service + tries) % IOP_ASYNCIO_MAX_CHANNELS;
        if (g_asyncio.channels[idx].active) {
            iop_asyncio_callback_t cb = g_asyncio.channels[idx].callback;
            void *ud = g_asyncio.channels[idx].user_data;

            g_asyncio.channels[idx].active   = 0;
            g_asyncio.channels[idx].callback = 0;
            g_asyncio.busy_count--;
            g_asyncio.next_service = (idx + 1) % IOP_ASYNCIO_MAX_CHANNELS;

            if (cb) cb(ud);
            return; /* one request per call - real live-observed burst
                      * pattern, not a full-queue drain. */
        }
    }
}

int iop_asyncio_busy_count(void)
{
    return g_asyncio.busy_count;
}
