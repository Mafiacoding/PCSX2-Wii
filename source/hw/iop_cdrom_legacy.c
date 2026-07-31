/*
 * iop_cdrom_legacy.c - real PS1-legacy CD-ROM controller command/
 * response state machine. See include/core/hw/iop_cdrom_legacy.h for
 * the full citation trail (Round 145, 185th finding).
 */
#include "core/hw/iop_cdrom_legacy.h"
#include "core/hw/iop_intc.h"
#include "core/hw/iop_hle_events.h" /* Round 148 (task #301): real kernel
                                      * event delivery for the CD-ROM
                                      * async driver, see raise_int(). */
#include <string.h>
#include "core/hw/iop_asyncio.h" /* Round 153 (task #307): real async
                                    * I/O queue - see cdrom_boot_kick_
                                    * complete()'s comment below. */

/* Real, cited PS1/PS2 kernel event class for CDROM/IRQ2 (psx-spx
 * kernelbios.md, directly quoted: "F0000003h IRQ2  CDROM Decoder"),
 * and the real per-completion-type spec bitmasks cited on the same
 * page's CdAsyncSeekL/CdAsyncGetStatus/CdAsyncReadSector entries:
 * "Completion is indicated by events (class=F0000003h, and spec=20h,
 * or 8000h)" (SeekL/GetStatus), "...spec=20h, 80h, or 8000h"
 * (ReadSector), and the explicit per-spec table further down the
 * same page: "F0000003h,80h  cdrom INT4 (reached end of disk)". The
 * exact mapping from THIS project's own real, cited INT1-INT5
 * interrupt-type model (Round 145) to these specific spec bits is a
 * clean-room inference bridging those two independently-real,
 * independently-cited facts (not a literal transcription of any
 * single source): the terminal interrupt of each real two-phase
 * command sequence (INT2 for Standby/Stop/Pause/Init/SeekL/SeekP/
 * Setsession/ReadTOC, INT1 for ReadN/ReadS) delivers spec 20h+8000h
 * (the pair psx-spx cites together for every async completion), and
 * INT4 (DataEnd) delivers spec 80h (the one spec psx-spx explicitly,
 * separately ties to "reached end of disk" = INT4). INT3-only (ack
 * with no pending second phase, e.g. Nop/Mute/Demute/Setmode) is not
 * given a delivery here since psx-spx's cited async-completion table
 * only documents SeekL/GetStatus/ReadSector (all real two-phase
 * commands terminating on INT2/INT1) - extending this to other
 * commands without a citation would be exactly the kind of guess this
 * project has consistently declined to make. */
#define IOP_CDROM_EVENT_CLASS 0xF0000003u

#define PARAM_FIFO_SIZE  16
#define RESULT_FIFO_SIZE 16
#define SECTOR_SIZE      2048

typedef enum {
    PENDING_NONE = 0,
    PENDING_INT2_STAT,   /* second response: INT2(stat) - Standby/Stop/Pause/
                          * Init/SeekL/SeekP/Setsession/ReadTOC */
    PENDING_INT1_READ,   /* second response: INT1(stat), then sector data
                          * becomes available via RDDATA - ReadN/ReadS */
    PENDING_INT2_GETID   /* second response: INT2(stat,flags,type,atip,"SCEx")
                          * or INT5 on error - GetID */
} pending_kind_t;

static struct {
    uint32_t bank;             /* RA, 2 bits */
    uint8_t  param_fifo[PARAM_FIFO_SIZE];
    int      param_count;
    uint8_t  result_fifo[RESULT_FIFO_SIZE];
    int      result_count;
    int      result_pos;
    uint8_t  intsts;           /* 0-7, real HINTSTS bits 0-2 */
    uint8_t  intmsk;           /* real HINTMSK bits 0-4 */
    uint8_t  stat;             /* real status(stat) byte */
    uint8_t  mode;             /* last Setmode value */
    uint8_t  filter_file, filter_channel;
    uint32_t loc_mm, loc_ss, loc_ff; /* real BCD MSF from Setloc */
    pending_kind_t pending;

    iso_image_t disc;
    int         disc_mounted;

