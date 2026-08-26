/*
 * iop_cdvd.c - see include/core/hw/iop_cdvd.h for scope notes.
 */
#include "core/hw/iop_cdvd.h"
#include "core/hw/iop_dma.h"   /* Round 206: iop_dma_channel_write_bytes() */
#include "core/hw/iop_intc.h"  /* Round 206: real IRQ2 raise (shared with legacy CD-ROM, iop_cdrom_legacy.c) */
#include <string.h>

/* Register offsets within the page (real hardware, ps2tek + PCSX2's
 * pcsx2/CDVD/CDVD.cpp cdvdRead/cdvdWrite switch statements). */
#define OFF_NCMD        0x04u
#define OFF_NREADY      0x05u
#define OFF_ERROR       0x06u
#define OFF_BREAK       0x07u
#define OFF_INTR_STAT   0x08u
#define OFF_STATUS      0x0Au
#define OFF_STATUS_STK  0x0Bu
#define OFF_TYPE        0x0Fu
#define OFF_SCOMMAND    0x16u
#define OFF_SDATAIN     0x17u
#define OFF_SDATAOUT    0x18u

/* Round 206: real N-command I_STAT bits (ps2tek's directly-cited,
 * per-bit CDVD I_STAT table - see iop_cdvd.h's citation). */
#define ISTAT_DATA_READY      0x01u
#define ISTAT_COMMAND_COMPLETE 0x02u

/* Round 206: real N-command opcodes (ps2tek "CDVD N Commands"). */
#define NCMD_NOP       0x00u
#define NCMD_NOPSYNC   0x01u
#define NCMD_STANDBY   0x02u
#define NCMD_STOP      0x03u
#define NCMD_PAUSE     0x04u
#define NCMD_SEEK      0x05u
#define NCMD_READCD    0x06u
#define NCMD_READDVD   0x08u
#define NCMD_GETTOC    0x09u

#define CDVD_SECTOR_SIZE 2048u
#define PARAM_BUF_MAX    16

static uint8_t g_regs[IOP_CDVD_SIZE];

/* Round 206: N-command parameter buffer. Real hardware: sequential
 * writes to 1F402005h (write side) each append one raw parameter
 * byte, consumed as a block once a command is written to 1F402004h -
 * see iop_cdvd.h's citation. Reset after each command dispatch. */
static uint8_t g_param_buf[PARAM_BUF_MAX];
static int     g_param_count;

/* Round 261 (task #422): S-command param/result buffers - same
 * buffering convention as g_param_buf/g_param_count above, but for
 * the separate S-command register block (0x16/0x17/0x18) - see
 * iop_cdvd.h's citation for the real register protocol this
 * implements. */
static uint8_t g_sparam_buf[PARAM_BUF_MAX];
static int     g_sparam_count;
static uint8_t g_sresult_buf[PARAM_BUF_MAX];
static int     g_sresult_count;
static int     g_sresult_pos;

/* Round 732 (task #447, fresh GT3-in-game-code context): live dispatch
 * counters/last-issued-command latches, purely diagnostic (mirrors the
 * existing iop_cdvd_get_last_ncommand()'s already-established
 * "project-internal accessor, not real hardware state" convention -
 * see that function's own header comment). Needed to answer a
 * concrete empirical question a checkpoint-resumed GT3 survey can't
 * answer any other way: does GT3's own real IOP-side code ever issue
 * a FURTHER N-command/S-command after its first on-screen frame, or
 * has it stopped calling into this file entirely (in which case the
 * stall is upstream of CDVD dispatch, not inside it). */
static uint64_t g_ncmd_call_count;
static uint64_t g_scmd_call_count;
static uint8_t  g_last_ncmd_issued;
static uint8_t  g_last_scmd_issued;

/* Round 206: this interface's own disc-image binding (separate from
 * iop_cdrom_legacy.c's - see iop_cdvd.h's citation for why). */
static iso_image_t g_disc;
static int         g_disc_mounted;

