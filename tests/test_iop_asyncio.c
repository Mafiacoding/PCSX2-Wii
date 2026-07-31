/*
 * test_iop_asyncio.c - host-native test for Round 153's real async
 * I/O request queue + channel dispatch subsystem
 * (source/hw/iop_asyncio.c). See include/core/hw/iop_asyncio.h for
 * the full citation trail and clean-room scope note.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "hw/iop_asyncio.c"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("ok:   %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

static int g_cb_calls = 0;
static void *g_cb_last_user_data = 0;
static void test_callback(void *user_data)
{
    g_cb_calls++;
    g_cb_last_user_data = user_data;
}

int main(void)
{
    /* Basic enqueue/service round trip. */
    iop_asyncio_init();
    CHECK(iop_asyncio_busy_count() == 0, "starts empty");

    int marker = 42;
    int idx = iop_asyncio_enqueue(IOP_ASYNCIO_DEV_CDROM, test_callback, &marker);
    CHECK(idx >= 0 && idx < IOP_ASYNCIO_MAX_CHANNELS, "enqueue returns a valid channel index");
    CHECK(iop_asyncio_busy_count() == 1, "busy count is 1 after one enqueue");
    CHECK(g_cb_calls == 0, "callback not yet invoked before service()");

    iop_asyncio_service();
    CHECK(g_cb_calls == 1, "callback invoked exactly once after one service() call");
    CHECK(g_cb_last_user_data == &marker, "callback receives the correct user_data pointer");
    CHECK(iop_asyncio_busy_count() == 0, "busy count back to 0 after servicing the only request");

    /* service() with nothing queued is a safe no-op. */
    iop_asyncio_service();
    CHECK(g_cb_calls == 1, "service() with an empty queue does not re-invoke the callback");

    /* Round-robin allocator: allocate, drain one, allocate again -
     * the freed slot should eventually get reused. */
    iop_asyncio_init();
    g_cb_calls = 0;
    for (int i = 0; i < 5; i++) {
        int c = iop_asyncio_enqueue(IOP_ASYNCIO_DEV_CDROM, test_callback, 0);
        CHECK(c == i, "sequential enqueue allocates sequential channel indices");
    }
    CHECK(iop_asyncio_busy_count() == 5, "busy count tracks 5 pending requests");

    /* service() drains exactly one request per call - the real,
     * live-observed burst pattern (Round 152), not a full drain. */
    iop_asyncio_service();
    CHECK(iop_asyncio_busy_count() == 4, "service() drains exactly one request per call");
    CHECK(g_cb_calls == 1, "exactly one callback fired after one service() call");

    int drained = 1;
    while (iop_asyncio_busy_count() > 0) {
        iop_asyncio_service();
        drained++;
    }
    CHECK(drained == 5, "draining the whole queue takes exactly 5 service() calls for 5 requests");
    CHECK(g_cb_calls == 5, "all 5 callbacks eventually fire");

    /* Channel table exhaustion: fill all 128 channels, confirm the
     * 129th enqueue is rejected (-1), matching the real live-traced
     * bound (Round 152: `slti v0, s1, 0x0080`). */
    iop_asyncio_init();
    int last_idx = -2;
    for (int i = 0; i < IOP_ASYNCIO_MAX_CHANNELS; i++) {
        last_idx = iop_asyncio_enqueue(IOP_ASYNCIO_DEV_CDROM, test_callback, 0);
    }
    CHECK(last_idx == IOP_ASYNCIO_MAX_CHANNELS - 1, "128th enqueue succeeds at the last valid index");
    CHECK(iop_asyncio_busy_count() == IOP_ASYNCIO_MAX_CHANNELS, "busy count saturates at 128");
    int overflow_idx = iop_asyncio_enqueue(IOP_ASYNCIO_DEV_CDROM, test_callback, 0);
    CHECK(overflow_idx == -1, "129th enqueue is rejected once all 128 channels are active");

    printf("\n%s: %d check(s) failed\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
