/*
 * test_gs_signal.c - host-native test for Round 108's real GS
 * SIGNAL/FINISH/LABEL registers (task #254, 149th finding - the
 * FINAL confirmed-missing GS register batch). See include/core/hw/
 * gif.h's GS_REG_SIGNAL/FINISH/LABEL comment for the full scope and
 * citation (official Sony GS Users Manual "SIGNAL/FINISH/LABEL :
 * ... Event Occurrence Request" - SIGLBLID masked-update semantics,
 * honestly scoped as GS-local-state-only, no EE/IOP interrupt-
 * controller wiring).
 *
 * Uses the same write_tag/append_ad A+D-mode packet-building
 * convention already established in tests/test_gs_alpha.c /
 * tests/test_gs_fba.c. Inspects gif_get_state() directly (same
 * convention as tests/test_gs_dimx.c and others) since SIGLBLID is
 * modeled as plain internal state, not a readable register path.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/hw/gif.h"
#include "core/hw/gs_mem.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void wle32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void write_tag(uint8_t *buf, int *off, uint32_t nloop, uint32_t regs_nibble0)
{
    uint32_t w0 = nloop & 0x7FFFu;
    uint32_t w1 = (0u << 26) | (1u << 28);
    uint32_t w2 = regs_nibble0 & 0xFu;
    uint32_t w3 = 0u;
    wle32(buf + *off, w0); wle32(buf + *off + 4, w1);
    wle32(buf + *off + 8, w2); wle32(buf + *off + 12, w3);
    *off += 16;
}

static void append_ad(uint8_t *buf, int *off, uint32_t data_lo, uint32_t data_hi, uint32_t addr)
{
    wle32(buf + *off, data_lo); wle32(buf + *off + 4, data_hi);
    wle32(buf + *off + 8, addr); wle32(buf + *off + 12, 0);
    *off += 16;
}

static void write_reg(uint32_t data_lo, uint32_t data_hi, uint32_t addr)
{
    uint8_t buf[16 * 2];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    write_tag(buf, &off, 1, 0xE);
    append_ad(buf, &off, data_lo, data_hi, addr);
    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
}

int main(void)
{
    gs_mem_init();
    gif_init();
    gif_state_t *gs = gif_get_state();

    { /* Initial state: SIGLBLID.SIGID/LBLID both 0, finish_pending 0
       * (regression safety - genuine no-op until any of these
       * registers is actually written). */
        CHECK(gs->siglblid_sigid == 0u && gs->siglblid_lblid == 0u && gs->finish_pending == 0u,
              "no SIGNAL/FINISH/LABEL write yet: SIGLBLID and finish_pending all start at 0 (regression safety)");
    }

    { /* SIGNAL with ID=0xFFFFFFFF, IDMSK=0x0000FFFF: only the low 16
       * bits of SIGID should update (masked formula), high 16 bits
       * stay 0. */
        write_reg(0xFFFFFFFFu, 0x0000FFFFu, GS_REG_SIGNAL);
        CHECK(gs->siglblid_sigid == 0x0000FFFFu,
              "SIGNAL ID=0xFFFFFFFF IDMSK=0x0000FFFF: SIGID masked-updates to 0x0000FFFF only");
    }

    { /* A second SIGNAL with a mask covering only the high 16 bits,
       * ID=0xABCD0000: the previously-set low 16 bits (0xFFFF) must
       * survive untouched - proves this is a genuine masked
       * read-modify-write, not a plain overwrite. */
        write_reg(0xABCD0000u, 0xFFFF0000u, GS_REG_SIGNAL);
        CHECK(gs->siglblid_sigid == 0xABCDFFFFu,
              "second SIGNAL with a high-half mask: low half (0xFFFF) survives untouched, high half updates to 0xABCD");
    }

    { /* LABEL updates LBLID independently of SIGID - SIGID must stay
       * exactly as SIGNAL left it. */
        write_reg(0x12345678u, 0xFFFFFFFFu, GS_REG_LABEL);
        CHECK(gs->siglblid_lblid == 0x12345678u && gs->siglblid_sigid == 0xABCDFFFFu,
              "LABEL ID=0x12345678 IDMSK=0xFFFFFFFF: LBLID fully updates, SIGID (from SIGNAL) untouched");
    }

    { /* FINISH: any data accepted as a genuine no-op - drawing
       * behavior/framebuffer state completely unaffected; only the
       * test-observability counter increments. */
        uint32_t before = gs->finish_pending;
        write_reg(0xDEADBEEFu, 0xCAFEBABEu, GS_REG_FINISH);
        CHECK(gs->finish_pending == before + 1,
              "FINISH: arbitrary data accepted as a real, distinct register (finish_pending increments)");
        CHECK(gs->siglblid_sigid == 0xABCDFFFFu && gs->siglblid_lblid == 0x12345678u,
              "FINISH: SIGLBLID (SIGID/LBLID) completely unaffected by the write");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
