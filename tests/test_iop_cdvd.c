/*
 * test_iop_cdvd.c - host-native test for the CDVD register scaffold
 * (ROADMAP section 7's "CDVD - disc/BIOS-boot-media emulation" item).
 *
 * See include/core/hw/iop_cdvd.h for the full design rationale and
 * citation trail (register offsets, power-on defaults, and the
 * ERROR/BREAK read-behavior quirks are all ported directly from
 * PCSX2's own pcsx2/CDVD/CDVD.cpp `cdvdReset()`/`cdvdRead()`, not
 * reinvented).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "core/hw/iop_cdvd.h"
#include "core/hw/iop_dma.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    iop_cdvd_init();

    /* --- Real cdvdReset() power-on defaults (diskless boot case) --- */
    {
        uint8_t v;
        /* Round 259 (task #419): real BIOS disassembly (EECONF module,
         * IOP addr 0x0010C070) + an independent real 2003 community
         * reimplementation source both show bit 3 (0x08) of this
         * register is polled and expected - see iop_cdvd.h's
         * IOP_CDVD_NREADY_CONFIG_READY citation for the full trail. */
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x05u, &v) && v == (IOP_CDVD_DRIVE_READY | IOP_CDVD_NREADY_CONFIG_READY | IOP_CDVD_NREADY_CONFIG2_READY),
              "N-READY (offset 0x05) defaults to CDVD_DRIVE_READY | CDVD_NREADY_CONFIG_READY | CDVD_NREADY_CONFIG2_READY (0x4A)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x0Au, &v) && v == IOP_CDVD_STATUS_TRAY_OPEN,
              "STATUS (offset 0x0A) defaults to CDVD_STATUS_TRAY_OPEN (0x01)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x0Bu, &v) && v == IOP_CDVD_STATUS_TRAY_OPEN,
              "STATUS STICKY (offset 0x0B) defaults to CDVD_STATUS_TRAY_OPEN too");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x0Fu, &v) && v == IOP_CDVD_TYPE_NODISC,
              "TYPE (offset 0x0F) defaults to CDVD_TYPE_NODISC (0x00)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x08u, &v) && v == 0u,
              "INTR_STAT (offset 0x08) defaults to 0 (no pending interrupt)");
        CHECK(iop_cdvd_get_status() == IOP_CDVD_STATUS_TRAY_OPEN, "accessor: get_status matches register");
        CHECK(iop_cdvd_get_ready() == (IOP_CDVD_DRIVE_READY | IOP_CDVD_NREADY_CONFIG_READY | IOP_CDVD_NREADY_CONFIG2_READY), "accessor: get_ready matches register");
        CHECK(iop_cdvd_get_disc_type() == IOP_CDVD_TYPE_NODISC, "accessor: get_disc_type matches register");
    }

    /* --- ERROR register (offset 0x06): read-clears real behavior --- */
    {
        uint8_t v;
        CHECK(iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x06u, 0x42u), "write to ERROR register accepted");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x06u, &v) && v == 0x42u,
              "first read of ERROR returns the written value");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x06u, &v) && v == 0u,
              "second read of ERROR returns 0 - real hardware clears it on read");
    }

    /* --- BREAK register (offset 0x07): always reads 0 --- */
    {
        uint8_t v;
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x07u, 0xFFu);
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x07u, &v) && v == 0u,
              "BREAK register always reads back 0 regardless of what was written");
    }

    /* --- NCMD register (offset 0x04): latched + triggers a
     * plausible completion IRQ instead of staying busy forever --- */
    {
        uint8_t v;
        CHECK(iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x04u, 0x0Cu), "write NCMD=0x0C accepted");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x04u, &v) && v == 0x0Cu,
              "NCMD readback matches what was written (real hardware latches it)");
        CHECK(iop_cdvd_get_last_ncommand() == 0x0Cu, "accessor: get_last_ncommand matches");
        /* Round 206: corrected per ps2tek's directly-cited, per-bit
         * CDVD I_STAT table (bit1 = "(N?) Command complete") - the
         * older placeholder treated this register as a single enum
         * code (0 = "Irq_CommandComplete"); the newly-fetched, more
         * precise real spec shows it's a bitmask, and every real
         * N-command sets bit1 on completion. Superseding the older,
         * vaguer expectation with the better-cited one, not silently
         * changing behavior undocumented. */
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x08u, &v) && (v & 0x02u),
              "writing NCMD sets INTR_STAT bit1 (command complete) - ps2tek-cited");
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x08u, 0xFFu); /* real 'write 1 to clear' ack, per ps2tek */
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x08u, &v) && v == 0u,
              "writing 0xFF to INTR_STAT (ack) clears all pending bits");
    }

    /* --- Round 206 (task #366): real N-command ReadCd sector-read
     * data path. Builds a small, entirely synthetic ISO9660 image
     * (public, non-proprietary format - see iso_loader.h's own scope
     * note) with one file, mounts it via iop_cdvd_mount_iso(), issues
     * a real ReadCd (06h) command through the register interface
     * exactly as real BIOS/kernel code would (params written to
     * 0x05 before the command byte to 0x04), and verifies the real
     * sector bytes land in "IOP RAM" at the CDVD DMA channel's (3)
     * current MADR - see iop_cdvd.h's citation (ps2tek's CDVD N
     * Commands / Reads and Seeks pages) for the exact real protocol
     * this reproduces. --- */
    {
        const char *path = "/tmp/test_cdvd_synth.iso";
        const uint32_t root_lba = 20;
        const uint32_t file_lba = 21;
        const char *file_name = "SYSTEM.CNF;1";
        const char *file_contents = "BOOT2 = cdrom0:\\SLUS_123.45;1\r\n";
        uint32_t file_len = (uint32_t)strlen(file_contents);

        FILE *fp = fopen(path, "wb");
        uint8_t sec[2048];
        memset(sec, 0, sizeof(sec));
        for (int i = 0; i < 16; i++) fwrite(sec, 1, sizeof(sec), fp);

        uint8_t pvd[2048]; memset(pvd, 0, sizeof(pvd));
        pvd[0] = 1; memcpy(&pvd[1], "CD001", 5); pvd[6] = 1;
        uint8_t *root_rec = &pvd[156];
        root_rec[0] = 34;
        root_rec[2] = (uint8_t)(root_lba & 0xFF); root_rec[3] = (uint8_t)((root_lba >> 8) & 0xFF);
        root_rec[10] = (uint8_t)(2048 & 0xFF); root_rec[11] = (uint8_t)((2048 >> 8) & 0xFF);
        root_rec[25] = 0x02; root_rec[32] = 1; root_rec[33] = 0x00;
        fwrite(pvd, 1, sizeof(pvd), fp);

        uint8_t term[2048]; memset(term, 0, sizeof(term));
        term[0] = 255; memcpy(&term[1], "CD001", 5);
        fwrite(term, 1, sizeof(term), fp);
        fwrite(sec, 1, sizeof(sec), fp); /* sector 18 */
        fwrite(sec, 1, sizeof(sec), fp); /* sector 19 */

        uint8_t dir[2048]; memset(dir, 0, sizeof(dir));
        uint32_t off = 0;
        dir[off+0]=34; dir[off+2]=(uint8_t)(root_lba&0xFF); dir[off+3]=(uint8_t)((root_lba>>8)&0xFF);
        dir[off+10]=(uint8_t)(2048&0xFF); dir[off+11]=(uint8_t)((2048>>8)&0xFF);
        dir[off+25]=0x02; dir[off+32]=1; dir[off+33]=0x00; off+=34;
        dir[off+0]=34; dir[off+2]=(uint8_t)(root_lba&0xFF); dir[off+3]=(uint8_t)((root_lba>>8)&0xFF);
        dir[off+10]=(uint8_t)(2048&0xFF); dir[off+11]=(uint8_t)((2048>>8)&0xFF);
        dir[off+25]=0x02; dir[off+32]=1; dir[off+33]=0x01; off+=34;
        uint8_t name_len = (uint8_t)strlen(file_name);
        uint8_t rec_len = (uint8_t)(33 + name_len);
        dir[off+0]=rec_len;
        dir[off+2]=(uint8_t)(file_lba&0xFF); dir[off+3]=(uint8_t)((file_lba>>8)&0xFF);
        dir[off+10]=(uint8_t)(file_len&0xFF); dir[off+11]=(uint8_t)((file_len>>8)&0xFF);
        dir[off+25]=0x00; dir[off+32]=name_len;
        memcpy(&dir[off+33], file_name, name_len);
        fwrite(dir, 1, sizeof(dir), fp);

        uint8_t file_sector[2048]; memset(file_sector, 0, sizeof(file_sector));
        memcpy(file_sector, file_contents, file_len);
        fwrite(file_sector, 1, sizeof(file_sector), fp);
        fclose(fp);

        iop_cdvd_init();
        CHECK(iop_cdvd_mount_iso(path) == 0, "iop_cdvd_mount_iso succeeds on synthetic image");

        static uint8_t fake_iop_ram[65536];
        iop_dma_init();
        iop_dma_bind_iop_ram(fake_iop_ram, sizeof(fake_iop_ram));
        iop_dma_state_t *dma = iop_dma_get_state();
        dma->ch[3].madr = 0x1000u; /* arbitrary test destination, channel 3 = CDROM per iop_dma.h's own channel table */

        /* Real protocol: params written to 0x05 BEFORE the command
         * byte to 0x04 - sector position (LE, bytes 0-3) then sector
         * count (LE, bytes 4-7). */
        uint8_t params[8];
        params[0] = (uint8_t)(file_lba & 0xFF); params[1] = (uint8_t)((file_lba >> 8) & 0xFF);
        params[2] = (uint8_t)((file_lba >> 16) & 0xFF); params[3] = (uint8_t)((file_lba >> 24) & 0xFF);
        params[4] = 1; params[5] = 0; params[6] = 0; params[7] = 0; /* 1 sector */
        for (int i = 0; i < 8; i++)
            iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x05u, params[i]);
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x04u, 0x06u); /* NCMD_READCD */

        CHECK(memcmp(fake_iop_ram + 0x1000u, file_contents, file_len) == 0,
              "ReadCd delivers the real synthetic SYSTEM.CNF sector bytes into IOP RAM at channel 3's MADR");
        CHECK(dma->ch[3].madr == 0x1000u + 2048u,
              "channel 3 MADR auto-advances by one real 2048-byte sector after the transfer");

        uint8_t istat;
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x08u, &istat) && (istat & 0x01u) && (istat & 0x02u),
              "successful ReadCd raises BOTH I_STAT bit0 (data ready) and bit1 (command complete) - ps2tek-cited");

        iop_cdvd_unmount_iso();
        remove(path);
    }

    /* --- Real hardware mirrors the byte registers across the whole
     * 4KB page (address masked to low 8 bits by PCSX2's own
     * psxHw4Read8/Write8) - verify that mirroring is replicated. --- */
    {
        /* Round 206: switched from offset 0x05 to 0x0A (STATUS) for
         * this check - 0x05's write side is now the real N-command
         * parameter FIFO (see this file's own citation above), a
         * genuinely different register from its read side, so a
         * plain write-then-read-back no longer applies to it. 0x0A
         * remains a plain read/write byte in this model, same as
         * before. */
        uint8_t v1, v2;
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x0Au, 0x99u);
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x10Au, &v1) && v1 == 0x99u,
              "register at +0x100 mirrors the same byte as +0x00 (page mirroring)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0xF0Au, &v2) && v2 == 0x99u,
              "register at +0xF00 also mirrors the same byte (full 4KB page)");
    }

    /* --- Round 170: iop_cdvd_set_disc_present() - real disc-loaded
     * state, evidence-based per real opened image (see iop_cdvd.h's
     * citation for IOP_CDVD_TYPE_PS2CD/IOP_CDVD_STATUS_PAUSE, PCSX2's
     * real cdvdCtrlTrayClose() fast-boot path) --- */
    {
        iop_cdvd_init(); /* reset to the diskless default first */
        iop_cdvd_set_disc_present(IOP_CDVD_TYPE_PS2CD);
        CHECK(iop_cdvd_get_disc_type() == IOP_CDVD_TYPE_PS2CD,
              "set_disc_present: TYPE becomes IOP_CDVD_TYPE_PS2CD (0x12)");
        CHECK(iop_cdvd_get_status() == IOP_CDVD_STATUS_PAUSE,
              "set_disc_present: STATUS becomes IOP_CDVD_STATUS_PAUSE (0x0A), not TRAY_OPEN");
        CHECK(iop_cdvd_get_ready() == (IOP_CDVD_DRIVE_READY | IOP_CDVD_NREADY_CONFIG_READY | IOP_CDVD_NREADY_CONFIG2_READY),
              "set_disc_present: READY stays/becomes CDVD_DRIVE_READY | CDVD_NREADY_CONFIG_READY | CDVD_NREADY_CONFIG2_READY (0x4A)");
        uint8_t v;
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x0Bu, &v) && (v & IOP_CDVD_STATUS_PAUSE),
              "set_disc_present: STATUS_STICKY ORs in PAUSE (real cdvdUpdateStatus() |= behavior)");
        /* re-init so the rest of the suite (if extended later) starts clean */
        iop_cdvd_init();
        CHECK(iop_cdvd_get_disc_type() == IOP_CDVD_TYPE_NODISC,
              "iop_cdvd_init() after set_disc_present() correctly returns to the diskless default");
    }

    /* --- Round 261 (task #422): S-command register block
     * (SCOMMAND=0x16, SDATAIN=0x17, SDATAOUT=0x18). Real protocol
     * verified via fresh disassembly of this project's own mounted
     * retail BIOS's sceCdSCmd()-equivalent function + ps2tek's
     * dedicated SCMD page - see iop_cdvd.h's citation for the full
     * trail. --- */
    {
        iop_cdvd_init();
        uint8_t v;

        /* Idle state: no result queued yet, so SDATAIN must report
         * NODATA (bit 0x40 set) - this is the exact bit the real
         * BIOS's initial "drain stale data" loop polls before it will
         * even attempt to send a command; if this reads wrong, that
         * loop spins forever (this was Round 260's measured hang
         * point, IOP pc 0x0010BB7C). */
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x17u, &v) && (v & IOP_CDVD_SDATAIN_NODATA),
              "SDATAIN idle: NODATA bit set before any S-command is issued");
        CHECK(!(v & IOP_CDVD_SDATAIN_BUSY),
              "SDATAIN idle: BUSY bit clear before any S-command is issued");

        /* OpenConfig (0x40): real, cited 1-byte zero result (ps2tek:
         * "Dobiestation returns zero"). */
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x16u, SCMD_OPENCONFIG);
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x17u, &v) && !(v & IOP_CDVD_SDATAIN_NODATA),
              "after OpenConfig: SDATAIN reports data available (NODATA bit clear)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x18u, &v) && v == 0x00u,
              "OpenConfig result byte 0 == 0x00 (real, cited value)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x17u, &v) && (v & IOP_CDVD_SDATAIN_NODATA),
              "after draining OpenConfig's one result byte, NODATA bit sets again - real BIOS's result-read loop relies on exactly this to know when to stop");

        /* ReadConfig (0x41): real, cited 4x32-bit-word (16 byte)
         * result. */
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x16u, SCMD_READCONFIG);
        int drained = 0;
        while (1) {
            CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x17u, &v), "SDATAIN read ok during ReadConfig drain");
            if (v & IOP_CDVD_SDATAIN_NODATA) break;
            uint8_t b;
            CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x18u, &b) && b == 0x00u,
                  "ReadConfig result byte is honestly zero-filled (no real config block modeled)");
            drained++;
            if (drained > 32) break; /* safety - don't let a test bug spin forever */
        }
        CHECK(drained == 16, "ReadConfig delivers exactly 16 bytes (four 32-bit words) - real, cited count per ps2tek's SCMD page");

        /* CloseConfig (0x43) and an unmodeled command (WriteConfig,
         * 0x42) both still resolve to a real, non-busy state rather
         * than hanging - honest generic-ack scope, same convention as
         * dispatch_ncmd()'s own unimplemented commands. */
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x16u, SCMD_CLOSECONFIG);
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x17u, &v) && !(v & IOP_CDVD_SDATAIN_BUSY),
              "CloseConfig: BUSY bit clear (resolves synchronously)");
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x16u, SCMD_WRITECONFIG);
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x17u, &v) && !(v & IOP_CDVD_SDATAIN_BUSY),
              "unmodeled S-command (WriteConfig): BUSY bit still clear, does not hang the caller");

        iop_cdvd_init(); /* leave clean for the rest of the suite */
    }

    /* --- Addresses outside the CDVD window are correctly rejected --- */
    {
        uint8_t v;
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE - 4u, &v) == 0,
              "address just below IOP_CDVD_BASE is correctly rejected (returns 0/not-handled)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x1000u, &v) == 0,
              "address at the end of the 4KB page (+0x1000) is correctly rejected");
    }

    if (failures) {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("\nAll checks passed.\n");
    return 0;
}