void iop_cdvd_init(void)
{
    memset(g_regs, 0, sizeof(g_regs));
    g_param_count = 0;
    /* Round 261 (task #422): idle S-command state - no result bytes
     * queued (pos>=count) so a read of SDATAIN reports
     * IOP_CDVD_SDATAIN_NODATA, matching real hardware's idle state and
     * letting the real BIOS's initial "drain stale data" loop (see
     * iop_cdvd.h's citation) exit immediately instead of spinning
     * forever - this was the exact hang point Round 260 measured this
     * project's boot trace resting at (IOP pc 0x0010BB7C). */
    g_sparam_count = 0;
    g_sresult_count = 0;
    g_sresult_pos = 0;
    /* Round 732: reset alongside every other dispatch-side state above -
     * a soft register reset legitimately restarts these too. */
    g_ncmd_call_count = 0;
    g_scmd_call_count = 0;
    g_last_ncmd_issued = 0;
    g_last_scmd_issued = 0;
    /* Round 206: deliberately does NOT touch g_disc_mounted/g_disc -
     * mirrors iop_cdrom_legacy_init()'s own precedent of resetting
     * register state on init while leaving disc-mount state to the
     * caller's own explicit mount/unmount calls, since a real disc
     * physically stays in the drive across a soft register reset. */

    /* Real cdvdReset() defaults (pcsx2/CDVD/CDVD.cpp) for the
     * diskless-boot case this scaffold targets - see the header
     * comment for the full citation. Round 259 (task #419): also
     * ORs in IOP_CDVD_NREADY_CONFIG_READY (bit 3) - see that
     * constant's own header comment for the full disassembly +
     * independent-real-source citation trail. Set unconditionally at
     * reset (not gated on disc-mount state) since the real EECONF
     * module's own use of this bit - reading/writing the drive
     * controller's onboard config block - is plausibly independent
     * of whether a disc is physically present. */
    g_regs[OFF_NREADY]     = IOP_CDVD_DRIVE_READY | IOP_CDVD_NREADY_CONFIG_READY | IOP_CDVD_NREADY_CONFIG2_READY;
    g_regs[OFF_STATUS]     = IOP_CDVD_STATUS_TRAY_OPEN;
    g_regs[OFF_STATUS_STK] = IOP_CDVD_STATUS_TRAY_OPEN;
    g_regs[OFF_TYPE]       = IOP_CDVD_TYPE_NODISC;
    g_regs[OFF_ERROR]      = 0u;
    g_regs[OFF_INTR_STAT]  = 0u;
    g_regs[OFF_SDATAIN]    = IOP_CDVD_SDATAIN_NODATA;
}

int iop_cdvd_mount_iso(const char *path)
{
    if (g_disc_mounted) iso_close(&g_disc);
    if (iso_open(path, &g_disc) != 0) { g_disc_mounted = 0; return -1; }
    g_disc_mounted = 1;
    return 0;
}

void iop_cdvd_unmount_iso(void)
{
    if (g_disc_mounted) { iso_close(&g_disc); g_disc_mounted = 0; }
}

/* Round 449 (task #247 final root cause, part 2): re-open the disc
 * image fresh in THIS process, WITHOUT touching the existing g_disc
 * first - see docs/STATUS.md Round 449 for the full citation. After
 * a checkpoint's raw [__data_start,_end) block restore, g_disc.fp is
 * a stale FILE* (a heap-allocated glibc FILE struct address) from
 * whichever process wrote the checkpoint - calling iso_close() on it
 * (as iop_cdvd_mount_iso() does before reopening) would itself
 * dereference/fclose() that invalid pointer and crash. iso_open()
 * unconditionally memset()s the whole out-struct before filling it
 * in, so it is always safe to call directly regardless of whatever
 * stale bytes g_disc held going in - this bypasses iop_cdvd_mount_iso()'s
 * unsafe-post-restore iso_close() call entirely. Exactly the same
 * "re-point to THIS process's own freshly-opened/allocated resource"
 * pattern already used for ee->ram/iop->ram/bios.data/g_alloclist. */
int iop_cdvd_rebind_iso(const char *path)
{
    if (iso_open(path, &g_disc) != 0) { g_disc_mounted = 0; return -1; }
    g_disc_mounted = 1;
    return 0;
}

