/*
 * iop_sio2.c - see include/core/hw/iop_sio2.h for the full scope
 * notes and citation trail (Round 135 register scaffold, Round 146
 * real memory-card command/response protocol, task #299).
 */
#include "core/hw/iop_sio2.h"
#include <string.h>
#include <stdio.h>

/* Raw register backing store for SEND3/SEND1/SEND2/RECV1-3 (no real
 * cited command protocol for those blocks - unchanged Round 135
 * scaffold behavior: read back whatever was last written). FIFOIN/
 * FIFOOUT/CTRL are intercepted below and drive the real memory-card
 * protocol instead of being plain readback registers. */
static uint8_t g_regs[IOP_SIO2_SIZE];

/* Real memory-card device state. */
typedef struct {
    int inserted;
    uint8_t flag;
    uint8_t data[IOP_MC_CARD_BYTES];
} mc_card_t;
static mc_card_t g_mc;

/* Real digital-pad device state (Round 184). buttons stores PRESSED
 * polarity (bit=1 means pressed) - the real hardware's inverted wire
 * polarity (0=pressed) is applied only at reply-generation time. */
typedef struct {
    int connected;
    uint16_t buttons; /* pressed-polarity */
    int analog_mode;  /* Round 195: 0=digital (real cited power-up
                        * default), 1=analog */
    uint8_t axis_rx, axis_ry, axis_lx, axis_ly; /* Round 195: real
                        * cited 00h/80h/FFh convention, default 0x80
                        * (Center) each */
} pad_t;
static pad_t g_pad;

/* Command/reply staging buffers for the batched FIFOIN -> CTRL-start
 * -> FIFOOUT model described in the header. */
#define MC_CMD_MAX 512
static uint8_t g_cmd_buf[MC_CMD_MAX];
static int g_cmd_len;
static uint8_t g_reply_buf[MC_CMD_MAX];
static int g_reply_len;
static int g_reply_pos;

/* Real, cited sub-block offsets (ps2tek SIO2 register table).
 * SEND3 (0x00, size 0x40), SEND1/SEND2 (0x40, size 0x20), and
 * RECV1/RECV2/RECV3 (0x6C/0x70/0x74) have no cited command protocol
 * (Round 135 scope note) and fall through to the plain readback
 * register file below; only FIFOIN/FIFOOUT/CTRL are intercepted. */
static const uint32_t OFF_FIFOIN  = 0x60;
static const uint32_t OFF_FIFOOUT = 0x64;
static const uint32_t OFF_CTRL    = 0x68;

void iop_sio2_init(void)
{
    memset(g_regs, 0, sizeof(g_regs));
    memset(&g_mc, 0, sizeof(g_mc));
    g_mc.inserted = 0; /* real cited default: no card, honest cardless
                         * boot-trace scenario preserved */
    g_mc.flag = IOP_MC_FLAG_INIT;
    g_pad.connected = 1; /* real cited default: a controller port is
                           * always physically present on real PS2
                           * hardware - opt-out via
                           * iop_sio2_pad_disconnect() for testing */
    g_pad.buttons = 0;   /* pressed-polarity: 0 = nothing pressed */
    g_pad.analog_mode = 0; /* real cited power-up default: digital */
    g_pad.axis_rx = g_pad.axis_ry = g_pad.axis_lx = g_pad.axis_ly =
        IOP_PAD_ANALOG_CENTER; /* real cited rest/center value */
    g_cmd_len = 0;
    g_reply_len = 0;
    g_reply_pos = 0;
}

void iop_sio2_mc_insert_blank(void)
{
    memset(g_mc.data, 0, sizeof(g_mc.data));
    g_mc.inserted = 1;
    g_mc.flag = IOP_MC_FLAG_INIT; /* real power-up/insert value, cited */
}

int iop_sio2_mc_insert_from_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    memset(g_mc.data, 0, sizeof(g_mc.data));
    size_t got = fread(g_mc.data, 1, sizeof(g_mc.data), f);
    fclose(f);
    if (got == 0)
        return 0;
    g_mc.inserted = 1;
    g_mc.flag = IOP_MC_FLAG_INIT;
    return 1;
}