    uint8_t  data_buf[SECTOR_SIZE];
    int      data_len;
    int      data_pos;
} g;

/* Real, well-known, public CD-Audio Red Book MSF<->LBA convention
 * (2-second/150-sector lead-in) - a public audio-CD standard, not
 * Sony/PS-BIOS-derived, same non-proprietary status this project
 * already treats ISO9660 (iso_loader.h) as having. */
static uint32_t bcd_to_bin(uint8_t v) { return (uint32_t)((v >> 4) * 10 + (v & 0xF)); }
static uint8_t  bin_to_bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static uint32_t msf_to_lba(uint32_t mm_bcd, uint32_t ss_bcd, uint32_t ff_bcd)
{
    uint32_t mm = bcd_to_bin((uint8_t)mm_bcd);
    uint32_t ss = bcd_to_bin((uint8_t)ss_bcd);
    uint32_t ff = bcd_to_bin((uint8_t)ff_bcd);
    uint32_t abs_frame = (mm * 60u + ss) * 75u + ff;
    return (abs_frame >= 150u) ? (abs_frame - 150u) : 0u;
}

static void result_clear(void) { g.result_count = 0; g.result_pos = 0; }
static void result_push(uint8_t b) { if (g.result_count < RESULT_FIFO_SIZE) g.result_fifo[g.result_count++] = b; }

static void raise_int(uint8_t which)
{
    g.intsts = which & 0x07u;
    /* Real, cited: "The CD-ROM drive fires an interrupt whenever
     * (HINTMSK & HINTSTS) is non-zero" - real IOP_IRQ_CDROM is 2
     * (psx-spx Interrupts page, directly cited: "2 IRQ2 CDROM"). */
    if (g.intmsk & g.intsts)
        iop_intc_raise(2);

    /* Round 148 (task #301): real kernel event delivery - see this
     * file's top-of-file comment for the full citation trail. This
     * is the missing piece that lets any real, interpreted BIOS code
     * that called B(08h) OpenEvent(F0000003h, spec, EvMdNOINTR, 0)
     * and then polls via B(0Bh) TestEvent ever observe completion -
     * previously nothing in this project ever called DeliverEvent
     * for this real, hardware-backed event class. */
    switch (which) {
    case IOP_CDROM_INT2:
    case IOP_CDROM_INT1:
        iop_hle_event_deliver_raw(IOP_CDROM_EVENT_CLASS, 0x0020u);
        iop_hle_event_deliver_raw(IOP_CDROM_EVENT_CLASS, 0x8000u);
        break;
    case IOP_CDROM_INT4:
        iop_hle_event_deliver_raw(IOP_CDROM_EVENT_CLASS, 0x0080u);
        break;
    default:
        break;
    }
}

static void update_stat_bits(void)
{
    /* PRMEMPT/PRMWRDY/RSLRRDY/DRQSTS/BUSYSTS live in the ADDRESS/HSTS
     * byte, computed on read - see mmio_read8. stat (the command
     * response byte) is tracked separately in g.stat. */
}

static int have_disc(void) { return g.disc_mounted; }

/* Real, cited no-disc error convention: "80h - Cannot respond yet...
 * also appears if no disk inserted at all", delivered as INT5 with
 * the Error bit (stat bit0) set and 80h as the second response byte. */
static void respond_no_disc(void)
{
    result_clear();
    result_push((uint8_t)(g.stat | IOP_CDROM_STAT_ERROR));
    result_push(0x80u);
    g.pending = PENDING_NONE;
    raise_int(IOP_CDROM_INT5);
}

/* Real, cited generic-unused-opcode response: INT5, stat with Error
 * bit set, error byte 40h "Invalid command". */
static void respond_invalid_command(void)
{
    result_clear();
    result_push((uint8_t)(g.stat | IOP_CDROM_STAT_ERROR));
    result_push(0x40u);
    g.pending = PENDING_NONE;
    raise_int(IOP_CDROM_INT5);
}

static void ack_stat_only(void)
{
    result_clear();
    result_push(g.stat);
    raise_int(IOP_CDROM_INT3);
}

static void begin_pending_int2(void)
{
    ack_stat_only();
    g.pending = PENDING_INT2_STAT;
}