/* Round 367 (real, evidenced gap found via a fresh look at real
 * PS2 conventions: SYSTEM.CNF and game data are read through the
 * generic SIF_SID_FILEIO service via cdrom0:/cdrom1: paths - e.g.
 * real ee/kernel/src/fileio.c's fioOpen("cdrom0:\SYSTEM.CNF;1", ...)
 * - the SAME RPC service this project already wired for rom0: files
 * in Round 346, but ee_core.c's FIO_F_OPEN handler has only ever
 * checked for a "rom0:" prefix, never "cdrom0:"/"cdrom1:". This
 * project already has a real, tested, standalone ISO9660 parser
 * (iso_loader.c, Round 139/170) and already holds a fully-opened,
 * fully-parsed g_disc for the mounted image right here in this file
 * - but nothing outside this file could ever query it. These two
 * small accessors expose exactly what ee_core.c's FIO_F_OPEN/READ
 * handlers need, mirroring the existing romdir_lookup()-style
 * pattern used for rom0: files, without duplicating any ISO9660
 * parsing logic - iso_loader.c's own iso_find_in_root()/
 * iso_read_sector() do the real work; this is a thin, honest
 * pass-through that only succeeds when a real disc is genuinely
 * mounted (g_disc_mounted) and the real ISO9660 root directory
 * genuinely contains the requested name. */
int iop_cdvd_disc_find_file(const char *name, uint32_t *out_lba, uint32_t *out_size)
{
    iso_dirent_t dirent;
    if (!g_disc_mounted) return 0;
    if (iso_find_in_root(&g_disc, name, &dirent) != 0) return 0; /* iso_find_in_root() returns 0 on success, -1 on failure - matches iso_open()'s own documented convention */
    if (dirent.is_directory) return 0; /* FIO_F_OPEN is for files - matches real fioOpen()'s own directory rejection */
    *out_lba = dirent.lba;
    *out_size = dirent.size;
    return 1;
}

int iop_cdvd_disc_read_sector(uint32_t lba, uint8_t *buf)
{
    if (!g_disc_mounted) return -1;
    return iso_read_sector(&g_disc, lba, buf);
}

/* Round 206: real N-command dispatch, run when OFF_NCMD is written.
 * See iop_cdvd.h's citation for exact real semantics/scope. */
static void dispatch_ncmd(uint8_t cmd)
{
    uint8_t irq_bits = ISTAT_COMMAND_COMPLETE; /* every real N-command raises this per ps2tek's "All N commands raise IRQ2" preamble */

    g_ncmd_call_count++; /* Round 732 - see field comment */
    g_last_ncmd_issued = cmd;

    if (cmd == NCMD_READCD || cmd == NCMD_READDVD) {
        if (g_disc_mounted && g_param_count >= 8) {
            uint32_t sector = (uint32_t)g_param_buf[0]
                             | ((uint32_t)g_param_buf[1] << 8)
                             | ((uint32_t)g_param_buf[2] << 16)
                             | ((uint32_t)g_param_buf[3] << 24);
            uint32_t count  = (uint32_t)g_param_buf[4]
                             | ((uint32_t)g_param_buf[5] << 8)
                             | ((uint32_t)g_param_buf[6] << 16)
                             | ((uint32_t)g_param_buf[7] << 24);
            /* Honest safety clamp (not a cited hardware limit) - a
             * corrupt/bogus param block must not turn into an
             * unbounded loop; real hardware would presumably error
             * on an out-of-range disc address instead, but this
             * project has no citable source for the exact real error
             * path, so this only bounds the LOOP, it doesn't fake a
             * specific real error code. */
            if (count > 4096u) count = 4096u;

            uint8_t sector_buf[CDVD_SECTOR_SIZE];
            uint32_t i;
            uint32_t read_ok_count = 0;
            for (i = 0; i < count; i++) {
                if (iso_read_sector(&g_disc, sector + i, sector_buf) != 0)
                    break; /* real end-of-disc/bad-sector case - stop delivering rather than fabricate data */
                if (!iop_dma_channel_write_bytes(3, sector_buf, CDVD_SECTOR_SIZE))
                    break; /* IOP RAM not bound yet, or destination MADR out of bounds */
                read_ok_count++;
            }
            if (read_ok_count > 0) {
                /* ps2tek's "CDVD Reads and Seeks" page: "Successful
                 * reads seem to raise both bits 1 AND 0 of CDVD
                 * I_STAT" - directly cited, not guessed. */
                irq_bits |= ISTAT_DATA_READY;
            } else {
                g_regs[OFF_ERROR] = 0x01u; /* honest: real error code unknown/uncited, only a non-zero "an error occurred" per this file's own already-existing OFF_ERROR read-clears-on-read convention */
            }
        } else {
            /* No disc mounted, or param block too short to contain a
             * real sector position/count - matches this project's
             * own already-established "honest no-disc" behavior
             * elsewhere (iop_cdrom_legacy.c's respond_no_disc()). */
            g_regs[OFF_ERROR] = 0x01u;
        }
    }
    /* NOP/NOPsync/Standby/Stop/Pause/Seek/GetToc (00h/01h/02h/03h/
     * 04h/05h/09h): acknowledged (irq_bits already set above), no
     * further modeled side effect - honest scope limit, see header. */

    g_regs[OFF_INTR_STAT] |= irq_bits;
    iop_intc_raise(2); /* real, shared CD-ROM/CDVD controller IRQ line - same physical IRQ2 iop_cdrom_legacy.c already raises for the legacy interface */

    g_param_count = 0; /* real hardware: params consumed once the command using them has been issued */
}