void iop_sio2_mc_eject(void)
{
    g_mc.inserted = 0;
}

int iop_sio2_mc_is_inserted(void)
{
    return g_mc.inserted;
}

void iop_sio2_pad_connect(void)
{
    g_pad.connected = 1;
}

void iop_sio2_pad_disconnect(void)
{
    g_pad.connected = 0;
}

int iop_sio2_pad_is_connected(void)
{
    return g_pad.connected;
}

void iop_sio2_pad_set_buttons(uint16_t pressed_mask)
{
    g_pad.buttons = pressed_mask;
}

void iop_sio2_pad_press(uint16_t pressed_bits)
{
    g_pad.buttons |= pressed_bits;
}

void iop_sio2_pad_release(uint16_t pressed_bits)
{
    g_pad.buttons &= (uint16_t)~pressed_bits;
}

uint16_t iop_sio2_pad_get_buttons(void)
{
    return g_pad.buttons;
}

void iop_sio2_pad_set_analog_mode(int enabled)
{
    g_pad.analog_mode = enabled ? 1 : 0;
}

int iop_sio2_pad_is_analog_mode(void)
{
    return g_pad.analog_mode;
}

void iop_sio2_pad_set_analog_axes(uint8_t right_x, uint8_t right_y,
                                   uint8_t left_x, uint8_t left_y)
{
    g_pad.axis_rx = right_x;
    g_pad.axis_ry = right_y;
    g_pad.axis_lx = left_x;
    g_pad.axis_ly = left_y;
}

void iop_sio2_pad_get_analog_axes(uint8_t *right_x, uint8_t *right_y,
                                   uint8_t *left_x, uint8_t *left_y)
{
    if (right_x) *right_x = g_pad.axis_rx;
    if (right_y) *right_y = g_pad.axis_ry;
    if (left_x)  *left_x  = g_pad.axis_lx;
    if (left_y)  *left_y  = g_pad.axis_ly;
}

/* Runs the full real memory-card command/response protocol against
 * g_cmd_buf[0..g_cmd_len), filling g_reply_buf/g_reply_len. See the
 * header's citation trail (psx-spx Memory Card Read/Write Commands +
 * ps2sdk mcsio2.c McReadPS1PDACard/McWritePS1PDACard) for every byte
 * value used here. */
/* Real digital-pad command/response protocol (Round 184). See the
 * header's citation trail (psx-spx Controllers - Communication
 * Sequence / Standard Digital Controllers) for every byte value used
 * here. */
