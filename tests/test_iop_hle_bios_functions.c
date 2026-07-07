/*
 * test_iop_hle_bios_functions.c - host-native test for the real
 * A0-table pure-computation BIOS calls added to source/hw/iop_hle_bios.c
 * this round (ABS/LABS, STRCAT/STRNCAT, STRCMP/STRNCMP, STRCPY/
 * STRNCPY, STRLEN, BCOPY/BZERO, MEMCPY/MEMSET/MEMMOVE, INITHEAP,
 * FLUSHCACHE, EXIT/_EXIT) - see iop_hle_bios.h's header comment for
 * the psx-spx citation and exact scope.
 *
 * Calls iop_hle_bios_try_handle() directly with hand-set registers
 * (same technique test_iop_hle_bios.c already uses for its B0-table
 * check) rather than hand-encoding MIPS programs for every case -
 * these calls don't need real instruction-level control flow to
 * exercise, just correct register-in/memory-out behavior.
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

static void put_str(iop_state_t *st, uint32_t addr, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i <= n; i++) /* include NUL */
        iop_mem_write8(st, addr + (uint32_t)i, (uint8_t)s[i]);
}

static char get_str(iop_state_t *st, uint32_t addr, char *out, size_t max)
{
    size_t i = 0;
    for (; i < max - 1; i++) {
        uint8_t c = iop_mem_read8(st, addr + (uint32_t)i);
        out[i] = (char)c;
        if (c == 0) return 0;
    }
    out[i] = 0;
    return 0;
}

/* Sets up gpr[9]=function, gpr[4..7]=a0..a3, gpr[31]=ra, calls the A0
 * trap, and returns. */
static void call_a0(iop_state_t *st, uint32_t function,
                     uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                     uint32_t ra)
{
    st->gpr[9]  = function;
    st->gpr[4]  = a0;
    st->gpr[5]  = a1;
    st->gpr[6]  = a2;
    st->gpr[7]  = a3;
    st->gpr[31] = ra;
    iop_hle_bios_try_handle(st, IOP_HLE_TABLE_A0);
}

