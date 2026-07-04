/*
 * iop_hle_bios.c - IOP BIOS syscall trap. See iop_hle_bios.h for the
 * full scope explanation (this is a generic PS1-style A0/B0/C0
 * dispatch mechanism, NOT a port of PCSX2's real, much more involved
 * IopBios.cpp - and specific function-number semantics are
 * deliberately not guessed at, with one narrow, well-evidenced
 * exception - InstallExceptionHandlers, C0h/0x07 - see below and the
 * header comment).
 */
#include "core/hw/iop_hle_bios.h"
#include <string.h>
#include <stdio.h>

static iop_hle_bios_state_t g_hle;

/* The well-known real-hardware "general exception vector" trampoline
 * - see the header comment's InstallExceptionHandlers note. Fixed
 * shape (LUI $k0,0 / ADDIU $k0,$k0,imm / JR $k0 / NOP); only the
 * ADDIU immediate (the jump target) varies by BIOS revision, which is
 * exactly why this is located in the actual loaded ROM rather than
 * hardcoded from the public documentation's example values. */
#define EXC_TEMPLATE_WORD0 0x3C1A0000u /* LUI  $k0, 0x0000        */
#define EXC_TEMPLATE_WORD2 0x03400008u /* JR   $k0                */
#define EXC_TEMPLATE_WORD3 0x00000000u /* NOP (branch delay slot) */
#define EXC_VECTOR_ADDR    0x00000080u /* hardware-mandated general exception vector */
#define EXC_GARBAGE_MIRROR 0x00000000u /* psx-spx's documented "Garbage Area" echo   */

static uint8_t  g_exc_template[16];
static uint8_t  g_exc_template_scanned; /* 1 once we've looked, whether or not found */

void iop_hle_bios_init(void)
{
    memset(&g_hle, 0, sizeof(g_hle));
    memset(g_exc_template, 0, sizeof(g_exc_template));
    g_exc_template_scanned = 0;
}

iop_hle_bios_state_t *iop_hle_bios_get_state(void) { return &g_hle; }

static const char *table_name(uint32_t pc)
{
    switch (pc) {
        case IOP_HLE_TABLE_A0: return "A0";
        case IOP_HLE_TABLE_B0: return "B0";
        case IOP_HLE_TABLE_C0: return "C0";
        default: return "?";
    }
}

/* Little-endian 32-bit read straight out of the ROM byte buffer (the
 * BIOS image is a raw little-endian PS2 ROM dump, same convention as
 * bios_loader.c's ROMDIR parsing). */
static uint32_t rom_read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Scans the loaded BIOS ROM for the distinctive, fixed 3-of-4-words
 * signature of the real general-exception-vector trampoline (see the
 * header comment). Returns 1 and fills out[16] with the real 16 bytes
 * found (verbatim, including whatever revision-specific jump target
 * this exact dump uses) if found; returns 0 (out left untouched) if
 * not - e.g. a differently-structured BIOS revision, or no BIOS
 * loaded. Only the first match is used; in practice this signature is
 * expected to appear exactly once in a real dump (confirmed for the
 * SCPH-10000 dump used during this project's real-BIOS testing - see
 * docs/STATUS.md). */
static int find_exception_handler_template(const bios_image_t *bios, uint8_t out[16])
{
    if (!bios || !bios->data || bios->size < 16)
        return 0;

    uint32_t limit = bios->size - 16;
    for (uint32_t off = 0; off <= limit; off += 4) {
        const uint8_t *p = bios->data + off;
        if (rom_read32(p) != EXC_TEMPLATE_WORD0)
            continue;
        if (rom_read32(p + 8) != EXC_TEMPLATE_WORD2)
            continue;
        if (rom_read32(p + 12) != EXC_TEMPLATE_WORD3)
            continue;
        memcpy(out, p, 16);
        return 1;
    }
    return 0;
}

/* InstallExceptionHandlers (C0h/0x07), real behavior instead of the
 * generic default - see the header comment for the full rationale and
 * citation. Writes the real, dump-specific 16-byte trampoline to the
 * hardware exception vector (RAM address 0x80) and mirrors it to
 * address 0 (psx-spx's documented "Garbage Area" echo). Only does
 * anything if the signature was actually found in the loaded ROM;
 * otherwise this is a silent no-op and the call still gets the usual
 * generic default return value. */
static void try_install_exception_handlers(iop_state_t *st)
{
    if (!g_exc_template_scanned) {
        g_hle.exception_handler_template_found =
            (uint8_t)find_exception_handler_template(st->bios, g_exc_template);
        g_exc_template_scanned = 1;
    }

    if (!g_hle.exception_handler_template_found)
        return;

    for (int i = 0; i < 16; i += 4) {
        uint32_t word = rom_read32(g_exc_template + i);
        iop_mem_write32(st, EXC_VECTOR_ADDR + i, word);
        iop_mem_write32(st, EXC_GARBAGE_MIRROR + i, word);
    }
    g_hle.exception_handler_installed = 1;
}

int iop_hle_bios_try_handle(iop_state_t *st, uint32_t pc)
{
    if (pc != IOP_HLE_TABLE_A0 && pc != IOP_HLE_TABLE_B0 && pc != IOP_HLE_TABLE_C0)
        return 0;

    uint32_t function = st->gpr[9];  /* $t1 - the real PS1/PS2 BIOS call convention register */
    uint32_t ra        = st->gpr[31]; /* $ra - where to return to */

    g_hle.calls_seen++;
    g_hle.last_table    = pc;
    g_hle.last_function = function;
    snprintf(g_hle.last_call_desc, sizeof(g_hle.last_call_desc),
             "%s table, function 0x%02X", table_name(pc), (unsigned int)function);

    if (pc == IOP_HLE_TABLE_C0 && function == 0x07u) {
        try_install_exception_handlers(st);
    }

    /* No other specific function-number behavior is implemented (see
     * the header comment for why) - every other call gets a generic
     * default return value of 0 in $v0 (r2), matching real MIPS
     * calling convention for a single-word return value, and control
     * returns to the caller exactly as if a real `JR $ra` had
     * executed. */
    st->gpr[2] = 0; /* $v0 = 0 */

    st->pc      = ra;
    st->next_pc = ra + 4;

    return 1;
}