static void pad_process_command(void)
{
    int n = g_cmd_len;
    g_reply_len = 0;
    g_reply_pos = 0;

    if (n < 1)
        return;

    if (!g_pad.connected) {
        /* real cited High-Z bus behavior, same FFh convention already
         * used for the no-memory-card case. */
        int i;
        for (i = 0; i < n; i++)
            g_reply_buf[g_reply_len++] = 0xFF;
        return;
    }

    g_reply_buf[g_reply_len++] = 0x00; /* reply to 01h: dummy/don't-care */
    if (n < 2)
        return;

    uint8_t cmd = g_cmd_buf[1];
    if (cmd != IOP_PAD_CMD_READ) {
        /* real cited invalid-command framing: only 42h is modeled for
         * this digital-pad-only scope; anything else gets no further
         * reply bytes beyond the dummy already emitted. */
        return;
    }

    /* Round 195: real cited ID depends on mode - 5A41h digital,
     * 5A73h analog (psx-spx). */
    g_reply_buf[g_reply_len++] = g_pad.analog_mode ?
        IOP_PAD_ID_ANALOG_LO : IOP_PAD_ID_LO; /* reply to 42h */
    if (n < 3) return;
    g_reply_buf[g_reply_len++] = g_pad.analog_mode ?
        IOP_PAD_ID_ANALOG_HI : IOP_PAD_ID_HI; /* reply to TAP byte */
    if (n < 4) return;

    /* real hardware wire polarity is 0=pressed - invert our
     * pressed-polarity g_pad.buttons only at this final step. */
    uint16_t wire = (uint16_t)~g_pad.buttons;
    g_reply_buf[g_reply_len++] = (uint8_t)(wire & 0xFFu);        /* swlo, reply to 1st MOT byte */
    if (n < 5) return;
    g_reply_buf[g_reply_len++] = (uint8_t)((wire >> 8) & 0xFFu); /* swhi, reply to 2nd MOT byte */

    if (!g_pad.analog_mode) {
        /* "transfer stops here for digital pad" - real cited fact,
         * no further reply bytes regardless of how many more were
         * sent. */
        return;
    }

    /* Round 195: real cited analog-mode continuation - 4 more bytes
     * (adc0..adc3 = RightJoyX, RightJoyY, LeftJoyX, LeftJoyY), each
     * only sent if the caller actually clocks that many bytes (same
     * "if (n < N) return" pattern already used throughout this
     * function for a real partial/short transfer). */
    if (n < 6) return;
    g_reply_buf[g_reply_len++] = g_pad.axis_rx; /* adc0 */
    if (n < 7) return;
    g_reply_buf[g_reply_len++] = g_pad.axis_ry; /* adc1 */
    if (n < 8) return;
    g_reply_buf[g_reply_len++] = g_pad.axis_lx; /* adc2 */
    if (n < 9) return;
    g_reply_buf[g_reply_len++] = g_pad.axis_ly; /* adc3 */
    /* "transfer stops here for analog pad (in analog mode)" - real
     * cited fact, no further reply bytes regardless of how many more
     * were sent. */
}

