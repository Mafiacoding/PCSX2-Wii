/*
 * test_iop_sio2_mc.c - host-native regression test for the real
 * memory-card command/response protocol implemented on top of SIO2
 * (Round 146, task #299). See include/core/hw/iop_sio2.h for the
 * full citation trail (psx-spx Memory Card Read/Write Commands +
 * ps2sdk mcsio2.c McReadPS1PDACard/McWritePS1PDACard).
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "hw/iop_sio2.c"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s\n", msg); } \
} while (0)

static void send_byte(uint8_t v)
{
    iop_sio2_mmio_write8(IOP_SIO2_BASE + 0x60, v); /* FIFOIN */
}

static void start_transfer(void)
{
    iop_sio2_mmio_write8(IOP_SIO2_BASE + 0x68, 0x01); /* CTRL bit0 */
}

static uint8_t recv_byte(void)
{
    uint8_t v = 0;
    iop_sio2_mmio_read8(IOP_SIO2_BASE + 0x64, &v); /* FIFOOUT */
    return v;
}

/* Sends the full Read Sector command stream (140 bytes total: addr,
 * cmd, 2 dummy id-pump bytes, MSB, LSB, 2 dummy ack-pump bytes, 2
 * dummy confirmed-addr-pump bytes, 128 dummy data-pump bytes, 1 dummy
 * checksum-pump byte, 1 dummy end-pump byte) and returns the reply
 * bytes into out[] (caller-provided, must be >=140 bytes). Returns
 * actual reply length. */
static int do_read(uint8_t msb, uint8_t lsb, uint8_t *out)
{
    send_byte(IOP_MC_ADDR_BYTE);
    send_byte(IOP_MC_CMD_READ);
    send_byte(0x00); send_byte(0x00); /* id pump */
    send_byte(msb);
    send_byte(lsb);
    send_byte(0x00); send_byte(0x00); /* ack pump */
    send_byte(0x00); send_byte(0x00); /* confirmed-addr pump */
    int i;
    for (i = 0; i < 128; i++) send_byte(0x00);
    send_byte(0x00); /* checksum pump */
    send_byte(0x00); /* end pump */
    start_transfer();
    int n = 0;
    while (g_reply_pos < g_reply_len)
        out[n++] = recv_byte();
    return n;
}

static int do_write(uint8_t msb, uint8_t lsb, const uint8_t *data, uint8_t chk, uint8_t *out)
{
    send_byte(IOP_MC_ADDR_BYTE);
    send_byte(IOP_MC_CMD_WRITE);
    send_byte(0x00); send_byte(0x00); /* id pump */
    send_byte(msb);
    send_byte(lsb);
    int i;
    for (i = 0; i < 128; i++) send_byte(data[i]);
    send_byte(chk);
    send_byte(0x00); send_byte(0x00); /* ack pump */
    send_byte(0x00); /* end pump */
    start_transfer();
    int n = 0;
    while (g_reply_pos < g_reply_len)
        out[n++] = recv_byte();
    return n;
}