int main(void)
{
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    iop_core_init(&bios);
    iop_state_t *st = iop_core_get_state();
    iop_hle_bios_state_t *hle = iop_hle_bios_get_state();

    /* Use a scratch region well away from RAM address 0 (which real
     * code also uses) - arbitrary offsets into IOP RAM, plenty of
     * room in the 2MB model. */
    const uint32_t SCRATCH = 0x00010000u;

    /* --- ABS / LABS --- */
    call_a0(st, IOP_HLE_A0_ABS, (uint32_t)(int32_t)-42, 0, 0, 0, 0x1000);
    CHECK(st->gpr[2] == 42, "ABS(-42) == 42");
    call_a0(st, IOP_HLE_A0_LABS, 42, 0, 0, 0, 0x1000);
    CHECK(st->gpr[2] == 42, "LABS(42) == 42");

    /* --- STRLEN --- */
    put_str(st, SCRATCH, "hello");
    call_a0(st, IOP_HLE_A0_STRLEN, SCRATCH, 0, 0, 0, 0x1000);
    CHECK(st->gpr[2] == 5, "STRLEN(\"hello\") == 5");

    /* --- STRCPY --- */
    put_str(st, SCRATCH, "world");
    call_a0(st, IOP_HLE_A0_STRCPY, SCRATCH + 0x100, SCRATCH, 0, 0, 0x1000);
    {
        char buf[16];
        get_str(st, SCRATCH + 0x100, buf, sizeof(buf));
        CHECK(strcmp(buf, "world") == 0, "STRCPY copied \"world\" correctly");
        CHECK(st->gpr[2] == SCRATCH + 0x100, "STRCPY returns dst in $v0");
    }

    /* --- STRNCPY (src shorter than maxlen -> zero-padded, no extra NUL needed) --- */
    put_str(st, SCRATCH, "ab");
    for (int i = 0; i < 8; i++) iop_mem_write8(st, SCRATCH + 0x200 + i, 0xFF); /* poison */
    call_a0(st, IOP_HLE_A0_STRNCPY, SCRATCH + 0x200, SCRATCH, 6, 0, 0x1000);
    {
        int ok = 1;
        const char expect[6] = {'a','b',0,0,0,0};
        for (int i = 0; i < 6; i++)
            if (iop_mem_read8(st, SCRATCH + 0x200 + i) != (uint8_t)expect[i]) ok = 0;
        CHECK(ok, "STRNCPY(\"ab\", maxlen=6) zero-pads correctly");
    }

    /* --- STRCAT --- */
    put_str(st, SCRATCH, "foo");
    put_str(st, SCRATCH + 0x50, "bar");
    call_a0(st, IOP_HLE_A0_STRCAT, SCRATCH, SCRATCH + 0x50, 0, 0, 0x1000);
    {
        char buf[16];
        get_str(st, SCRATCH, buf, sizeof(buf));
        CHECK(strcmp(buf, "foobar") == 0, "STRCAT(\"foo\",\"bar\") == \"foobar\"");
    }

    /* --- STRNCAT (always NUL-terminates) --- */
    put_str(st, SCRATCH, "x");
    put_str(st, SCRATCH + 0x50, "abcdef");
    call_a0(st, IOP_HLE_A0_STRNCAT, SCRATCH, SCRATCH + 0x50, 3, 0, 0x1000);
    {
        char buf[16];
        get_str(st, SCRATCH, buf, sizeof(buf));
        CHECK(strcmp(buf, "xabc") == 0, "STRNCAT(\"x\",\"abcdef\",3) == \"xabc\"");
    }

    /* --- STRCMP --- */
    put_str(st, SCRATCH, "abc");
    put_str(st, SCRATCH + 0x50, "abc");
    call_a0(st, IOP_HLE_A0_STRCMP, SCRATCH, SCRATCH + 0x50, 0, 0, 0x1000);
    CHECK(st->gpr[2] == 0, "STRCMP(\"abc\",\"abc\") == 0");
    put_str(st, SCRATCH + 0x50, "abd");
    call_a0(st, IOP_HLE_A0_STRCMP, SCRATCH, SCRATCH + 0x50, 0, 0, 0x1000);
    CHECK((int32_t)st->gpr[2] < 0, "STRCMP(\"abc\",\"abd\") < 0");

    /* --- STRNCMP --- */
    put_str(st, SCRATCH, "abcXX");
    put_str(st, SCRATCH + 0x50, "abcYY");
    call_a0(st, IOP_HLE_A0_STRNCMP, SCRATCH, SCRATCH + 0x50, 3, 0, 0x1000);
    CHECK(st->gpr[2] == 0, "STRNCMP(\"abcXX\",\"abcYY\",3) == 0 (differ only past n)");

    /* --- BCOPY (src,dst,len - REVERSED from memcpy) --- */
    put_str(st, SCRATCH, "srcdata");
    call_a0(st, IOP_HLE_A0_BCOPY, SCRATCH, SCRATCH + 0x300, 8, 0, 0x1000);
    {
        char buf[16];
        get_str(st, SCRATCH + 0x300, buf, sizeof(buf));
        CHECK(strcmp(buf, "srcdata") == 0, "BCOPY(src,dst,len) copied src->dst (reversed arg order)");
    }

    /* --- BZERO --- */
    for (int i = 0; i < 8; i++) iop_mem_write8(st, SCRATCH + 0x400 + i, 0xAA);
    call_a0(st, IOP_HLE_A0_BZERO, SCRATCH + 0x400, 8, 0, 0, 0x1000);
    {
        int ok = 1;
        for (int i = 0; i < 8; i++)
            if (iop_mem_read8(st, SCRATCH + 0x400 + i) != 0) ok = 0;
        CHECK(ok, "BZERO zeroed 8 bytes");
    }

    /* --- MEMCPY --- */
    put_str(st, SCRATCH, "memcpytest");
    call_a0(st, IOP_HLE_A0_MEMCPY, SCRATCH + 0x500, SCRATCH, 11, 0, 0x1000);
    {
        char buf[16];
        get_str(st, SCRATCH + 0x500, buf, sizeof(buf));
        CHECK(strcmp(buf, "memcpytest") == 0, "MEMCPY copied correctly");
        CHECK(st->gpr[2] == SCRATCH + 0x500, "MEMCPY returns dst in $v0");
    }

    /* --- MEMSET --- */
    call_a0(st, IOP_HLE_A0_MEMSET, SCRATCH + 0x600, 0x7A, 4, 0, 0x1000);
    {
        int ok = 1;
        for (int i = 0; i < 4; i++)
            if (iop_mem_read8(st, SCRATCH + 0x600 + i) != 0x7A) ok = 0;
        CHECK(ok, "MEMSET filled 4 bytes with 0x7A");
        CHECK(st->gpr[2] == SCRATCH + 0x600, "MEMSET returns dst in $v0");
    }

    /* --- MEMMOVE (matches documented-buggy forward-copy, not overlap-safe) --- */
    put_str(st, SCRATCH + 0x700, "abcdefgh");
    /* Overlapping move: dst = src+2, len=6 - a correct memmove would
     * produce "ababcdgh"-ish non-corrupted output; the documented
     * real-hardware bug (plain forward copy) instead corrupts it by
     * propagating already-overwritten bytes forward. We assert the
     * BUGGY behavior specifically, matching psx-spx's ";Bugged" note. */
    call_a0(st, IOP_HLE_A0_MEMMOVE, SCRATCH + 0x700 + 2, SCRATCH + 0x700, 6, 0, 0x1000);
    {
        uint8_t expect[6];
        /* Reproduce the same buggy forward-copy byte-by-byte to
         * compute the expected corrupted result independently of the
         * implementation under test. */
        uint8_t src[8] = {'a','b','c','d','e','f','g','h'};
        for (int i = 0; i < 6; i++) {
            src[2 + i] = src[i]; /* forward copy corrupts as it goes, same as the impl */
            expect[i] = src[2 + i];
        }
        int ok = 1;
        for (int i = 0; i < 6; i++)
            if (iop_mem_read8(st, SCRATCH + 0x700 + 2 + i) != expect[i]) ok = 0;
        CHECK(ok, "MEMMOVE matches documented-buggy forward-copy behavior on overlap");
    }

    /* --- INITHEAP (bookkeeping only) --- */
    call_a0(st, IOP_HLE_A0_INITHEAP, 0x00020000u, 0x1000u, 0, 0, 0x1000);
    CHECK(hle->heap_initialized == 1, "INITHEAP recorded as initialized");
    CHECK(hle->heap_addr == 0x00020000u && hle->heap_size == 0x1000u,
          "INITHEAP recorded addr/size correctly");

    /* --- FLUSHCACHE (no-op, must not halt or corrupt state) --- */
    call_a0(st, IOP_HLE_A0_FLUSHCACHE, 0, 0, 0, 0, 0x1000);
    CHECK(st->halted == 0, "FLUSHCACHE is a correct no-op (core not halted)");

    /* --- known_calls_handled sanity: every real-behavior call above
     * should have incremented it, generic-default calls should not. */
    uint64_t known_before = hle->known_calls_handled;
    call_a0(st, 0x7Fu /* unassigned/reserved function number */, 0, 0, 0, 0, 0x1000);
    CHECK(hle->known_calls_handled == known_before,
          "an unimplemented function number does NOT increment known_calls_handled");
    CHECK(st->gpr[2] == 0, "unimplemented function number still gets the generic default ($v0=0)");

    /* --- EXIT halts the core with a descriptive reason --- */
    CHECK(st->halted == 0, "core not yet halted before EXIT test");
    call_a0(st, IOP_HLE_A0_EXIT, 7 /* exitcode */, 0, 0, 0, 0x1000);
    CHECK(st->halted == 1, "EXIT halts the core");
    CHECK(hle->exited == 1 && hle->exit_code == 7, "EXIT recorded exited=1, exit_code=7");
    CHECK(strstr(st->halt_reason, "EXIT") != NULL, "halt_reason mentions EXIT");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