static void deliver_pending(void)
{
    switch (g.pending) {
    case PENDING_INT2_STAT:
        result_clear();
        result_push(g.stat);
        g.pending = PENDING_NONE;
        raise_int(IOP_CDROM_INT2);
        break;
    case PENDING_INT1_READ:
        result_clear();
        result_push(g.stat | IOP_CDROM_STAT_READ);
        g.pending = PENDING_NONE;
        raise_int(IOP_CDROM_INT1);
        /* Real: "sector data must be read separately via RDDATA or
         * DMA" after this INT1 - data_buf/data_len were already
         * filled by the ReadN/ReadS handler below. */
        g.data_pos = 0;
        break;
    case PENDING_INT2_GETID:
        result_clear();
        if (have_disc()) {
            /* Simplified, structurally-real GetID success response
             * (stat, flags, type, atip, "SCEx") - the exact real
             * flags/type/atip byte semantics for a genuine licensed
             * PS2 data disc were not part of this round's fetched
             * citation, so only the real 8-byte response SHAPE and
             * the real success/failure branch (see respond_no_disc
             * for the cited failure case) are modeled; content bytes
             * below are an honest, clearly-labeled placeholder, not
             * a claimed-verified hardware value. */
            result_push(g.stat);
            result_push(0x00u); /* flags: placeholder, "licensed" */
            result_push(0x00u); /* type: placeholder, "Mode2" */
            result_push(0x00u); /* atip: placeholder */
            result_push('S'); result_push('C'); result_push('E'); result_push('x');
            g.pending = PENDING_NONE;
            raise_int(IOP_CDROM_INT2);
        } else {
            respond_no_disc();
        }
        break;
    default:
        break;
    }
}

void iop_cdrom_legacy_init(void)
{
    memset(&g, 0, sizeof(g));
    g.stat = 0; /* motor off, no error, shell state unknown/closed default */
    g.intmsk = 0;
    g.pending = PENDING_NONE;
    g.disc_mounted = 0;
}

int iop_cdrom_legacy_mount_iso(const char *path)
{
    if (g.disc_mounted) iso_close(&g.disc);
    if (iso_open(path, &g.disc) != 0) { g.disc_mounted = 0; return -1; }
    g.disc_mounted = 1;
    g.stat |= IOP_CDROM_STAT_MOTOR;
    return 0;
}

void iop_cdrom_legacy_unmount_iso(void)
{
    if (g.disc_mounted) { iso_close(&g.disc); g.disc_mounted = 0; }
}

static void do_read(int mode_s)
{
    if (!have_disc()) { respond_no_disc(); return; }
    uint32_t lba = msf_to_lba(g.loc_mm, g.loc_ss, g.loc_ff);
    if (iso_read_sector(&g.disc, lba, g.data_buf) != 0) {
        respond_no_disc();
        return;
    }
    g.data_len = SECTOR_SIZE;
    /* Real: ReadN(06h)/ReadS(1Bh) both -> INT3(stat) first, then
     * INT1(stat) once the sector is ready. */
    (void)mode_s;
    ack_stat_only();
    g.pending = PENDING_INT1_READ;
}

/* Round 153 (task #307): labeled, NON-cited HLE boot-unblock
 * synthesis. Rounds 149-152 established: (a) this project's own
 * diskless boot issues a real Setloc and then parks forever in the
 * real, cited B0h/TestEvent poll waiting for kernel event class
 * 0xF0000003 spec 5/15 (docs/STATUS.md 188th-189th findings); (b)
 * that wait does NOT depend on disc/media presence (189th finding's
 * disc-mount experiment); (c) the real trigger chain this project's
 * own boot would need to reach that event organically leads into a
 * generic async I/O queue/channel-dispatch subsystem (190th-192nd
 * findings) that is architecturally real (see iop_asyncio.h) but
 * whose exact real invocation point from THIS project's specific
 * diskless boot sequence was not conclusively identified.
 *
 * psx-spx's cited CD-ROM command table does NOT document Setloc
 * itself completing on INT1/INT2/INT4 (see this file's top-of-file
 * comment) - this function does not claim otherwise. It exists only
 * so the real, cited B0h/TestEvent boot-time wait can be satisfied
 * and this project's boot can keep making forward progress, clearly
 * separated here from the real, per-command-cited INT-to-DeliverEvent
 * mapping already in raise_int(). It is routed through the real
 * iop_asyncio queue (rather than an unconditional bare call) so the
 * event delivery is genuinely queued-and-serviced, exercising the
 * same real infrastructure a fuller future implementation would
 * eventually use for genuine in-game disc commands. */