/* Round 261 (task #422): real S-command dispatch, run when
 * OFF_SCOMMAND is written. See iop_cdvd.h's citation for the exact
 * real register protocol (busy-wait/drain-loop) this unblocks and the
 * per-command result-size citations. Mirrors dispatch_ncmd()'s own
 * "immediate synthetic completion, no fabricated command-specific
 * data" philosophy exactly. */
static void dispatch_scmd(uint8_t cmd)
{
    g_scmd_call_count++; /* Round 732 - see field comment */
    g_last_scmd_issued = cmd;

    g_sresult_count = 0;
    g_sresult_pos = 0;

    switch (cmd) {
    case SCMD_OPENCONFIG:
        /* Real, cited: ps2tek "Dobiestation returns zero" - one
         * result byte, value 0. */
        g_sresult_buf[0] = 0x00u;
        g_sresult_count = 1;
        break;
    case SCMD_READCONFIG:
        /* Real, cited: ps2tek "Output is four 32bit words" (16
         * bytes). Honestly zero-filled - no real config block is
         * modeled, this only supplies the real byte COUNT so the
         * real result-read loop drains exactly as many bytes as real
         * hardware would present, then stops. */
        memset(g_sresult_buf, 0, 16);
        g_sresult_count = 16;
        break;
    case SCMD_CLOSECONFIG:
        /* ps2tek documents this command but not its result size;
         * same single-zero-byte minimal ack as OpenConfig for
         * consistency (honest default, not a specific cited value). */
        g_sresult_buf[0] = 0x00u;
        g_sresult_count = 1;
        break;
    default:
        /* SCMD_WRITECONFIG and any other/unimplemented S-command:
         * acknowledged, no further modeled side effect - same honest
         * scope limit dispatch_ncmd() already uses for its own
         * unimplemented commands. */
        g_sresult_buf[0] = 0x00u;
        g_sresult_count = 1;
        break;
    }

    g_sparam_count = 0; /* real hardware: params consumed once the command using them has been issued */
}