static void mc_process_command(void)
{
    int n = g_cmd_len;
    g_reply_len = 0;
    g_reply_pos = 0;

    if (n < 1)
        return;

    if (g_cmd_buf[0] == IOP_PAD_ADDR_BYTE) {
        pad_process_command();
        return;
    }

    if (g_cmd_buf[0] != IOP_MC_ADDR_BYTE || !g_mc.inserted) {
        /* Either not addressed to the memory card, or no card
         * physically present - real cited High-Z bus behavior
         * ("FFFFh=High-Z, no controller connected, pins floating
         * High-Z", psx-spx). */
        int i;
        for (i = 0; i < n; i++)
            g_reply_buf[g_reply_len++] = 0xFF;
        return;
    }

    uint8_t cmd = (n > 1) ? g_cmd_buf[1] : 0;
    g_reply_buf[g_reply_len++] = 0x00; /* reply to 81h: dummy/don't-care */

    if (n < 2) {
        return; /* address byte only, nothing to dispatch yet */
    }

    if (cmd == IOP_MC_CMD_READ) {
        g_reply_buf[g_reply_len++] = g_mc.flag; /* reply to 52h */
        g_reply_buf[g_reply_len++] = IOP_MC_ID1;
        g_reply_buf[g_reply_len++] = IOP_MC_ID2;

        /* real cited layout: cmd_buf[2..3] are the two ID-pump send
         * bytes (dummy 00h, replies 5Ah/5Dh already emitted above);
         * MSB/LSB are the NEXT two sent bytes, at [4]/[5]. */
        uint8_t msb = (n > 4) ? g_cmd_buf[4] : 0;
        uint8_t lsb = (n > 5) ? g_cmd_buf[5] : 0;
        g_reply_buf[g_reply_len++] = 0x00; /* reply to sent MSB */
        g_reply_buf[g_reply_len++] = msb;  /* reply to sent LSB ("pre") */

        g_reply_buf[g_reply_len++] = IOP_MC_ACK1;
        g_reply_buf[g_reply_len++] = IOP_MC_ACK2;

        uint32_t sector = ((uint32_t)msb << 8) | lsb;
        int valid = (sector < IOP_MC_SECTOR_COUNT);
        if (!valid) {
            /* real cited Sony-card behavior: FFFFh confirmed address,
             * abort without data/checksum/end byte */
            g_reply_buf[g_reply_len++] = 0xFF;
            g_reply_buf[g_reply_len++] = 0xFF;
            return;
        }
        g_reply_buf[g_reply_len++] = msb; /* confirmed address MSB */
        g_reply_buf[g_reply_len++] = lsb; /* confirmed address LSB */

        const uint8_t *sec = &g_mc.data[sector * IOP_MC_SECTOR_SIZE];
        uint8_t chk = (uint8_t)(msb ^ lsb);
        uint32_t i;
        for (i = 0; i < IOP_MC_SECTOR_SIZE; i++) {
            g_reply_buf[g_reply_len++] = sec[i];
            chk ^= sec[i];
        }
        g_reply_buf[g_reply_len++] = chk;             /* checksum */
        g_reply_buf[g_reply_len++] = IOP_MC_END_GOOD;  /* 47h, always
                                                          * Good for Read */
        /* real cited fact: bit3 is NOT reset by reading, only by
         * writing - flag left unchanged here. */
        return;
    }

    if (cmd == IOP_MC_CMD_WRITE) {
        g_reply_buf[g_reply_len++] = g_mc.flag; /* reply to 57h */
        g_reply_buf[g_reply_len++] = IOP_MC_ID1;
        g_reply_buf[g_reply_len++] = IOP_MC_ID2;

        /* real cited layout: cmd_buf[2..3] are the two ID-pump send
         * bytes; MSB/LSB are at [4]/[5]; the 128-byte data payload
         * starts right after LSB, at [6..133]; the checksum byte
         * follows at [134]. */
        uint8_t msb = (n > 4) ? g_cmd_buf[4] : 0;
        uint8_t lsb = (n > 5) ? g_cmd_buf[5] : 0;
        g_reply_buf[g_reply_len++] = 0x00; /* reply to sent MSB */
        g_reply_buf[g_reply_len++] = msb;  /* reply to sent LSB ("pre") */

        uint8_t chk = (uint8_t)(msb ^ lsb);
        uint8_t prev = lsb;
        uint32_t i;
        uint8_t data[IOP_MC_SECTOR_SIZE];
        for (i = 0; i < IOP_MC_SECTOR_SIZE; i++) {
            uint8_t b = (n > (int)(6 + i)) ? g_cmd_buf[6 + i] : 0;
            data[i] = b;
            chk ^= b;
            g_reply_buf[g_reply_len++] = prev; /* "(pre)": echoes the
                                                  * previously-sent byte */
            prev = b;
        }
        uint8_t sent_chk = (n > (int)(6 + IOP_MC_SECTOR_SIZE)) ?
            g_cmd_buf[6 + IOP_MC_SECTOR_SIZE] : 0;
        g_reply_buf[g_reply_len++] = prev; /* reply to checksum byte: (pre) */

        g_reply_buf[g_reply_len++] = IOP_MC_ACK1;
        g_reply_buf[g_reply_len++] = IOP_MC_ACK2;

        uint32_t sector = ((uint32_t)msb << 8) | lsb;
        if (sector >= IOP_MC_SECTOR_COUNT) {
            g_reply_buf[g_reply_len++] = IOP_MC_END_BADSECTOR; /* FFh */
        } else if (sent_chk != chk) {
            g_reply_buf[g_reply_len++] = IOP_MC_END_BADCHECKSUM; /* 4Eh */
        } else {
            memcpy(&g_mc.data[sector * IOP_MC_SECTOR_SIZE], data,
                   IOP_MC_SECTOR_SIZE);
            g_mc.flag &= (uint8_t)~IOP_MC_FLAG_DIRTY; /* real cited fact:
                                                         * bit3 clears on
                                                         * write */
            g_reply_buf[g_reply_len++] = IOP_MC_END_GOOD; /* 47h */
        }
        return;
    }

    if (cmd == IOP_MC_CMD_GETID) {
        g_reply_buf[g_reply_len++] = g_mc.flag; /* reply to 53h */
        g_reply_buf[g_reply_len++] = IOP_MC_ID1;
        g_reply_buf[g_reply_len++] = IOP_MC_ID2;
        g_reply_buf[g_reply_len++] = IOP_MC_ACK1;
        g_reply_buf[g_reply_len++] = IOP_MC_ACK2;
        /* real cited fixed reply bytes (Sony-cards-only command) */
        g_reply_buf[g_reply_len++] = 0x04;
        g_reply_buf[g_reply_len++] = 0x00;
        g_reply_buf[g_reply_len++] = 0x00;
        g_reply_buf[g_reply_len++] = 0x80;
        return;
    }

    /* real cited behavior: invalid command byte -> transfer aborts
     * immediately after the faulty command byte. */
    g_reply_buf[g_reply_len++] = g_mc.flag; /* reply to the invalid cmd
                                               * byte is still FLAG, per
                                               * doc's uniform "Send cmd,
                                               * Receive FLAG" framing */
}