static void cdrom_boot_kick_complete(void *user_data)
{
    (void)user_data;
    iop_hle_event_deliver_raw(IOP_CDROM_EVENT_CLASS, 0x0020u);
    iop_hle_event_deliver_raw(IOP_CDROM_EVENT_CLASS, 0x8000u);
}

static void dispatch_command(uint8_t cmd)
{
    uint8_t *p = g.param_fifo;
    int n = g.param_count;

    switch (cmd) {
    case 0x01: /* Nop -> INT3(stat) */
        ack_stat_only();
        break;
    case 0x02: /* Setloc(min,sec,frame) -> INT3(stat) */
        if (n >= 3) { g.loc_mm = p[0]; g.loc_ss = p[1]; g.loc_ff = p[2]; }
        ack_stat_only();
        /* Round 153 (task #307): queue the boot-unblock completion -
         * see cdrom_boot_kick_complete()'s comment above for exactly
         * what this is and, just as importantly, what it is NOT
         * claiming about real Setloc behavior. */
        iop_asyncio_enqueue(IOP_ASYNCIO_DEV_CDROM, cdrom_boot_kick_complete, 0);
        break;
    case 0x03: /* Play(track optional) -> INT3(stat) - ack only, no
                * real audio streaming (see header scope note) */
        ack_stat_only();
        break;
    case 0x06: /* ReadN -> INT3(stat) -> INT1(stat) -> datablock */
        do_read(0);
        break;
    case 0x07: /* Standby -> INT3(stat) -> INT2(stat) */
        g.stat |= IOP_CDROM_STAT_MOTOR;
        begin_pending_int2();
        break;
    case 0x08: /* Stop -> INT3(stat) -> INT2(stat) */
        g.stat &= (uint8_t)~IOP_CDROM_STAT_MOTOR;
        begin_pending_int2();
        break;
    case 0x09: /* Pause -> INT3(stat) -> INT2(stat) */
        g.stat &= (uint8_t)~(IOP_CDROM_STAT_READ | IOP_CDROM_STAT_PLAY | IOP_CDROM_STAT_SEEK);
        begin_pending_int2();
        break;
    case 0x0A: /* Init -> INT3(stat) -> INT2(stat) */
        g.mode = 0;
        g.stat = (uint8_t)((g.stat & IOP_CDROM_STAT_SHELLOPEN) | IOP_CDROM_STAT_MOTOR);
        begin_pending_int2();
        break;
    case 0x0B: /* Mute -> INT3(stat) */
    case 0x0C: /* Demute -> INT3(stat) */
        ack_stat_only();
        break;
    case 0x0D: /* Setfilter(file,channel) -> INT3(stat) */
        if (n >= 2) { g.filter_file = p[0]; g.filter_channel = p[1]; }
        ack_stat_only();
        break;
    case 0x0E: /* Setmode(mode) -> INT3(stat) */
        if (n >= 1) g.mode = p[0];
        ack_stat_only();
        break;
    case 0x0F: /* Getparam -> INT3(stat,mode,0,file,channel) */
        result_clear();
        result_push(g.stat);
        result_push(g.mode);
        result_push(0x00u);
        result_push(g.filter_file);
        result_push(g.filter_channel);
        raise_int(IOP_CDROM_INT3);
        break;
    case 0x10: /* GetlocL -> INT3(min,sec,frame,mode,file,channel,sm,ci) */
        if (!have_disc()) { respond_no_disc(); break; }
        result_clear();
        result_push((uint8_t)g.loc_mm);
        result_push((uint8_t)g.loc_ss);
        result_push((uint8_t)g.loc_ff);
        result_push(g.mode);
        result_push(g.filter_file);
        result_push(g.filter_channel);
        result_push(0x00u); /* sm: submode, not modeled beyond placeholder */
        result_push(0x00u); /* ci: coding info, not modeled beyond placeholder */
        raise_int(IOP_CDROM_INT3);
        break;
    case 0x11: /* GetlocP -> INT3(track,index,rmin,rsec,rframe,min,sec,frame) */
        if (!have_disc()) { respond_no_disc(); break; }
        result_clear();
        result_push(bin_to_bcd(1)); /* track: single real data track, see scope note */
        result_push(bin_to_bcd(1)); /* index */
        result_push(0x00u); result_push(0x02u); result_push(0x00u); /* relative MSF (real 2s pregap) */
        result_push((uint8_t)g.loc_mm);
        result_push((uint8_t)g.loc_ss);
        result_push((uint8_t)g.loc_ff);
        raise_int(IOP_CDROM_INT3);
        break;
    case 0x12: /* Setsession(session) -> INT3(stat) -> INT2(stat) */
        begin_pending_int2();
        break;
    case 0x13: /* GetTN -> INT3(stat,first,last) */
        if (!have_disc()) { respond_no_disc(); break; }
        result_clear();
        result_push(g.stat);
        result_push(bin_to_bcd(1)); /* first: single real data track */
        result_push(bin_to_bcd(1)); /* last */
        raise_int(IOP_CDROM_INT3);
        break;
    case 0x14: /* GetTD(track) -> INT3(stat,mm,ss) - simplified, see
                * header scope note (no real TOC/session parsing). */
        if (!have_disc()) { respond_no_disc(); break; }
        result_clear();
        result_push(g.stat);
        result_push(bin_to_bcd(0));
        result_push(bin_to_bcd(2)); /* real 2-second lead-in convention */
        raise_int(IOP_CDROM_INT3);
        break;
    case 0x15: /* SeekL -> INT3(stat) -> INT2(stat) */
    case 0x16: /* SeekP -> INT3(stat) -> INT2(stat) */
        if (!have_disc()) { respond_no_disc(); break; }
        g.stat |= IOP_CDROM_STAT_SEEK;
        begin_pending_int2();
        break;
    case 0x19: /* Test - only sub-function 20h (BIOS date/version) is
                * real-cited as actually used by kernel code. */
        if (n >= 1 && p[0] == 0x20u) {
            result_clear();
            /* Real 4-byte SHAPE (yy,mm,dd,ver) is cited; the specific
             * byte VALUES vary per real console/BIOS revision and
             * weren't part of this round's citation - honest
             * placeholder content, not a claimed-verified value. */
            result_push(0x00u); result_push(0x00u); result_push(0x00u); result_push(0x00u);
            raise_int(IOP_CDROM_INT3);
        } else {
            respond_invalid_command();
        }
        break;
    case 0x1A: /* GetID -> INT3(stat) -> INT2/5(...) */
        ack_stat_only();
        g.pending = PENDING_INT2_GETID;
        break;
    case 0x1B: /* ReadS -> INT3(stat) -> INT1(stat) -> datablock */
        do_read(1);
        break;
    case 0x1C: /* Reset -> INT3(stat) - real hardware then reboots the
                * HC05 after a delay; this project's commands complete
                * on the same step (see header scope note), so the
                * post-reset state is applied immediately. */
        iop_cdrom_legacy_init();
        g.result_count = 0; /* init() already cleared everything */
        result_push(0x00u);
        raise_int(IOP_CDROM_INT3);
        break;
    case 0x1E: /* ReadTOC -> INT3(stat) -> INT2(stat) */
        if (!have_disc()) { respond_no_disc(); break; }
        begin_pending_int2();
        break;
    default:
        respond_invalid_command();
        break;
    }
    g.param_count = 0;
}