int main(void)
{
    uint8_t out[256];
    int n;

    /* --- No card inserted: real High-Z (FFh) convention --- */
    iop_sio2_init();
    n = do_read(0x00, 0x00, out);
    CHECK(n > 0, "no-card read produced some reply bytes");
    {
        int all_ff = 1, i;
        for (i = 0; i < n; i++) if (out[i] != 0xFF) all_ff = 0;
        CHECK(all_ff, "no-card read replies are all 0xFF (High-Z)");
    }

    /* --- Insert a blank card --- */
    iop_sio2_init();
    iop_sio2_mc_insert_blank();
    CHECK(iop_sio2_mc_is_inserted(), "card reports inserted after insert_blank");

    /* --- Read sector 0 of a blank card --- */
    n = do_read(0x00, 0x00, out);
    CHECK(n == 140, "blank-card read sector 0 produces 140 reply bytes");
    CHECK(out[0] == 0x00, "reply[0] dummy byte for 81h");
    CHECK(out[1] == IOP_MC_FLAG_INIT, "reply[1] FLAG == real init value 08h");
    CHECK(out[2] == IOP_MC_ID1 && out[3] == IOP_MC_ID2, "reply[2..3] real MC ID bytes 5Ah/5Dh");
    CHECK(out[6] == IOP_MC_ACK1 && out[7] == IOP_MC_ACK2, "reply[6..7] real ack bytes 5Ch/5Dh");
    CHECK(out[8] == 0x00 && out[9] == 0x00, "reply[8..9] confirmed address echoes sector 0");
    {
        int all_zero = 1, i;
        for (i = 0; i < 128; i++) if (out[10 + i] != 0x00) all_zero = 0;
        CHECK(all_zero, "blank sector 0 data is all zero");
    }
    CHECK(out[138] == 0x00, "checksum of all-zero sector/addr is 0");
    CHECK(out[139] == IOP_MC_END_GOOD, "read end byte is real 47h Good");

    /* --- Write sector 5, then read it back --- */
    uint8_t wdata[128];
    {
        int i;
        for (i = 0; i < 128; i++) wdata[i] = (uint8_t)(i * 3 + 7);
    }
    uint8_t chk = (uint8_t)(0x00 ^ 0x05);
    {
        int i;
        for (i = 0; i < 128; i++) chk ^= wdata[i];
    }
    n = do_write(0x00, 0x05, wdata, chk, out);
    CHECK(n == 138, "write sector 5 produces 138 reply bytes");
    CHECK(out[n - 1] == IOP_MC_END_GOOD, "write end byte is real 47h Good on correct checksum");
    CHECK(out[n - 3] == IOP_MC_ACK1 && out[n - 2] == IOP_MC_ACK2, "write ack bytes present before end byte");

    n = do_read(0x00, 0x05, out);
    CHECK(n == 140, "read-back sector 5 produces 140 reply bytes");
    {
        int match = 1, i;
        for (i = 0; i < 128; i++) if (out[10 + i] != wdata[i]) match = 0;
        CHECK(match, "read-back sector 5 data matches what was written");
    }
    CHECK((g_mc.flag & IOP_MC_FLAG_DIRTY) == 0, "FLAG bit3 cleared after a successful write (real cited behavior)");

    /* --- Bad checksum write --- */
    n = do_write(0x00, 0x06, wdata, (uint8_t)(chk ^ 0xFF), out);
    CHECK(out[n - 1] == IOP_MC_END_BADCHECKSUM, "write with wrong checksum returns real 4Eh BadChecksum");

    /* --- Invalid sector read (>= 0x400) --- */
    n = do_read(0x04, 0x00, out); /* sector 0x400, out of range */
    CHECK(n == 10, "invalid-sector read aborts after confirmed-address bytes (10 total)");
    CHECK(out[8] == 0xFF && out[9] == 0xFF, "invalid sector read returns real FFFFh confirmed address");

    /* --- Invalid sector write --- */
    n = do_write(0x04, 0x00, wdata, 0x00, out);
    CHECK(out[n - 1] == IOP_MC_END_BADSECTOR, "invalid-sector write returns real FFh BadSector");

    /* --- Get MC ID command --- */
    iop_sio2_init();
    iop_sio2_mc_insert_blank();
    send_byte(IOP_MC_ADDR_BYTE);
    send_byte(IOP_MC_CMD_GETID);
    send_byte(0x00); send_byte(0x00);
    send_byte(0x00); send_byte(0x00);
    send_byte(0x00); send_byte(0x00);
    send_byte(0x00); send_byte(0x00);
    start_transfer();
    n = 0;
    while (g_reply_pos < g_reply_len) out[n++] = recv_byte();
    CHECK(n == 10, "GetID produces 10 reply bytes");
    CHECK(out[1] == IOP_MC_FLAG_INIT, "GetID reply[1] is FLAG");
    CHECK(out[2] == IOP_MC_ID1 && out[3] == IOP_MC_ID2, "GetID reply[2..3] real ID bytes");
    CHECK(out[4] == IOP_MC_ACK1 && out[5] == IOP_MC_ACK2, "GetID reply[4..5] real ack bytes");
    CHECK(out[6] == 0x04 && out[7] == 0x00 && out[8] == 0x00 && out[9] == 0x80,
          "GetID reply[6..9] real fixed bytes 04,00,00,80");

    /* --- Invalid command byte --- */
    iop_sio2_init();
    iop_sio2_mc_insert_blank();
    send_byte(IOP_MC_ADDR_BYTE);
    send_byte(0x99); /* not R/W/S */
    start_transfer();
    n = 0;
    while (g_reply_pos < g_reply_len) out[n++] = recv_byte();
    CHECK(n == 2, "invalid command aborts after 2 reply bytes");

    /* --- Eject --- */
    iop_sio2_mc_eject();
    CHECK(!iop_sio2_mc_is_inserted(), "card reports not inserted after eject");

    printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail != 0;
}
