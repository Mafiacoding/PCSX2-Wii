/*
 * driver_lean_r310b.c - Round 310 lean boot-trace driver with a
 * TARGETED checkpoint/resume mechanism, built directly against the
 * REAL repository's source/include. Unlike the scratch harness's
 * blind whole-[__data_start,_end)-range dump (which crashes on
 * same-binary resume for reasons never fully root-caused - see
 * Round 307/308/309 STATUS.md entries), this driver explicitly
 * accounts for the two heap-allocated pointers (ee_state_t.ram,
 * iop_state_t.ram - both memalign()'d at init, so their pointer
 * VALUE is not stable across process invocations even with the same
 * binary/same input): the raw [__data_start,_end) range is still
 * dumped/restored as one block (all the *fixed-address* global
 * state - g_state's non-ram fields, g_dma, g_gs, g_intc, etc. all
 * live there and their addresses ARE stable across runs, same as
 * before), but immediately after restoring that block the two ram
 * pointers are explicitly re-pointed at THIS process's own freshly
 * memalign()'d buffers (not the stale pointer value the old process
 * had), and the actual RAM CONTENTS are read from two separate,
 * explicit file sections into those correct, current-process
 * pointers. Read-only investigation tool - never committed to the
 * real repo.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <stdarg.h>
#include <execinfo.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/dma.h"
#include "core/hw/iop_dma.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/iop_cdrom_legacy.h"
#include "core/hw/iop_sio2.h"
#include "core/hw/gs.h"
#include "core/hw/iop_heap.h"

/* R313: safe-printf helper (see docs/STATUS.md Round 312) - r313_safe_printf()/
 * stdio reliably crashes on any call made after this checkpoint
 * format's raw restore, for reasons not fully root-caused. This
 * formats via vsnprintf() into a stack buffer and writes it with a
 * raw write() syscall, entirely avoiding the FILE-stream machinery. */
static void r313_safe_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf)) n = sizeof(buf);
        ssize_t w = write(1, buf, (size_t)n);
        (void)w;
    }
}

static void r313_segv(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "[R313-SIGSEGV] fault at addr=%p\n", si->si_addr);
    ssize_t w = write(2, buf, (size_t)n); (void)w;
    void *bt[64];
    int nframes = backtrace(bt, 64);
    write(2, "[R313-BACKTRACE]\n", 18);
    backtrace_symbols_fd(bt, nframes, 2);
    _exit(77);
}

#define CHUNK 5000000ull

extern char __data_start;
extern char _end;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void write_block(FILE *f, const void *addr, size_t size)
{
    uint64_t s = (uint64_t)size;
    fwrite(&s, sizeof(s), 1, f);
    fwrite(addr, 1, size, f);
}

/* Reads into a staged heap buffer first, then memcpy's into place -
 * same defensive pattern tried (unsuccessfully, for the scratch
 * driver) in Round 309, kept here since it is still strictly safer
 * than a direct fread() into live global memory even if it wasn't
 * sufcient on its own there. */
static int read_block_into(FILE *f, void *addr, size_t expect_size)
{
    uint64_t s = 0;
    if (fread(&s, sizeof(s), 1, f) != 1) return -1;
    if (s != (uint64_t)expect_size) {
        fprintf(stderr, "[!] size mismatch: got %llu expected %zu\n", (unsigned long long)s, expect_size);
        return -1;
    }
    void *tmp = malloc(expect_size);
    if (!tmp) return -1;
    if (fread(tmp, 1, expect_size, f) != expect_size) { free(tmp); return -1; }
    memcpy(addr, tmp, expect_size);
    free(tmp);
    return 0;
}

static bios_image_t bios; /* global, not stack-local - see docs/STATUS.md
 * Round 310/312: g_state.bios must point at a STABLE, checkpoint-
 * restorable address, not a dead stack frame from whichever process
 * wrote the checkpoint. */

