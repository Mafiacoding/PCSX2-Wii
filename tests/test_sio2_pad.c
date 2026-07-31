/* Host-native test for Round 184's real SIO2 controller (pad)
 * protocol - written to include hw/iop_sio2.c directly, matching
 * this project's existing unit-test convention (see tests dir). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "hw/iop_sio2.c"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

static uint8_t send_recv(uint8_t v)
{
    iop_sio2_mmio_write8(IOP_SIO2_BASE + OFF_FIFOIN, v);
    return 0;
}

static void start_transfer(void)
{
    iop_sio2_mmio_write8(IOP_SIO2_BASE + OFF_CTRL, 0x01u);
}

static uint8_t read_out(void)
{
    uint8_t b;
    iop_sio2_mmio_read8(IOP_SIO2_BASE + OFF_FIFOOUT, &b);
    return b;
}

int main(void)
{
    iop_sio2_init();

    /* 1. Default state: pad connected, no buttons pressed. */
    CHECK(iop_sio2_pad_is_connected() == 1, "pad connected by default");
    CHECK(iop_sio2_pad_get_buttons() == 0, "no buttons pressed by default");

    /* 2. Real digital-pad read sequence: 01h 42h 00h 00h 00h */
    send_recv(IOP_PAD_ADDR_BYTE);
    send_recv(IOP_PAD_CMD_READ);
    send_recv(0x00u); /* TAP */
    send_recv(0x00u); /* MOT */
    send_recv(0x00u); /* MOT */
    start_transfer();

    uint8_t r0 = read_out(); /* reply to 01h: dummy */
    uint8_t r1 = read_out(); /* reply to 42h: idlo */
    uint8_t r2 = read_out(); /* reply to TAP: idhi */
    uint8_t r3 = read_out(); /* reply to 1st MOT: swlo */
    uint8_t r4 = read_out(); /* reply to 2nd MOT: swhi */
    (void)r0;
    CHECK(r1 == IOP_PAD_ID_LO, "idlo == 0x41");
    CHECK(r2 == IOP_PAD_ID_HI, "idhi == 0x5A");
    CHECK(r3 == 0xFFu, "swlo == 0xFF (nothing pressed, wire polarity inverted)");
    CHECK(r4 == 0xFFu, "swhi == 0xFF (nothing pressed, wire polarity inverted)");

    /* 3. Press CROSS + START, verify exact wire bits. */
    iop_sio2_pad_set_buttons(IOP_PAD_BTN_CROSS | IOP_PAD_BTN_START);
    CHECK(iop_sio2_pad_get_buttons() == (IOP_PAD_BTN_CROSS | IOP_PAD_BTN_START),
          "pressed-polarity readback matches what was set");

    send_recv(IOP_PAD_ADDR_BYTE);
    send_recv(IOP_PAD_CMD_READ);
    send_recv(0x00u);
    send_recv(0x00u);
    send_recv(0x00u);
    start_transfer();
    read_out(); /* dummy */
    read_out(); /* idlo */
    read_out(); /* idhi */
    uint8_t swlo = read_out();
    uint8_t swhi = read_out();
    /* wire polarity: 0=pressed. START=bit3(swlo), CROSS=bit14(swhi bit6) */
    uint16_t wire = (uint16_t)swlo | ((uint16_t)swhi << 8);
    uint16_t expected_wire = (uint16_t)~(IOP_PAD_BTN_CROSS | IOP_PAD_BTN_START);
    CHECK(wire == expected_wire, "wire bits correctly inverted for pressed buttons");
    CHECK((swlo & (1u << 3)) == 0, "START bit clear (pressed) on wire");
    CHECK((swhi & (1u << 6)) == 0, "CROSS bit (bit14 overall = swhi bit6) clear (pressed) on wire");

    /* 4. press()/release() helpers. */
    iop_sio2_pad_set_buttons(0);
    iop_sio2_pad_press(IOP_PAD_BTN_TRIANGLE);
    CHECK(iop_sio2_pad_get_buttons() == IOP_PAD_BTN_TRIANGLE, "press() sets bit");
    iop_sio2_pad_press(IOP_PAD_BTN_SQUARE);
    CHECK(iop_sio2_pad_get_buttons() == (IOP_PAD_BTN_TRIANGLE | IOP_PAD_BTN_SQUARE), "press() accumulates");
    iop_sio2_pad_release(IOP_PAD_BTN_TRIANGLE);
    CHECK(iop_sio2_pad_get_buttons() == IOP_PAD_BTN_SQUARE, "release() clears only that bit");

    /* 5. Disconnected pad -> all FFh (High-Z), same convention as no memory card. */
    iop_sio2_pad_disconnect();
    CHECK(iop_sio2_pad_is_connected() == 0, "disconnect() works");
    send_recv(IOP_PAD_ADDR_BYTE);
    send_recv(IOP_PAD_CMD_READ);
    send_recv(0x00u);
    send_recv(0x00u);
    send_recv(0x00u);
    start_transfer();
    CHECK(read_out() == 0xFF, "disconnected pad: byte0 FFh");
    CHECK(read_out() == 0xFF, "disconnected pad: byte1 FFh");
    iop_sio2_pad_connect();

    /* 6. Memory-card protocol still works unaffected (regression check
     * against Round 146's existing behavior) - insert blank card, read
     * sector 0, expect all-zero data + Good end byte. */
    iop_sio2_mc_insert_blank();
    uint8_t cmd[6] = { IOP_MC_ADDR_BYTE, IOP_MC_CMD_READ, 0, 0, 0, 0 };
    for (int i = 0; i < 6; i++) send_recv(cmd[i]);
    start_transfer();
    uint8_t mc_r0 = read_out(); (void)mc_r0;
    uint8_t mc_flag = read_out();
    uint8_t mc_id1 = read_out();
    uint8_t mc_id2 = read_out();
    CHECK(mc_flag == IOP_MC_FLAG_INIT, "MC still replies real FLAG after pad addition");
    CHECK(mc_id1 == IOP_MC_ID1 && mc_id2 == IOP_MC_ID2, "MC ID bytes unaffected by pad addition");

    /* 7. Round 195: analog mode - default off, ID stays digital. */
    CHECK(iop_sio2_pad_is_analog_mode() == 0, "analog mode off by default (real cited power-up state)");
    uint8_t rx, ry, lx, ly;
    iop_sio2_pad_get_analog_axes(&rx, &ry, &lx, &ly);
    CHECK(rx == IOP_PAD_ANALOG_CENTER && ry == IOP_PAD_ANALOG_CENTER &&
          lx == IOP_PAD_ANALOG_CENTER && ly == IOP_PAD_ANALOG_CENTER,
          "all 4 analog axes default to real cited Center (0x80)");

    /* 8. Enable analog mode: ID switches to 5A73h, transfer continues
     * past swhi into the 4 real cited adc0-adc3 bytes instead of
     * stopping. */
    iop_sio2_pad_set_analog_mode(1);
    CHECK(iop_sio2_pad_is_analog_mode() == 1, "analog mode enabled");
    iop_sio2_pad_set_analog_axes(0x10, 0x20, 0x30, 0x40);

    uint8_t cmdbuf[9] = { IOP_PAD_ADDR_BYTE, IOP_PAD_CMD_READ, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 9; i++) send_recv(cmdbuf[i]);
    start_transfer();
    read_out(); /* dummy */
    uint8_t a_idlo = read_out();
    uint8_t a_idhi = read_out();
    read_out(); /* swlo */
    read_out(); /* swhi */
    uint8_t a_rx = read_out();
    uint8_t a_ry = read_out();
    uint8_t a_lx = read_out();
    uint8_t a_ly = read_out();
    CHECK(a_idlo == IOP_PAD_ID_ANALOG_LO, "analog-mode idlo == 0x73");
    CHECK(a_idhi == IOP_PAD_ID_ANALOG_HI, "analog-mode idhi == 0x5A");
    CHECK(a_rx == 0x10, "adc0 (RightJoyX) matches set value");
    CHECK(a_ry == 0x20, "adc1 (RightJoyY) matches set value");
    CHECK(a_lx == 0x30, "adc2 (LeftJoyX) matches set value");
    CHECK(a_ly == 0x40, "adc3 (LeftJoyY) matches set value");

    /* 9. A short (5-byte) transfer in analog mode still stops cleanly
     * after swhi, same "if (n < N) return" partial-transfer behavior
     * already exercised for the digital-mode case above - analog mode
     * must not read/write past what was actually clocked. */
    for (int i = 0; i < 5; i++) send_recv(cmdbuf[i]);
    start_transfer();
    read_out(); read_out(); read_out(); read_out(); read_out();
    CHECK(read_out() == 0xFFu, "short analog-mode transfer: FIFOOUT idle (0xFF) past the 5 real bytes");

    /* 10. Disable analog mode again: ID reverts to digital, transfer
     * stops after swhi as before (regression check against the
     * original Round 184 digital-only behavior). */
    iop_sio2_pad_set_analog_mode(0);
    for (int i = 0; i < 5; i++) send_recv(cmdbuf[i]);
    start_transfer();
    read_out();
    CHECK(read_out() == IOP_PAD_ID_LO, "back to digital ID after disabling analog mode");

    if (g_fail == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d check(s) FAILED\n", g_fail);
    return g_fail != 0;
}
