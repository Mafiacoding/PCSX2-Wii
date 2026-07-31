/*
 * test_iop_cdrom_legacy.c - host-native test for Round 145's real
 * PS1-legacy CD-ROM controller command/response protocol
 * (source/hw/iop_cdrom_legacy.c). See the header's citation trail.
 *
 * Builds a small, entirely synthetic ISO9660 image on disk (same
 * technique as test_iso_loader.c - public, non-proprietary format,
 * not PS2 BIOS/game content) to exercise the real disc-backed read
 * path, plus MMIO-level command/response sequencing against the
 * no-disc case.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* Round 153 (task #307): iop_cdrom_legacy.c now links against this - see its own test_iop_asyncio.c for dedicated coverage. */
#include "hw/iop_asyncio.c"
#include "hw/iop_cdrom_legacy.c"
#include "hw/iop_intc.c"
#include "core/iop/iop_core.h"
#include "hw/iop_hle_events.c"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("ok:   %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

static void write_le32(uint8_t *p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static void write_be32(uint8_t *p, uint32_t v) { p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF; }
static void write_both32(uint8_t *p, uint32_t v) { write_le32(p, v); write_be32(p+4, v); }

/* Builds a synthetic single-file ISO9660 image (PVD at sector 16,
 * root dir at sector 20, one file "SYSTEM.CNF;1" at sector 21 filled
 * with a recognizable byte pattern) so ReadN's returned bytes can be
 * checked against known content. */
static void build_synth_iso(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint8_t sector[2048];

    for (uint32_t s = 0; s < 22; s++) {
        memset(sector, 0, sizeof(sector));
        if (s == 16) {
            sector[0] = 1;
            memcpy(&sector[1], "CD001", 5);
            uint8_t *root = &sector[156];
            root[0] = 34;
            write_both32(&root[2], 20);   /* extent LBA */
            write_both32(&root[10], 2048); /* data length */
            root[25] = 0x02; /* directory flag */
            root[32] = 1;
            root[33] = 0;
        } else if (s == 20) {
            uint8_t *e;
            e = &sector[0];
            e[0] = 34; write_both32(&e[2], 20); write_both32(&e[10], 2048); e[25]=0x02; e[32]=1; e[33]=0;
            e = &sector[34];
            e[0] = 34; write_both32(&e[2], 20); write_both32(&e[10], 2048); e[25]=0x02; e[32]=1; e[33]=1;
            e = &sector[68];
            const char *name = "SYSTEM.CNF;1";
            uint8_t namelen = (uint8_t)strlen(name);
            e[0] = (uint8_t)(33 + namelen + (1 - (namelen & 1)));
            write_both32(&e[2], 21);
            write_both32(&e[10], 2048);
            e[25] = 0x00;
            e[32] = namelen;
            memcpy(&e[33], name, namelen);
        } else if (s == 21) {
            for (int i = 0; i < 2048; i++) sector[i] = (uint8_t)(i & 0xFF);
        }
        fwrite(sector, 1, sizeof(sector), f);
    }
    fclose(f);
}

static uint8_t hsts(void) { uint8_t v; iop_cdrom_legacy_mmio_read8(0x1F801800u, &v); return v; }
static void set_bank(uint8_t b) { iop_cdrom_legacy_mmio_write8(0x1F801800u, b); }
static void write_cmd(uint8_t cmd) { set_bank(0); iop_cdrom_legacy_mmio_write8(0x1F801801u, cmd); }
static void write_param(uint8_t v) { set_bank(0); iop_cdrom_legacy_mmio_write8(0x1F801802u, v); }
static uint8_t read_result(void) { uint8_t v; iop_cdrom_legacy_mmio_read8(0x1F801801u, &v); return v; }
static uint8_t read_intsts(void) { set_bank(1); uint8_t v; iop_cdrom_legacy_mmio_read8(0x1F801803u, &v); return v & 0x07u; }
static void set_intmsk_all(void) { set_bank(1); iop_cdrom_legacy_mmio_write8(0x1F801802u, 0x1Fu); }
static void ack_int(void) { set_bank(1); iop_cdrom_legacy_mmio_write8(0x1F801803u, 0x07u); }

int main(void)
{
    iop_intc_init();
    iop_cdrom_legacy_init();
    iop_hle_events_init();
    set_intmsk_all();

    /* Nop -> INT3(stat) */
    write_cmd(0x01);
    CHECK(read_intsts() == IOP_CDROM_INT3, "Nop raises INT3");
    CHECK((read_result() & IOP_CDROM_STAT_ERROR) == 0, "Nop stat has no error bit");
    CHECK((iop_intc_get_state()->istat & (1u << 2)) != 0, "Nop raised real IOP IRQ2");
    ack_int();

    /* Unimplemented opcode -> INT5, error byte 40h */
    write_cmd(0xFE);
    CHECK(read_intsts() == IOP_CDROM_INT5, "unimplemented opcode raises INT5");
    uint8_t stat1 = read_result();
    uint8_t err1 = read_result();
    CHECK((stat1 & IOP_CDROM_STAT_ERROR) != 0, "unimplemented opcode sets Error stat bit");
    CHECK(err1 == 0x40u, "unimplemented opcode error byte is 40h (Invalid command)");
    ack_int();

    /* GetTN with no disc mounted -> real 'no disc' INT5/80h error */
    write_cmd(0x13);
    CHECK(read_intsts() == IOP_CDROM_INT5, "GetTN with no disc raises INT5");
    uint8_t s2 = read_result(); uint8_t e2 = read_result();
    CHECK((s2 & IOP_CDROM_STAT_ERROR) != 0, "no-disc GetTN sets Error bit");
    CHECK(e2 == 0x80u, "no-disc GetTN error byte is 80h (cannot respond/no disk)");
    ack_int();

    /* Mount a real synthetic ISO */
    const char *path = "/tmp/test_cdrom_synth.iso";
    build_synth_iso(path);
    CHECK(iop_cdrom_legacy_mount_iso(path) == 0, "mount synthetic ISO succeeds");

    /* GetTN now succeeds */
    write_cmd(0x13);
    CHECK(read_intsts() == IOP_CDROM_INT3, "GetTN with disc mounted raises INT3");
    uint8_t stat3 = read_result();
    CHECK((stat3 & IOP_CDROM_STAT_ERROR) == 0, "mounted GetTN has no error");
    ack_int();

    /* Setloc to sector 21 (abs frame 21+150=171 -> 02:21:21 MSF) then
     * ReadN and verify real sector bytes come back via RDDATA. */
    uint32_t abs_frame = 21 + 150;
    uint32_t mm = abs_frame / (60*75);
    uint32_t rem = abs_frame % (60*75);
    uint32_t ss = rem / 75;
    uint32_t ff = rem % 75;
    write_param(bin_to_bcd(mm));
    write_param(bin_to_bcd(ss));
    write_param(bin_to_bcd(ff));
    write_cmd(0x02); /* Setloc, params already queued in FIFO */
    CHECK(read_intsts() == IOP_CDROM_INT3, "Setloc raises INT3");
    ack_int();

    write_cmd(0x06); /* ReadN */
    CHECK(read_intsts() == IOP_CDROM_INT3, "ReadN first response is INT3");
    ack_int();
    CHECK(read_intsts() == IOP_CDROM_INT1, "ReadN second response is INT1 (data ready)");
    CHECK((hsts() & IOP_CDROM_LEGACY_STATUS_DRQSTS) != 0, "DRQSTS set after ReadN data-ready");
    uint8_t b0, b1, b2;
    iop_cdrom_legacy_mmio_read8(0x1F801802u, &b0);
    iop_cdrom_legacy_mmio_read8(0x1F801802u, &b1);
    iop_cdrom_legacy_mmio_read8(0x1F801802u, &b2);
    CHECK(b0 == 0 && b1 == 1 && b2 == 2, "RDDATA returns real synthetic sector bytes (0,1,2,...)");
    ack_int();

    /* Two-phase INT3->INT2 command: Pause */
    write_cmd(0x09);
    CHECK(read_intsts() == IOP_CDROM_INT3, "Pause first response is INT3");
    ack_int();
    CHECK(read_intsts() == IOP_CDROM_INT2, "Pause second response is INT2 after ack");
    ack_int();

    /* GetID with disc mounted: two-phase INT3->INT2 */
    write_cmd(0x1A);
    CHECK(read_intsts() == IOP_CDROM_INT3, "GetID first response is INT3");
    ack_int();
    CHECK(read_intsts() == IOP_CDROM_INT2, "GetID second response is INT2 (disc present)");
    ack_int();

    /* Round 148 (task #301): real B0h event delivery for the CD-ROM
     * async driver - class F0000003h (real, cited IRQ2/CDROM event
     * class), spec 0x20/0x8000 (real, cited async-completion pair).
     * OpenEvent(class, spec, EvMdNOINTR, 0) then TestEvent must see
     * ALREADY (1) only after a real terminal CD-ROM interrupt
     * (INT2/INT1) actually fires - not before. */
    iop_state_t evst;
    memset(&evst, 0, sizeof(evst));
    evst.gpr[4] = 0xF0000003u; /* class */
    evst.gpr[5] = 0x0020u;     /* spec */
    evst.gpr[6] = IOP_EVCB_MODE_NOINTR;
    evst.gpr[7] = 0;
    iop_hle_event_open(&evst);
    uint32_t handle20 = evst.gpr[2];
    memset(&evst, 0, sizeof(evst));
    evst.gpr[4] = handle20;
    iop_hle_event_enable(&evst); /* real semantics: OpenEvent alone leaves
                                   * the EvCB in WAIT state; EnableEvent
                                   * moves it to ACTIVE so DeliverEvent can
                                   * act on it (Round 142 lifecycle). */

    memset(&evst, 0, sizeof(evst));
    evst.gpr[4] = handle20;
    iop_hle_event_test(&evst);
    CHECK(evst.gpr[2] == 0, "CDROM class-3/spec-0x20 event not yet delivered before any terminal interrupt");

    /* Pause (0x09) is a real two-phase INT3->INT2 command (already
     * exercised above) - trigger it again fresh and confirm the
     * event now shows delivered. */
    write_cmd(0x09);
    CHECK(read_intsts() == IOP_CDROM_INT3, "Pause (2nd run) first response is INT3");
    ack_int();
    CHECK(read_intsts() == IOP_CDROM_INT2, "Pause (2nd run) second response is INT2 after ack");

    memset(&evst, 0, sizeof(evst));
    evst.gpr[4] = handle20;
    iop_hle_event_test(&evst);
    CHECK(evst.gpr[2] == 1, "CDROM class-3/spec-0x20 event IS delivered after real INT2 fires (task #301 fix)");
    ack_int();

    /* Same check for spec 0x8000, delivered alongside 0x20 on the
     * same INT2/INT1 per the cited pair. */
    memset(&evst, 0, sizeof(evst));
    evst.gpr[4] = 0xF0000003u;
    evst.gpr[5] = 0x8000u;
    evst.gpr[6] = IOP_EVCB_MODE_NOINTR;
    evst.gpr[7] = 0;
    iop_hle_event_open(&evst);
    uint32_t handle8000 = evst.gpr[2];
    memset(&evst, 0, sizeof(evst));
    evst.gpr[4] = handle8000;
    iop_hle_event_enable(&evst);

    write_cmd(0x09); /* Pause again */
    ack_int();       /* clears INT3, triggers pending INT2 */

    memset(&evst, 0, sizeof(evst));
    evst.gpr[4] = handle8000;
    iop_hle_event_test(&evst);
    CHECK(evst.gpr[2] == 1, "CDROM class-3/spec-0x8000 event also delivered on the same real INT2 (task #301 fix)");
    ack_int();

    iop_cdrom_legacy_unmount_iso();
    remove(path);

    printf("\n%d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