int iop_cdvd_mmio_read8(uint32_t addr, uint8_t *out)
{
    /* Round 130 (task #172/#196, 170th finding): the IOP has no MMU/
     * TLB, so KUSEG (0x1F402xxx), KSEG0 (0x9F402xxx) and KSEG1
     * (0xBF402xxx) all address the SAME physical CDVD register page -
     * real code frequently polls hardware status via the KSEG1
     * (uncached) alias specifically to avoid a stale cached read
     * during a busy-wait, exactly the pattern task #165 already found
     * and fixed for the SIF mailbox window (see sif.c's matching
     * comment). This check previously only accepted the bare KUSEG
     * form, so a real KSEG1-alias poll (e.g. 0xBF40200A) silently
     * fell through to unmapped/out-of-range RAM (always reading 0)
     * instead of the real register - found via live host-native
     * tracing showing the IOP polling this exact KSEG1 address in a
     * tight loop after Round 129's exception-vector fix unblocked
     * further boot progress. Masking off the segment-select bits
     * before the window check, same as sif.c's iop_mem_ptr()-style
     * convention, fixes all three aliases at once. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_CDVD_BASE || phys >= IOP_CDVD_BASE + 0x1000u) return 0;
    /* Real hardware mirrors these byte registers across the whole
     * 4KB page - PCSX2's psxHw4Read8/Write8 masks the address to its
     * low 8 bits before dispatching, so replicate that here rather
     * than only accepting the first IOP_CDVD_SIZE bytes of the page. */
    uint32_t off = (phys - IOP_CDVD_BASE) & 0xFFu;

    if (off == OFF_ERROR) {
        /* Real behavior: reading ERROR returns its current value and
         * clears it (pcsx2/CDVD/CDVD.cpp's cdvdRead06). */
        *out = g_regs[off];
        g_regs[off] = 0u;
        return 1;
    }
    if (off == OFF_BREAK) {
        /* Real hardware: BREAK always reads back 0 (cdvdRead07). */
        *out = 0u;
        return 1;
    }
    if (off == OFF_SDATAIN) {
        /* Round 261 (task #422): real status byte - this project
         * resolves every S-command synchronously (same convention as
         * dispatch_ncmd()), so IOP_CDVD_SDATAIN_BUSY (bit 7) never
         * needs to be set. IOP_CDVD_SDATAIN_NODATA (bit 6) reflects
         * whether g_sresult_buf still has unread bytes - see
         * iop_cdvd.h's citation for why real code depends on this bit
         * clearing/setting correctly (both the pre-command drain loop
         * and the post-command result-read loop key off it). */
        uint8_t v = 0u;
        if (g_sresult_pos >= g_sresult_count) v |= IOP_CDVD_SDATAIN_NODATA;
        *out = v;
        return 1;
    }
    if (off == OFF_SDATAOUT) {
        /* Round 261 (task #422): pop the next queued S-command result
         * byte, real "read one at a time" semantics per ps2tek's SCMD
         * page preamble. Reading past the end (shouldn't happen if
         * calling code honors SDATAIN's NODATA bit like real BIOS
         * code does) returns 0 rather than reading out of bounds. */
        if (g_sresult_pos < g_sresult_count) {
            *out = g_sresult_buf[g_sresult_pos++];
        } else {
            *out = 0u;
        }
        return 1;
    }

    *out = g_regs[off];
    return 1;
}

int iop_cdvd_mmio_write8(uint32_t addr, uint8_t value)
{
    /* Round 130 (170th finding): same KUSEG/KSEG0/KSEG1 aliasing fix
     * as iop_cdvd_mmio_read8() above - see that function's comment
     * for the full citation trail. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_CDVD_BASE || phys >= IOP_CDVD_BASE + 0x1000u) return 0;
    uint32_t off = (phys - IOP_CDVD_BASE) & 0xFFu;

    /* Round 206: OFF_NREADY (0x05) and OFF_INTR_STAT (0x08) are real,
     * cited bank-switched registers where the WRITE side has a
     * completely different meaning from the READ side at the same
     * address (ps2tek: 0x05 write = "N command param", 0x05 read =
     * "N command status"; 0x08 write = "Acknowledge (1=Clear bit)",
     * 0x08 read = "Status (1=Reason for IRQ)") - handled specially
     * here rather than the old unconditional "g_regs[off]=value"
     * store, which would have silently corrupted the read-side status
     * bits' meaning with a raw parameter byte. */
    if (off == OFF_NREADY) {
        if (g_param_count < PARAM_BUF_MAX)
            g_param_buf[g_param_count++] = value;
        return 1;
    }
    if (off == OFF_INTR_STAT) {
        g_regs[off] &= (uint8_t)~value; /* real "write 1 to clear" acknowledge semantics */
        return 1;
    }
    if (off == OFF_SDATAIN) {
        /* Round 261 (task #422): write side of the same address is
         * "S command params" (real, per ps2tek's SCMD page and this
         * project's own fresh disassembly) - same buffering
         * convention as OFF_NREADY's write side above. */
        if (g_sparam_count < PARAM_BUF_MAX)
            g_sparam_buf[g_sparam_count++] = value;
        return 1;
    }
    if (off == OFF_SCOMMAND) {
        g_regs[off] = value;
        dispatch_scmd(value);
        return 1;
    }

    g_regs[off] = value;

    if (off == OFF_NCMD) {
        dispatch_ncmd(value);
    }

    return 1;
}

