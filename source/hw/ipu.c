/*
 * ipu.c - see include/core/hw/ipu.h for full scope notes and the
 * real-hardware citation trail (Round 521/522, task #487).
 */
#include "core/hw/ipu.h"
#include <string.h>

#define IPU_REG_CMD_DATA  0x10002000u
#define IPU_REG_CMD_BUSY  0x10002004u
#define IPU_REG_CTRL      0x10002010u
#define IPU_REG_BP        0x10002020u
#define IPU_REG_TOP       0x10002030u
#define IPU_REG_TOPBUSY   0x10002034u

/* IPU_CTRL bit layout (PCSX2's tIPU_CTRL, IPU.h - see ipu.h header
 * comment for the full field list). Only the fields this skeleton
 * round actually reads/writes are named here; the rest live inside
 * the raw ctrl word and are preserved bit-for-bit across writes. */
#define CTRL_IFC_SHIFT   0u
#define CTRL_IFC_MASK    0x0000000Fu
#define CTRL_OFC_SHIFT   4u
#define CTRL_OFC_MASK    0x000000F0u
#define CTRL_ECD_BIT     0x00040000u
#define CTRL_SCD_BIT     0x00080000u
#define CTRL_RST_BIT     0x40000000u
#define CTRL_BUSY_BIT    0x80000000u
/* Real writable-bits mask on a software write to IPU_CTRL - directly
 * cited from PCSX2's tIPU_CTRL::write(): "CTRL = the first 16 bits of
 * ctrl [0x8000ffff], + value for the next 16 bits, minus the reserved
 * bits (18-19; 27-29) [0x47f30000]". BUSY (bit31) and the low 16 bits
 * (IFC/OFC/CBP, real-time status) are never settable by a plain
 * write; RST (bit30) IS software-writable and handled specially
 * below, matching real hardware's "write RST to reset the unit". */
#define CTRL_WRITABLE_MASK 0x47F30000u

#define IPU_FIFO_DEPTH_QWC 8u /* real hardware: 8 QWC input FIFO depth */

typedef struct {
    uint32_t cmd_data;
    uint32_t cmd_busy;
    uint32_t ctrl;
    uint32_t bp;
    uint32_t top;
    uint32_t topbusy;

    /* Real input FIFO: up to IPU_FIFO_DEPTH_QWC quadwords (16 bytes
     * each), fed by DMA_CHANNEL_TOIPU. No consumer drains it yet
     * (real decode is a later round - see ipu.h), so entries are
     * discarded once IFC is updated; this struct only tracks the
     * real fill-level bookkeeping software can observe via
     * IPU_CTRL.IFC, not the actual bytes. */
    uint32_t in_fifo_count;
} ipu_state_t;

static ipu_state_t g_ipu;

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void ipu_init(void)
{
    memset(&g_ipu, 0, sizeof(g_ipu));
}

/* Real hardware: BCLR clears the input FIFO and resets the bit-stream
 * pointer (ps2tek/PCSX2-cited: "BCLR clears all data in the input
 * FIFO"). Matches this project's own dma.c-style channel-reset
 * conventions elsewhere. */
static void ipu_cmd_bclr(void)
{
    g_ipu.in_fifo_count = 0;
    g_ipu.bp = 0;
    g_ipu.ctrl &= ~(CTRL_IFC_MASK | CTRL_SCD_BIT | CTRL_ECD_BIT);
}

/* Real hardware: "When a command is sent, ECD and SCD are cleared to
 * 0 in the IPU_CTRL register" (directly cited). This project doesn't
 * yet implement the real per-command decode this status would
 * normally reflect (see ipu.h scope note), so BUSY is set then
 * immediately cleared in the same call - an honest simplification of
 * real hardware's actual multi-cycle decode latency, not a claim that
 * real decode happened. */
static void ipu_dispatch_cmd(uint32_t value)
{
    uint32_t cmd = (value >> 28) & 0xFu;

    g_ipu.ctrl &= ~(CTRL_ECD_BIT | CTRL_SCD_BIT);
    g_ipu.cmd_data = value;

    if (cmd == 0u) { /* BCLR */
        ipu_cmd_bclr();
        g_ipu.cmd_busy = 0;
        return;
    }

    /* IDEC/BDEC/VDEC/FDEC/SETIQ/SETVQ/CSC/PACK/SETTH (cmd 1-9): real
     * register-level acknowledgement only - see ipu.h's scope note.
     * BUSY set-then-cleared rather than left set, since there is no
     * real decode work in flight yet to eventually clear it. */
    g_ipu.cmd_busy = CTRL_BUSY_BIT;
    g_ipu.cmd_busy = 0;
}

int ipu_mmio_read32(uint32_t addr, uint32_t *out)
{
    switch (addr) {
        case IPU_REG_CMD_DATA:
            *out = g_ipu.cmd_data;
            return 1;
        case IPU_REG_CMD_BUSY:
            *out = g_ipu.cmd_busy;
            return 1;
        case IPU_REG_CTRL:
            *out = g_ipu.ctrl;
            return 1;
        case IPU_REG_BP:
            *out = g_ipu.bp;
            return 1;
        case IPU_REG_TOP:
            *out = g_ipu.top;
            return 1;
        case IPU_REG_TOPBUSY:
            *out = g_ipu.topbusy;
            return 1;
        default:
            return 0;
    }
}

int ipu_mmio_write32(uint32_t addr, uint32_t value)
{
    switch (addr) {
        case IPU_REG_CMD_DATA:
            ipu_dispatch_cmd(value);
            return 1;
        case IPU_REG_CTRL:
            if (value & CTRL_RST_BIT) {
                /* Real hardware: writing RST resets the unit - same
                 * real effect as BCLR plus clearing the whole
                 * register, matching PCSX2's own tIPU_CTRL::reset()
                 * ("_u32 &= 0x7F33F00", i.e. drop everything except a
                 * handful of status bits it preserves). This project
                 * has no real decode state to preserve, so a full
                 * clear is the honest equivalent. */
                ipu_cmd_bclr();
                g_ipu.ctrl = 0;
                g_ipu.cmd_busy = 0;
                return 1;
            }
            g_ipu.ctrl = (value & CTRL_WRITABLE_MASK) | (g_ipu.ctrl & ~CTRL_WRITABLE_MASK);
            return 1;
        case IPU_REG_BP:
            g_ipu.bp = value;
            return 1;
        case IPU_REG_TOP:
            g_ipu.top = value;
            return 1;
        case IPU_REG_TOPBUSY:
            g_ipu.topbusy = value;
            return 1;
        case IPU_REG_CMD_BUSY:
            return 1; /* real hardware: BUSY is read-only status, write ignored */
        default:
            return 0;
    }
}

void ipu_process_quadwords(int channel, const uint8_t *data, uint32_t qwc)
{
    (void)channel;
    (void)data;
    (void)rd_le32; /* reserved for the real decode round - see ipu.h scope note */

    for (uint32_t i = 0; i < qwc; i++) {
        if (g_ipu.in_fifo_count >= IPU_FIFO_DEPTH_QWC)
            break; /* real FIFO full - see ipu.h's honest-stall-not-modeled note */
        g_ipu.in_fifo_count++;
    }
    g_ipu.ctrl = (g_ipu.ctrl & ~CTRL_IFC_MASK)
               | ((g_ipu.in_fifo_count << CTRL_IFC_SHIFT) & CTRL_IFC_MASK);
}
