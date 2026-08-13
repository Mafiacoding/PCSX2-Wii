/*
 * Round 575 (task #550) checkpoint round-trip verification driver.
 * Host-native only, not part of the Wii build (see tools/README
 * convention - every roundNNN-* driver here is a scratch test
 * harness for the emulator core, compiled standalone).
 *
 * Boots the diskless JP-BIOS path to a modest instruction budget,
 * saves a checkpoint, keeps running further (mutating live state),
 * then loads the checkpoint back and confirms the restored state
 * exactly matches the pre-mutation snapshot (instructions_executed,
 * EE pc, a sampled EE RAM window, and GS pmode/dispfb2) - proving
 * checkpoint_save()/checkpoint_load() round-trip correctly.
 */
#include <stdio.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/gs.h"
#include "core/checkpoint.h"

int main(void)
{
    bios_image_t bios;
    if (bios_load("/tmp/round238_diag/bios.bin", &bios) != 0) {
        printf("[FAIL] could not load BIOS\n");
        return 1;
    }
    if (system_init(&bios, &bios) != 0) {
        printf("[FAIL] system_init failed\n");
        return 1;
    }

    /* Run to a modest milestone. */
    system_run_interleaved(2000000);

    ee_state_t *ee = ee_core_get_state();
    gs_state_t *gs = gs_get_state();

    uint64_t snap_instr = ee->instructions_executed;
    uint32_t snap_pc = ee->pc;
    uint8_t snap_ram[64];
    memcpy(snap_ram, ee->ram + 0x100000, sizeof(snap_ram));
    uint32_t snap_pmode = gs->pmode;
    uint32_t snap_dispfb2 = gs->dispfb2;

    printf("[INFO] pre-checkpoint: instr=%llu pc=0x%08x pmode=0x%02x dispfb2=0x%08x\n",
           (unsigned long long)snap_instr, snap_pc, snap_pmode, snap_dispfb2);

    if (checkpoint_save("/tmp/round575_ckpt.bin") != 0) {
        printf("[FAIL] checkpoint_save failed\n");
        return 1;
    }
    printf("[INFO] checkpoint saved\n");

    /* Mutate: run further, changing instructions_executed/pc/RAM/GS state. */
    system_run_interleaved(500000);
    printf("[INFO] post-mutation: instr=%llu pc=0x%08x\n",
           (unsigned long long)ee->instructions_executed, ee->pc);

    if (checkpoint_load("/tmp/round575_ckpt.bin", &bios, &bios, NULL) != 0) {
        printf("[FAIL] checkpoint_load failed\n");
        return 1;
    }

    ee = ee_core_get_state(); /* re-fetch: pointer identity unchanged, but be explicit */
    gs = gs_get_state();

    int ok = 1;
    if (ee->instructions_executed != snap_instr) { printf("[FAIL] instr mismatch: %llu != %llu\n", (unsigned long long)ee->instructions_executed, (unsigned long long)snap_instr); ok = 0; }
    if (ee->pc != snap_pc) { printf("[FAIL] pc mismatch: 0x%08x != 0x%08x\n", ee->pc, snap_pc); ok = 0; }
    if (memcmp(ee->ram + 0x100000, snap_ram, sizeof(snap_ram)) != 0) { printf("[FAIL] RAM window mismatch\n"); ok = 0; }
    if (gs->pmode != snap_pmode) { printf("[FAIL] pmode mismatch: 0x%02x != 0x%02x\n", (unsigned)gs->pmode, (unsigned)snap_pmode); ok = 0; }
    if (gs->dispfb2 != snap_dispfb2) { printf("[FAIL] dispfb2 mismatch: 0x%08x != 0x%08x\n", (unsigned)gs->dispfb2, (unsigned)snap_dispfb2); ok = 0; }

    if (ok) {
        printf("[PASS] checkpoint round-trip: restored state matches pre-checkpoint snapshot exactly\n");
        return 0;
    }
    printf("[FAIL] checkpoint round-trip mismatch\n");
    return 1;
}