uint8_t iop_cdvd_get_last_ncommand(void) { return g_regs[OFF_NCMD]; }

/* Round 732 (task #447): diagnostic-only getters for the counters/
 * latches above - same "project-internal accessor" convention as
 * iop_cdvd_get_last_ncommand() immediately above. */
uint64_t iop_cdvd_get_ncmd_call_count(void) { return g_ncmd_call_count; }
uint64_t iop_cdvd_get_scmd_call_count(void) { return g_scmd_call_count; }
uint8_t  iop_cdvd_get_last_ncmd_issued(void) { return g_last_ncmd_issued; }
uint8_t  iop_cdvd_get_last_scmd_issued(void) { return g_last_scmd_issued; }

/* Round 347 (IOP RPC re-entry architecture): non-consuming peek at
 * the real OFF_ERROR register - unlike iop_cdvd_mmio_read8()'s own
 * real "read clears" semantics (pcsx2/CDVD/CDVD.cpp's cdvdRead06,
 * already cited above), this is a project-internal accessor only,
 * never reachable from real IOP-side MMIO reads, so it cannot alter
 * any real, already-tested register behavior. Lets ee_core.c's real-
 * dispatch completion path (see ee_check_cdvd_ncmd_pending()) decide
 * a real success/failure reply value from dispatch_ncmd()'s own
 * already-real result, without racing or disturbing whatever real
 * IOP-side code (if any) later reads+clears OFF_ERROR through the
 * normal MMIO path for its own purposes. */
uint8_t iop_cdvd_peek_last_ncmd_error(void) { return g_regs[OFF_ERROR]; }
uint8_t iop_cdvd_get_status(void)        { return g_regs[OFF_STATUS]; }
uint8_t iop_cdvd_get_ready(void)         { return g_regs[OFF_NREADY]; }
uint8_t iop_cdvd_get_disc_type(void)     { return g_regs[OFF_TYPE]; }

void iop_cdvd_set_disc_present(uint8_t disc_type)
{
    /* Real, cited values - see iop_cdvd.h's own citation for
     * IOP_CDVD_STATUS_PAUSE/IOP_CDVD_TYPE_PS2CD (PCSX2's real
     * cdvdCtrlTrayClose() fast-boot path). */
    g_regs[OFF_TYPE]       = disc_type;
    g_regs[OFF_STATUS]     = IOP_CDVD_STATUS_PAUSE;
    g_regs[OFF_STATUS_STK] |= IOP_CDVD_STATUS_PAUSE; /* real StatusSticky |= behavior, cdvdUpdateStatus() */
    /* Round 259 (task #419): same IOP_CDVD_NREADY_CONFIG_READY (bit
     * 3) fix as iop_cdvd_init() above - kept consistent here so a
     * disc-mount call after init doesn't regress this bit back off.
     * See iop_cdvd.h's citation for the full trail. */
    g_regs[OFF_NREADY]     = IOP_CDVD_DRIVE_READY | IOP_CDVD_NREADY_CONFIG_READY | IOP_CDVD_NREADY_CONFIG2_READY;
}