static void dump_checkpoint(const char *path)
{
    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen write"); return; }
    write_block(f, (void*)&__data_start, (size_t)(&_end - &__data_start));
    write_block(f, ee->ram, ee->ram_size);
    write_block(f, iop->ram, iop->ram_size);
    /* Round 448 (task #247): explicit IOP-heap-chain snapshot -
     * see iop_heap.h's citation on why the raw [__data_start,_end)
     * block alone is not enough for this one piece of state (the
     * only host-heap-allocated global anywhere under source/). */
    {
        uint32_t hsz = iop_heap_snapshot_size();
        void *hbuf = malloc(hsz ? hsz : 1);
        if (hbuf) {
            iop_heap_snapshot_save(hbuf);
            write_block(f, hbuf, hsz);
            free(hbuf);
        }
    }
    fclose(f);
    r313_safe_printf("[+] checkpoint written to %s\n", path);
}

static int load_checkpoint(const char *path)
{
    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    uint8_t *fresh_ee_ram = ee->ram;     /* THIS process's own memalign()'d buffer */
    uint8_t *fresh_iop_ram = iop->ram;   /* THIS process's own memalign()'d buffer */
    uint32_t ee_ram_size = ee->ram_size;
    uint32_t iop_ram_size = iop->ram_size;
    uint8_t *fresh_bios_data = bios.data;
    uint32_t bios_size = bios.size;

    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen read"); return -1; }
    int rc = 0;
    rc |= read_block_into(f, (void*)&__data_start, (size_t)(&_end - &__data_start));
    /* The block above just clobbered ee->ram / iop->ram / bios.data
     * with the OLD process's stale pointer values - fix them back to
     * THIS process's real, valid buffers before touching anything,
     * and re-bind dma.c's/iop_dma.c's own separately-cached copies. */
    ee->ram = fresh_ee_ram;
    ee->ram_size = ee_ram_size;
    iop->ram = fresh_iop_ram;
    iop->ram_size = iop_ram_size;
    bios.data = fresh_bios_data;
    bios.size = bios_size;
    dma_bind_ee_ram(ee->ram, ee->ram_size);
    iop_dma_bind_iop_ram(iop->ram, iop->ram_size);
    rc |= read_block_into(f, ee->ram, ee_ram_size);
    rc |= read_block_into(f, iop->ram, iop_ram_size);
    /* Round 448 (task #247): rebuild the IOP heap chain from its
     * explicit snapshot, in THIS process's own fresh malloc()'d
     * nodes - the raw block above just clobbered g_alloclist with a
     * stale pointer from the writing process (see iop_heap.h's
     * citation), so this must run AFTER that block and BEFORE
     * anything touches the IOP heap. */
    {
        uint64_t hsz64 = 0;
        if (fread(&hsz64, sizeof(hsz64), 1, f) == 1 && hsz64 > 0) {
            void *hbuf = malloc((size_t)hsz64);
            if (hbuf && fread(hbuf, 1, (size_t)hsz64, f) == hsz64) {
                iop_heap_snapshot_load(hbuf, (uint32_t)hsz64);
            } else {
                rc |= -1;
            }
            free(hbuf);
        }
    }
    fclose(f);
    if (rc == 0) r313_safe_printf("[+] resumed from checkpoint %s\n", path);
    return rc;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s run|resume <ckpt_path> <slices>\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    const char *ckpt_path = argv[2];
    uint64_t slices_to_run = strtoull(argv[3], NULL, 10);

    { struct sigaction sa; memset(&sa,0,sizeof(sa)); sa.sa_sigaction=r313_segv; sa.sa_flags=SA_SIGINFO; sigaction(SIGSEGV,&sa,NULL); }

    memset(&bios, 0, sizeof(bios));
    if (bios_load("/tmp/round238_diag/bios.bin", &bios) != 0) { r313_safe_printf("[!] bios_load failed\n"); return 1; }
    if (iop_cdvd_mount_iso("/tmp/round238_diag/disc.iso") != 0) r313_safe_printf("[!] iop_cdvd_mount_iso failed\n");
    iop_cdvd_set_disc_present(0x12);
    if (iop_cdrom_legacy_mount_iso("/tmp/round238_diag/disc.iso") != 0) r313_safe_printf("[!] iop_cdrom_legacy_mount_iso failed\n");
    if (system_init(&bios, &bios) != 0) { r313_safe_printf("[!] system_init failed\n"); return 1; }
    iop_sio2_pad_connect();
    iop_sio2_pad_press(IOP_PAD_BTN_CROSS);

    if (strcmp(mode, "resume") == 0) {
        if (load_checkpoint(ckpt_path) != 0) { r313_safe_printf("[!] load_checkpoint failed\n"); return 1; }
    } else if (strcmp(mode, "run") != 0) {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    ee_state_t *ee = ee_core_get_state();
    gs_state_t *gs = gs_get_state();

    double t0 = now_sec();
    uint64_t total = 0;
    uint64_t remaining = slices_to_run;
    uint64_t last_pad_toggle_at = 0;
    int pad_pressed = 1; /* matches the initial one-shot press already issued above */
    while (remaining > 0) {
        uint64_t chunk = remaining < CHUNK ? remaining : CHUNK;
        int rc = system_run_interleaved(chunk);
        total += chunk;
        remaining -= chunk;
        /* R313: repeated CROSS press/release, not a single one-shot
         * press at boot - real human input to an idle menu/attract
         * screen is a series of taps, not a single held-forever press.
         * Toggle every 2,000,000 slices (~16M instructions at this
         * project's 8:1 EE:IOP interleave ratio) - see docs/STATUS.md
         * Round 312's "next investigative target". */
        if (total - last_pad_toggle_at >= 2000000ull) {
            if (pad_pressed) { iop_sio2_pad_release(IOP_PAD_BTN_CROSS); pad_pressed = 0; }
            else { iop_sio2_pad_press(IOP_PAD_BTN_CROSS); pad_pressed = 1; }
            last_pad_toggle_at = total;
        }
        if (gs->pmode || gs->dispfb1 || gs->display1) {
            r313_safe_printf("[R310b] DISPLAY MILESTONE at total_slices=%llu instr=%llu: pmode=0x%llx dispfb1=0x%llx display1=0x%llx\n",
                (unsigned long long)total, (unsigned long long)ee->instructions_executed,
                (unsigned long long)gs->pmode, (unsigned long long)gs->dispfb1, (unsigned long long)gs->display1);
            break;
        }
        if (rc != 0 && ee->halted) {
            r313_safe_printf("[R310b] EE halted at total_slices=%llu pc=0x%08lX\n", (unsigned long long)total, (unsigned long)ee->pc);
            break;
        }
        double elapsed = now_sec() - t0;
        if (elapsed > 36.0) {
            r313_safe_printf("[R310b] wall-clock budget reached, stopping early at total_slices=%llu\n", (unsigned long long)total);
            break;
        }
    }
    double t1 = now_sec();
    r313_safe_printf("[R310b] FINAL(this-run): total_slices=%llu ee_total_instr=%llu pc=0x%08lX halted=%d elapsed=%.2fs rate=%.0f slices/s\n",
        (unsigned long long)total, (unsigned long long)ee->instructions_executed, (unsigned long)ee->pc,
        ee->halted, t1 - t0, (double)total / (t1 - t0));
    r313_safe_printf("[R310b] GS: pmode=0x%llx dispfb1=0x%llx display1=0x%llx csr=0x%llx\n",
        (unsigned long long)gs->pmode, (unsigned long long)gs->dispfb1,
        (unsigned long long)gs->display1, (unsigned long long)gs->csr);
    { extern uint64_t g_r303_rpc_pending_sets, g_r303_rpc_delivered_count;
      r313_safe_printf("[R313] RPC: pending_sets=%llu delivered=%llu pad_pressed=%d\n",
        (unsigned long long)g_r303_rpc_pending_sets, (unsigned long long)g_r303_rpc_delivered_count, pad_pressed); }

    dump_checkpoint(ckpt_path);
    return 0;
}
