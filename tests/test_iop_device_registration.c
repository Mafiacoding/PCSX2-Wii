/*
 * test_iop_device_registration.c - host-native test for Round 29
 * continued's 6th change: A(96h) AddCDROMDevice() and A(97h)
 * AddMemCardDevice(), implemented as real, active, queryable device
 * registration (not demo/no-op stubs), per explicit user request.
 *
 * See include/core/hw/iop_hle_bios.h's IOP_HLE_A0_ADDCDROMDEVICE and
 * IOP_HLE_A0_ADDMEMCARDDEVICE comments for the full design rationale,
 * including why this project does not (yet) write a fabricated DCB
 * struct into emulated RAM: psx-spx documents the DCB table's address
 * and total size but not a citable, byte-exact per-entry layout, and
 * this project only writes struct layouts to real guest RAM once it
 * has directly confirmed them (see the jmp_buf and ExCB chain-node
 * formats elsewhere in this file).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/iop/iop_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static iop_state_t *fresh_iop(void)
{
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    iop_core_init(&bios);
    return iop_core_get_state();
}

int main(void) {
    iop_state_t *st = fresh_iop();
    iop_hle_bios_state_t *hle = iop_hle_bios_get_state();

    /* --- Fresh core: neither device is registered yet --- */
    CHECK(hle->cdrom_device_registered == 0, "cdrom_device_registered starts at 0 (not registered)");
    CHECK(hle->memcard_device_registered == 0, "memcard_device_registered starts at 0 (not registered)");
    CHECK(hle->add_cdrom_device_calls == 0, "add_cdrom_device_calls starts at 0");
    CHECK(hle->add_memcard_device_calls == 0, "add_memcard_device_calls starts at 0");

    /* --- A(96h) AddCDROMDevice(): real registration, not a no-op --- */
    {
        st->gpr[9]  = IOP_HLE_A0_ADDCDROMDEVICE;
        st->gpr[31] = 0xBFC00100u;
        int handled = iop_hle_bios_try_handle(st, IOP_HLE_TABLE_A0);
        CHECK(handled == 1, "A(96h) AddCDROMDevice was recognized and handled");
        CHECK(hle->cdrom_device_registered == 1,
              "AddCDROMDevice genuinely flipped cdrom_device_registered to 1 (real, not demo)");
        CHECK(hle->add_cdrom_device_calls == 1, "add_cdrom_device_calls incremented to 1");
        CHECK(st->gpr[2] == 0u, "AddCDROMDevice returns 0 in $v0");
        CHECK(st->pc == 0xBFC00100u, "control returned to $ra after AddCDROMDevice");
        /* MemCard must be unaffected by the CDROM call. */
        CHECK(hle->memcard_device_registered == 0,
              "memcard_device_registered still 0 - AddCDROMDevice didn't touch it");
    }

    /* --- A(97h) AddMemCardDevice(): real registration, independent of CDROM --- */
    {
        st->gpr[9]  = IOP_HLE_A0_ADDMEMCARDDEVICE;
        st->gpr[31] = 0xBFC00104u;
        int handled = iop_hle_bios_try_handle(st, IOP_HLE_TABLE_A0);
        CHECK(handled == 1, "A(97h) AddMemCardDevice was recognized and handled");
        CHECK(hle->memcard_device_registered == 1,
              "AddMemCardDevice genuinely flipped memcard_device_registered to 1 (real, not demo)");
        CHECK(hle->add_memcard_device_calls == 1, "add_memcard_device_calls incremented to 1");
        CHECK(st->gpr[2] == 0u, "AddMemCardDevice returns 0 in $v0");
        CHECK(st->pc == 0xBFC00104u, "control returned to $ra after AddMemCardDevice");
        /* CDROM registration from the previous block must still hold. */
        CHECK(hle->cdrom_device_registered == 1,
              "cdrom_device_registered is still 1 - independent of AddMemCardDevice");
    }

    /* --- Idempotency: calling either function again is safe, matches
     * real hardware's "already registered" behavior (psx-spx's
     * testdevice() note: re-registering an existing device is a
     * harmless no-op), and the counters keep incrementing (so this
     * project can still observe how many times real BIOS code called
     * each function). --- */
    {
        st->gpr[9]  = IOP_HLE_A0_ADDCDROMDEVICE;
        st->gpr[31] = 0xBFC00108u;
        iop_hle_bios_try_handle(st, IOP_HLE_TABLE_A0);
        CHECK(hle->cdrom_device_registered == 1,
              "calling AddCDROMDevice a second time leaves it registered (idempotent)");
        CHECK(hle->add_cdrom_device_calls == 2,
              "add_cdrom_device_calls incremented again on the second call");
    }

    if (failures) {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("\nAll checks passed.\n");
    return 0;
}