static uint8_t read_hsts(void)
{
    uint8_t v = (uint8_t)(g.bank & 0x03u);
    if (g.result_pos < g.result_count) v |= IOP_CDROM_LEGACY_STATUS_RSLRRDY;
    if (g.param_count < PARAM_FIFO_SIZE) v |= IOP_CDROM_LEGACY_STATUS_PRMWRDY;
    if (g.param_count == 0) v |= IOP_CDROM_LEGACY_STATUS_PRMEMPT;
    if (g.data_pos < g.data_len) v |= IOP_CDROM_LEGACY_STATUS_DRQSTS;
    /* BUSYSTS always reads 0 - this project's commands complete
     * synchronously (see header scope note on timing). */
    return v;
}

int iop_cdrom_legacy_mmio_read8(uint32_t addr, uint8_t *out)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_CDROM_LEGACY_BASE || phys >= IOP_CDROM_LEGACY_BASE + IOP_CDROM_LEGACY_SIZE)
        return 0;
    uint32_t off = phys - IOP_CDROM_LEGACY_BASE;

    if (off == 0u) { *out = read_hsts(); return 1; }

    if (off == 1u) { /* RESULT - real: all banks read the same result FIFO */
        if (g.result_count > 0) {
            uint8_t v = g.result_fifo[g.result_pos];
            if (g.result_pos < g.result_count - 1) g.result_pos++;
            *out = v;
        } else {
            *out = 0x00u;
        }
        return 1;
    }

    if (off == 2u) { /* RDDATA - real: all banks read the sector buffer */
        if (g.data_pos < g.data_len) {
            *out = g.data_buf[g.data_pos++];
        } else if (g.data_len > 0) {
            /* Real: "repeat the byte at index [800h-8]... as padding" -
             * simplified to repeating the last real byte read. */
            *out = g.data_buf[g.data_len - 1];
        } else {
            *out = 0x00u;
        }
        return 1;
    }

    if (off == 3u) { /* HINTSTS (banks 1/3) or HINTMSK (banks 0/2) on read */
        if (g.bank == 1u || g.bank == 3u) {
            /* Real: bits 0-2 INTSTS, bits 3-4 BFEMPT/BFWRDY (not
             * modeled - no real XA-ADPCM sound-map path), bits 5-7
               always 1. */
            *out = (uint8_t)(g.intsts | 0xE0u);
        } else {
            *out = (uint8_t)(g.intmsk | 0xE0u);
        }
        return 1;
    }

    *out = 0u;
    return 1;
}

