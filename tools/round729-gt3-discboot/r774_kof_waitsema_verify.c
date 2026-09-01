/*
 * Round 774 scratch: instrumented WaitSema/SignalSema/iSignalSema
 * verification driver (task #447 follow-up, per user's explicit
 * 7-point checklist). Compiled ONLY against a scratch copy of
 * ee_core.c (/tmp/r774_scratch) with temporary logging hooks inserted
 * at the WaitSema entry/result and SignalSema/iSignalSema entry
 * points - the real tracked ee_core.c is untouched by this file.
 *
 * Purpose: distinguish "bad syscall dispatch" from "correct dispatch,
 * but nothing real ever signals this semaphore" for King of Fighters
 * 2000-2001 (real disc, SLES_528.76), and cross-check against Tekken
 * Tag Tournament (full) and Klonoa 2 at the same WaitSema checkpoint
 * class already documented in Round 772/773.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/iop_cdvd.h"

/* --- rate-limited logging hooks called from the scratch-patched ee_core.c --- */
static uint64_t g_entry_calls = 0;
static uint64_t g_entry_printed = 0;
static uint32_t g_last_semid = 0xFFFFFFFFu;
static int g_last_valid = -1;

void r774_log_waitsema_entry(uint32_t pc, uint32_t semid, int valid, int count, uint64_t instr)
{
    g_entry_calls++;
    /* Print on: first-ever call, any semid change, any valid-flag
     * change, or every 20,000,000th call as a heartbeat (bounds
     * output during a long busy-park without hiding real state
     * changes). */
    int changed = (semid != g_last_semid) || (valid != g_last_valid);
    if (g_entry_calls == 1 || changed || (g_entry_calls % 20000000ull) == 0) {
        fprintf(stderr, "[R774-WAIT-ENTRY] call#%llu pc=0x%08x semid=%u valid=%d count=%d instr=%llu\n",
                (unsigned long long)g_entry_calls, pc, semid, valid, count, (unsigned long long)instr);
        g_entry_printed++;
    }
    g_last_semid = semid;
    g_last_valid = valid;
}

static uint64_t g_result_calls = 0;
static int g_last_blocked = -1;
void r774_log_waitsema_result(uint32_t pc, uint32_t semid, int blocked, uint32_t v0)
{
    g_result_calls++;
    int changed = (blocked != g_last_blocked);
    if (g_result_calls <= 5 || changed) {
        fprintf(stderr, "[R774-WAIT-RESULT] call#%llu pc=0x%08x semid=%u blocked=%d v0=0x%08x\n",
                (unsigned long long)g_result_calls, pc, semid, blocked, v0);
    }
    g_last_blocked = blocked;
}

static uint64_t g_signal_calls = 0;
void r774_log_signalsema(uint32_t pc, int32_t sysn, uint32_t semid, int valid, int wait_threads, uint64_t instr)
{
    g_signal_calls++;
    fprintf(stderr, "[R774-SIGNAL] call#%llu pc=0x%08x sysnum=%d semid=%u valid=%d wait_threads=%d instr=%llu\n",
            (unsigned long long)g_signal_calls, pc, sysn, semid, valid, wait_threads, (unsigned long long)instr);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 400000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    ee_state_t *ee = ee_core_get_state();

    uint64_t chunk = 10000000ull, done = 0;
    uint32_t last_pc = 0;
    int same_pc_streak = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        fprintf(stderr, "[R774] instr=%llu pc=0x%08x halted=%u\n",
                (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
        fflush(stderr);
        if (ee->pc == last_pc) same_pc_streak++; else same_pc_streak = 0;
        last_pc = ee->pc;
        if (same_pc_streak >= 4) {
            fprintf(stderr, "[R774] pc unchanged for 4+ consecutive chunks - likely parked, stopping early\n");
            break;
        }
    }

    printf("[R774-FINAL] instr=%llu pc=0x%08x halted=%u reason=%s\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
           ee->halt_reason[0] ? ee->halt_reason : "(none)");
    printf("[R774-SUMMARY] waitsema_entry_calls=%llu waitsema_result_calls=%llu signalsema_calls=%llu\n",
           (unsigned long long)g_entry_calls, (unsigned long long)g_result_calls, (unsigned long long)g_signal_calls);
    return 0;
}