int iop_sio2_mmio_read8(uint32_t addr, uint8_t *out)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SIO2_BASE || phys >= IOP_SIO2_BASE + IOP_SIO2_SIZE)
        return 0;
    uint32_t off = phys - IOP_SIO2_BASE;

    if (off == OFF_FIFOOUT) {
        if (g_reply_pos < g_reply_len)
            *out = g_reply_buf[g_reply_pos++];
        else
            *out = 0xFF; /* real cited High-Z-style idle value once the
                           * reply buffer is exhausted */
        return 1;
    }

    *out = g_regs[off];
    return 1;
}

int iop_sio2_mmio_write8(uint32_t addr, uint8_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SIO2_BASE || phys >= IOP_SIO2_BASE + IOP_SIO2_SIZE)
        return 0;
    uint32_t off = phys - IOP_SIO2_BASE;

    if (off == OFF_FIFOIN) {
        if (g_cmd_len < MC_CMD_MAX)
            g_cmd_buf[g_cmd_len++] = value;
        g_regs[off] = value;
        return 1;
    }
    if (off == OFF_CTRL) {
        g_regs[off] = value;
        if (value & 0x01u) { /* real cited "bit 0 starts the command
                               * transfer" */
            mc_process_command();
            g_cmd_len = 0;
        }
        if (value & 0x0Cu) { /* real cited "bits 2 and 3 reset SIO2" */
            g_cmd_len = 0;
            g_reply_len = 0;
            g_reply_pos = 0;
        }
        return 1;
    }

    g_regs[off] = value;
    return 1;
}

int iop_sio2_mmio_read32(uint32_t addr, uint32_t *out)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SIO2_BASE || phys + 4u > IOP_SIO2_BASE + IOP_SIO2_SIZE)
        return 0;
    uint32_t off = phys - IOP_SIO2_BASE;

    if (off == OFF_FIFOOUT) {
        uint8_t b;
        iop_sio2_mmio_read8(addr, &b);
        *out = b;
        return 1;
    }

    *out = (uint32_t)g_regs[off] | ((uint32_t)g_regs[off + 1] << 8) |
           ((uint32_t)g_regs[off + 2] << 16) | ((uint32_t)g_regs[off + 3] << 24);
    return 1;
}

int iop_sio2_mmio_write32(uint32_t addr, uint32_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SIO2_BASE || phys + 4u > IOP_SIO2_BASE + IOP_SIO2_SIZE)
        return 0;
    uint32_t off = phys - IOP_SIO2_BASE;

    if (off == OFF_FIFOIN) {
        iop_sio2_mmio_write8(addr, (uint8_t)(value & 0xFFu));
        return 1;
    }
    if (off == OFF_CTRL) {
        iop_sio2_mmio_write8(addr, (uint8_t)(value & 0xFFu));
        g_regs[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
        g_regs[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
        g_regs[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
        return 1;
    }

    g_regs[off]     = (uint8_t)(value & 0xFFu);
    g_regs[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    g_regs[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
    g_regs[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
    return 1;
}