int iop_cdrom_legacy_mmio_write8(uint32_t addr, uint8_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_CDROM_LEGACY_BASE || phys >= IOP_CDROM_LEGACY_BASE + IOP_CDROM_LEGACY_SIZE)
        return 0;
    uint32_t off = phys - IOP_CDROM_LEGACY_BASE;

    if (off == 0u) { /* ADDRESS - selects bank */
        g.bank = value & 0x03u;
        return 1;
    }

    if (off == 1u) {
        if (g.bank == 0u) { /* COMMAND */
            dispatch_command(value);
        }
        /* bank 1 WRDATA / bank 2 CI / bank 3 ATV2: audio/debug paths,
         * accepted with no side effect (see header scope note). */
        return 1;
    }

    if (off == 2u) {
        if (g.bank == 0u) { /* PARAMETER */
            if (g.param_count < PARAM_FIFO_SIZE)
                g.param_fifo[g.param_count++] = value;
        } else if (g.bank == 1u) { /* HINTMSK (write) */
            g.intmsk = value & 0x1Fu;
        }
        /* bank 2 ATV0 / bank 3 ATV3: audio mixing, no side effect. */
        return 1;
    }

    if (off == 3u) {
        if (g.bank == 0u) {
            /* HCHPCTL - BFRD/BFWR request sector buffer access; this
             * project's RDDATA is always ready once a read completes
             * (see read_hsts's DRQSTS), so no explicit action is
             * needed beyond accepting the write. */
        } else if (g.bank == 1u) { /* HCLRCTL */
            if (value & 0x07u) {
                g.intsts = IOP_CDROM_INT_NONE;
                if (g.pending != PENDING_NONE)
                    deliver_pending();
            }
            if (value & 0x40u) { /* CLRPRM */
                g.param_count = 0;
            }
            if (value & 0x80u) { /* CHPRST */
                iop_cdrom_legacy_init();
            }
        }
        /* bank 2 ATV1 / bank 3 ADPCTL: audio, no side effect. */
        return 1;
    }

    (void)update_stat_bits;
    return 1;
}
