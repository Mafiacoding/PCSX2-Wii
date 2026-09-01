/*
 * iop_module_loader.c - see include/core/hw/iop_module_loader.h for
 * scope, mechanism, and citations.
 */
#include "core/hw/iop_module_loader.h"
#include "core/hw/iop_elf.h"
#include "core/hw/sif.h"
#include "core/hw/iop_intc.h"
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_hle_thread.h" /* Round 389: real THREADMAN thread/semaphore HLE */
#include "core/hw/iop_hle_heap.h" /* Round 421: real SYSMEM heap-export sentinel gates */
#include <string.h>
#include <stdio.h>

#define ROMDIR_ENTRY_SZ 16
#define ROMDIR_SCAN_LIMIT 0x10000u
#define ROM_MAX_ENTRIES 512
#define MODLIST_MAX 48
#define MODNAME_MAX 32
#define EXPORT_REGISTRY_MAX 48

/* Bump allocator start - chosen well clear of the low IOP RAM
 * addresses this project's own boot-time trace (docs/STATUS.md's
 * "round 14" section) showed real BIOS boot code already occupying
 * (the fatal JALR itself was found around pc=0x00000E0C). Not a real
 * hardware convention - real SYSMEM/loadcore would use a real heap
 * allocator, which doesn't exist yet (see iop_hle_bios.c's INITHEAP
 * note for the same honest simplification elsewhere in this
 * project). */
#define BUMP_BASE 0x00100000u

/* Initial stack pointer for each module's entry point. Real IOP
 * kernel modules assume a working stack is already set up before
 * their _start runs (their own prologues save/restore $ra and
 * locals via $sp-relative stores) - something this project never
 * modeled before this round, since no code ever got far enough to
 * need it. No specific real initial-SP value is cited/verified
 * here (unlike the ELF/relocation/table format work, which IS
 * fully cited - see iop_elf.h) - this is a standard, defensively
 * reasonable "top of RAM, growing down" MIPS kernel convention,
 * honestly labeled as this project's own choice, not a verified
 * real hardware constant. Discovered necessary by tracing a real
 * bug this round: without it, the first loaded module's function
 * prologue corrupted its own saved $ra via a stack write to
 * whatever garbage address $sp held, causing it to "return" to
 * address 0 instead of this loader's trampoline. */
/* Round 416 (task #152): 0x100 bytes of headroom below the top of
 * 2MB IOP RAM was never enough - live watches (docs/STATUS.md Round
 * 415/416) proved LOADCORE's own real init function's stack frame
 * (nested alloca()-sized buffers for its module-count-dependent
 * registration-list walk) needs more than that, and every
 * stack-relative access past the 2MB boundary silently reads back 0
 * / silently drops writes (iop_mem_ptr()'s own bounds check returns
 * NULL past st->ram_size - see iop_core.c), which is exactly what a
 * live watch on this loop's own retry counter showed: permanently
 * stuck at 0, its own increment never actually persisting. Widened
 * to 0x4000 (16KB) - still safely within the documented gap between
 * the thread-stack-arena's own end (0x001F0000, see iop_heap.c's own
 * arena-placement comment) and the top of RAM (0x00200000), leaving
 * a large, defensively generous margin for any single module's own
 * init-time stack usage without colliding with that separate arena. */
#define INITIAL_SP (IOP_RAM_SIZE_CONST - 0x4000u)
#define IOP_RAM_SIZE_CONST 0x00200000u

/* Module-entry $a0 argument (task #92's documented "third boundary",
 * closed this round). Disassembly of the real, loaded SYSMEM module's
 * own init code (docs/STATUS.md's "Round 17" section) shows, at the
 * very start of its entry function:
 *     lw   v0, (a0)
 *     sll  sp, v0, 0x14        -- sp = (*a0) * 0x100000
 *     addiu sp, sp, -0x40
 * i.e. SYSMEM expects a0 to point at a single word giving the number
 * of MEGABYTES of IOP RAM, which it left-shifts by 20 (multiply by
 * 0x100000) to compute a top-of-RAM initial stack pointer. Before this
 * round, this project's loader left a0 = 0, so *a0 read whatever
 * happened to be at IOP RAM address 0 (typically 0), collapsing sp to
 * 0 - which this round traced all the way through to a genuine,
 * previously-mysterious failure mode: as the (wrongly near-zero) sp
 * gets used for ordinary stack-relative pushes by later code, it
 * walks down through and past low RAM addresses, including address
 * 0x80 - the hardware-mandated general exception vector
 * (iop_hle_bios.c's EXC_VECTOR_ADDR) - silently overwriting the real,
 * dump-specific exception trampoline InstallExceptionHandlers had
 * correctly installed there. Any later exception (a real SYSCALL, in
 * the traced case) then vectors through 0x80 into garbage instead of
 * the real handler.
 *
 * Real PS2 IOP hardware invariantly has exactly 2MB of RAM across
 * every consumer model (unlike the EE side, this is not configurable)
 * - the same constant this file already uses as IOP_RAM_SIZE_CONST.
 * So this is not a guess at unknown/variable hardware behavior; it is
 * supplying the one value real hardware could ever supply here, given
 * the real, disassembled shift-by-20 computation cited above. Backed
 * by that citation, not fabricated further. */
#define BOOT_INFO_RAM_MB 2u

/* Round 29 continued (12th change): a live-traced disassembly of
 * SYSMEM's own real init code (docs/STATUS.md's "Round 29 continued
 * (12th change)" section - RAM 0x100D00-0x100D8C) shows it reads a
 * LARGER boot-info struct than just the single RAM-MB word: offsets
 * 0x04/0x08/0x10/0x14/0x18/0x1C are copied into local stack slots
 * (not otherwise used in the disassembled span), but offset 0x0C is
 * actively DEREFERENCED - stored into a fixed global slot, read back,
 * and then written through as a pointer (`sw $zero,($a0)`, i.e. "zero
 * out whatever this points to"). Before this change, this project
 * left offsets 0x04 onward at 0 (the bump allocator only ever
 * reserved the first 4 bytes - see BOOT_INFO_RAM_MB's original
 * comment), so offset 0x0C's value was 0, making that final store
 * write to real RAM ADDRESS 0 - an actively observed, real bug, not a
 * hypothetical one (confirmed via live tracing, the same way the
 * INITIAL_SP bug immediately below was confirmed).
 *
 * No citable real value for what offset 0x0C *should* point to was
 * found (same search as the 9th finding: psx-spx, ps2tek, PCSX2
 * upstream, an independent PS2-boot write-up, and a live pcsx2-mcp
 * reference instance - see the 10th finding). Rather than guess a
 * fabricated "real" target, this project applies the exact same
 * honest mitigation already precedented by INITIAL_SP below: point
 * offset 0x0C at a dedicated, zero-initialized scratch word this
 * project itself bump-allocates, so the observed real write-through
 * lands somewhere safe instead of stomping on RAM address 0. This is
 * explicitly a defensive choice, not a verified real hardware value -
 * exactly like INITIAL_SP's own comment says of itself. Offsets
 * 0x04/0x08/0x10/0x14/0x18/0x1C remain honestly zero (their real
 * values, if any, are still unknown - not fabricated). */
#define BOOT_INFO_STRUCT_SIZE 0x20u /* offsets 0x00-0x1C, 8 words */
#define BOOT_INFO_OFF_RAM_MB     0x00u
#define BOOT_INFO_OFF_SCRATCH_PTR 0x0Cu
#define BOOT_INFO_OFF_LIST_COUNT 0x18u /* see build_real_registration_list() */
#define BOOT_INFO_OFF_LIST_PTR   0x1Cu /* see build_real_registration_list() */

typedef struct {
    char name[10 + 1];
    uint32_t payload_off;
    uint32_t size;
} romdir_entry_t;

typedef struct {
    uint32_t fptr_table_addr; /* address of the export table's fptrs[0] slot */
    uint32_t fptr_count;
    char name[9];
} export_registry_entry_t;

static struct {
    int attempted;
    int booted_ok;

    romdir_entry_t romdir[ROM_MAX_ENTRIES];
    int romdir_count;
    uint32_t romdir_base_file_off; /* file offset payloads are relative to (always 0 - see bios_loader.c) */

    char modlist[MODLIST_MAX][MODNAME_MAX];
    int modlist_count;
    int modlist_index; /* next module to try, once the current one returns */

    uint32_t bump_next;
    uint32_t trampoline_addr;
    int idle_transition_done; /* Round 425: see the one-time gate at this trap's own call site */
    uint32_t boot_info_addr; /* see BOOT_INFO_RAM_MB's comment below */

    export_registry_entry_t exports[EXPORT_REGISTRY_MAX];
    int export_count;

    /* Round 29 continued (31st change): front-loading support - see
     * load_all_modules()'s header comment below. entry_points[i] is
     * modlist[i]'s real entry point (0 = failed to load), computed
     * once by load_all_modules() before any entry point runs.
     * elf_results[i] keeps each module's iop_elf_load() output (in
     * particular its import table list) around so the deferred
     * second-pass linking step doesn't need to re-parse the ELF. */
    uint32_t entry_points[MODLIST_MAX];
    iop_elf_load_result_t elf_results[MODLIST_MAX];
    int all_loaded;

    /* Round 29 continued (task #158): registration_list_slot_addr[i]
     * is the IOP RAM address of modlist[i]'s own word slot inside the
     * real registration list build_real_registration_list() builds
     * (0 if modlist[i] failed to load and so has no slot). Used by
     * mark_module_dispatched() to patch a module's own slot from a
     * real header pointer to an inert tag word the INSTANT it starts
     * executing - see that function's header comment for the full
     * reasoning (docs/STATUS.md's 38th finding). */
    uint32_t registration_list_slot_addr[MODLIST_MAX];

    iop_module_loader_stats_t stats;
} g;

void iop_module_loader_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.bump_next = BUMP_BASE;
}

iop_module_loader_stats_t *iop_module_loader_get_stats(void) { return &g.stats; }

int iop_module_loader_get_module_count(void) { return g.modlist_count; }

const char *iop_module_loader_get_module_name(int index)
{
    if (index < 0 || index >= g.modlist_count) return NULL;
    return g.modlist[index];
}

uint32_t iop_module_loader_get_module_entry(int index)
{
    if (index < 0 || index >= g.modlist_count) return 0u;
    return g.entry_points[index];
}

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Same signature-based ROMDIR-location approach as bios_loader.c
 * (real BIOS dumps put RESET/ROMDIR at a revision-dependent file
 * offset - there is no universal fixed offset, see that file's own
 * header comment for the original discovery of this). Reimplemented
 * here (rather than calling into bios_loader.c) because that file's
 * ROMDIR walk is private to its own libfat-based bios_load() path and
 * only extracts ROMVER - this loader needs the full entry list with
 * computed sequential payload offsets. */
static int locate_and_parse_romdir(const uint8_t *data, uint32_t size)
{
    if (size < 0x20) return 0;
    uint32_t limit = (size < ROMDIR_SCAN_LIMIT) ? size : ROMDIR_SCAN_LIMIT;
    uint32_t romdir_off = 0xFFFFFFFFu;
    for (uint32_t off = 0; off + 10 <= limit; off++) {
        if (memcmp(data + off, "RESET\0\0\0\0\0", 10) == 0) {
            /* Confirm entry 1 is "ROMDIR" itself, per the universal
             * convention bios_loader.c documents. */
            if (off + ROMDIR_ENTRY_SZ + 6 <= size &&
                memcmp(data + off + ROMDIR_ENTRY_SZ, "ROMDIR", 6) == 0) {
                romdir_off = off;
                break;
            }
        }
    }
    if (romdir_off == 0xFFFFFFFFu) return 0;

    g.romdir_count = 0;
    uint32_t payload_off = 0;
    uint32_t off = romdir_off;
    while (g.romdir_count < ROM_MAX_ENTRIES && off + ROMDIR_ENTRY_SZ <= size) {
        char name[11];
        memcpy(name, data + off, 10);
        name[10] = '\0';
        uint16_t extinfo = rd_le16(data + off + 10);
        uint32_t psize = rd_le32(data + off + 12);
        if (name[0] == '\0' && extinfo == 0 && psize == 0) break; /* real terminator entry */

        romdir_entry_t *e = &g.romdir[g.romdir_count++];
        /* Round 332: `name` (the local 11-byte buffer just above) is
         * already explicitly null-terminated at index 10 before this
         * point, so a straight memcpy of all 11 bytes is both fully
         * safe and clearer than the previous strncpy(...,10) - which
         * was ALSO safe (the explicit e->name[10]='\0' right after it
         * guaranteed termination regardless of what strncpy copied),
         * but triggered a real, if harmless, devkitPPC/GCC
         * -Wstringop-truncation warning because strncpy's own
         * semantics can't statically prove non-truncation from a
         * 10-byte source with no guaranteed embedded NUL. No
         * behavioral change - same bytes end up in e->name either way. */
        memcpy(e->name, name, 11);
        e->payload_off = payload_off;
        e->size = psize;

        payload_off += (psize + 15u) & ~15u;
        off += ROMDIR_ENTRY_SZ;
    }
    return g.romdir_count > 0;
}

static const romdir_entry_t *romdir_find(const char *name)
{
    for (int i = 0; i < g.romdir_count; i++)
        if (strcmp(g.romdir[i].name, name) == 0)
            return &g.romdir[i];
    return NULL;
}

/* IOPBTCONF/IOPBTCON2 are plain ASCII text: an "@800" header line
 * (see iop_module_loader.h's citation trail) followed by one ROMDIR
 * module name per line. Blank lines are skipped. */
static void parse_boot_config(const uint8_t *payload, uint32_t size)
{
    g.modlist_count = 0;
    uint32_t i = 0;
    int first_line = 1;
    while (i < size && g.modlist_count < MODLIST_MAX) {
        uint32_t start = i;
        while (i < size && payload[i] != '\n' && payload[i] != '\r' && payload[i] != '\0') i++;
        uint32_t len = i - start;
        while (i < size && (payload[i] == '\n' || payload[i] == '\r' || payload[i] == '\0')) i++;

        if (len == 0) continue;
        if (first_line) { first_line = 0; continue; } /* the "@800" line */
        if (len > MODNAME_MAX - 1) len = MODNAME_MAX - 1;

        memcpy(g.modlist[g.modlist_count], payload + start, len);
        g.modlist[g.modlist_count][len] = '\0';
        g.modlist_count++;
    }
}

static uint32_t bump_alloc(uint32_t size)
{
    uint32_t addr = g.bump_next;
    g.bump_next += (size + 15u) & ~15u; /* 16-byte align, generous */
    return addr;
}

static void export_registry_add(const char *name, uint32_t fptr_table_addr, uint32_t fptr_count)
{
    if (g.export_count >= EXPORT_REGISTRY_MAX) return;
    export_registry_entry_t *e = &g.exports[g.export_count++];
    strncpy(e->name, name, 8);
    e->name[8] = '\0';
    e->fptr_table_addr = fptr_table_addr;
    e->fptr_count = fptr_count;
}

static const export_registry_entry_t *export_registry_find(const char *name)
{
    for (int i = 0; i < g.export_count; i++)
        if (strncmp(g.exports[i].name, name, 8) == 0)
            return &g.exports[i];
    return NULL;
}

/* Round 59 (90th/91st findings, task #219): real ps2sdk ships several
 * modules as a "P"/non-P ("I") twin pair sharing one ROMDIR name
 * prefix (INTRMANP/INTRMANI, TIMEMANP/TIMEMANI, ...) - each twin's
 * own real `_start()` reads COP0 PRId + this project's own already-
 * modeled `iop_sbus_ctrl`/ICFG bit 3 (see iop_icfg.c) to decide,
 * independently, whether IT is the one that should register itself
 * as resident on real hardware; on a real PS2 booting natively (not
 * PS1-BC mode - the 91st finding fixed this project's own PRId to
 * confirm this), the non-P ("I") twin is always the real winner.
 *
 * Until now, this project's `load_only_one()` (below) registered
 * BOTH twins' export tables unconditionally at ELF-parse time,
 * regardless of that real runtime decision, and `export_registry_
 * find()`'s first-match-wins scan meant whichever twin happened to
 * load earlier in the boot list (always the P twin, per every real
 * ROMDIR ordering observed so far) permanently shadowed the other -
 * the 90th finding's root cause for TIMEMAN's `AllocHardTimer`
 * resolving against the wrong (restricted, 16-bit-only) table.
 *
 * This is a narrow, name-pattern-based fix (matches ps2sdk's own
 * real P/I naming convention exactly, not a guess) rather than the
 * fuller "trace real RegisterLibraryEntries calls" architecture
 * change floated in the 90th finding - lower risk, directly targets
 * the one real, confirmed symptom, and doesn't touch load order or
 * timing for any module outside a real P/I pair. */
static int module_has_i_twin(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len >= MODNAME_MAX || name[len - 1] != 'P')
        return 0;
    char twin[MODNAME_MAX];
    memcpy(twin, name, len - 1);
    twin[len - 1] = 'I';
    twin[len] = '\0';
    for (int i = 0; i < g.modlist_count; i++) {
        if (strcmp(g.modlist[i], twin) == 0)
            return 1;
    }
    return 0;
}

/* Round 760 (task #747-749): real PS2 IOP hardware does NOT load
 * every module the same way. Per this project's own uploaded 2002-
 * 2003 [RO]man clean-room reimplementation source (sysmem.c's own
 * header: "[loaded @] 00000830-00001500"; excepman.c's own header:
 * "[loaded @] 00003430-00003D00") - independently cross-verified
 * this round by parsing THIS EXACT uploaded scph10000.bin's real
 * ROMDIR and confirming both modules' real payload file offsets
 * match those documented comments exactly (docs/STATUS.md Round
 * 760) - SYSMEM and EXCEPMAN load at fixed, hardcoded low IOP RAM
 * addresses, not a relocatable/dynamic one. This matches independent
 * public documentation (ps2tek/forum research, same round) that
 * IOPBOOT loads SYSMEM (and LOADCORE) directly, before LOADCORE's
 * own general module-boot sequence (which loads EXCEPMAN first)
 * even begins - a different, more primitive mechanism than the
 * uniform ROMDIR-driven loop this function implements for every
 * other module.
 *
 * This explains why SYSMEM's own real, unrelocated (Round 758)
 * ordinal-10 (QueryBlockSize) export stub hardcodes an absolute
 * jump to 0x0000044C (Round 755): a real, deliberate reference into
 * EXCEPMAN's own fixed-address `common[16]` exception-dispatch
 * table (Round 757/759/760 - see excepman.c's own `common=0x440`
 * assignment and its trailing struct-layout comment, `first` at
 * 0x400-0x43F immediately followed by `common` at 0x440-0x47F) -
 * only sensible on hardware where these two modules are NOT
 * relocated arbitrarily far from that shared low-memory region, as
 * this project's uniform bump_alloc(>=0x100000) scheme (until this
 * round) always did.
 *
 * Returns 0 for every module NOT in this small, evidenced set - the
 * normal bump_alloc() path (this function's own caller-visible
 * behavior) is unchanged for everything else. Deliberately NOT
 * extended to LOADCORE/INTRMAN/IOMAN/etc without the same direct,
 * cross-verified evidence this round obtained for these specific
 * two - see docs/STATUS.md Round 760's own "not yet extended"
 * note. */
static uint32_t kernel_tier_fixed_address(const char *name)
{
    if (strcmp(name, "SYSMEM") == 0)   return 0x00000830u;
    if (strcmp(name, "EXCEPMAN") == 0) return 0x00003430u;
    return 0;
}

/* Loads one module by ROMDIR name and registers its own exports.
 * Returns its entry point (or 0 on any failure - missing ROMDIR
 * entry, malformed ELF, etc; the caller decides whether to skip it).
 *
 * Round 29 continued (31st change): this used to ALSO resolve the
 * module's own imports inline, one module at a time, interleaved
 * with running each one's entry point - meaning a module could only
 * ever resolve imports against modules that happened to load EARLIER
 * in the boot list, and any already-loaded module's own code/data
 * (e.g. LOADCORE's real init, which the 27th finding in
 * docs/STATUS.md traced walking its own internal registration list)
 * only ever saw whatever partial state existed at that one moment,
 * since nothing else had been loaded yet. Import linking is now a
 * separate, deferred step (link_imports_one(), below) that
 * load_all_modules() runs in its own second pass, once every listed
 * module has already been loaded and every real export table is
 * already known - see load_all_modules()'s own header comment for
 * the full rationale (docs/STATUS.md's "31st finding"). */
static uint32_t load_only_one(iop_state_t *st, const char *name, iop_elf_load_result_t *out)
{
    g.stats.modules_attempted++;

    const romdir_entry_t *rd = romdir_find(name);
#ifdef IOP_MODLOADER_DEBUG
    fprintf(stderr, "[modloader] load_only_one('%s') rd=%p\n", name, (void*)rd);
#endif
    if (!rd || rd->size == 0) return 0;
    if ((uint64_t)rd->payload_off + rd->size > st->bios->size) return 0;

    /* Round 760 (task #747-749): consumed unconditionally, for EVERY
     * module, purely to keep the bump allocator's running counter -
     * and therefore every OTHER (non-kernel-tier) module's own bump-
     * allocated address - byte-for-byte identical to every prior
     * round's behavior. See kernel_tier_fixed_address()'s own comment
     * for why SYSMEM/EXCEPMAN specifically don't use this value. */
    uint32_t bump_addr = bump_alloc(rd->size + 0x1000u /* headroom for bss + tables, generous */);
    uint32_t fixed_addr = kernel_tier_fixed_address(name);
    uint32_t load_addr = fixed_addr ? fixed_addr : bump_addr;

    const char *err = NULL;
    int rc = iop_elf_load(st, st->bios->data + rd->payload_off, rd->size, load_addr, out, &err);
#ifdef IOP_MODLOADER_DEBUG
    fprintf(stderr, "[modloader]   rc=%d err=%s entry=0x%x\n", rc, err ? err : "(none)", rc==0?out->entry:0);
#endif
    if (rc != 0) return 0;

    if (!fixed_addr) {
        /* Advance the bump allocator to the module's real end (we
         * over-allocated headroom above; this reclaims the unused
         * part for the NEXT module, keeping IOP RAM usage honest). */
        g.bump_next = (out->load_end + 15u) & ~15u;
    }
    /* else: bump_addr's slot is deliberately left reserved/unused -
     * a few KB of permanently-wasted IOP RAM, harmless - so every
     * subsequently-bump-allocated module's own address stays exactly
     * where it already was before this round's fix. Only SYSMEM's
     * and EXCEPMAN's own load address (and therefore entry point and
     * exported-symbol addresses) actually changes. */

    g.stats.modules_loaded++;

    /* Register this module's own export table(s) immediately (not
     * deferred) so EVERY module - including ones loaded earlier in
     * the same front-loading pass - can resolve imports against it
     * once link_imports_one() runs for everyone.
     *
     * Round 59 (90th/91st findings, corrected Round 60/93rd finding,
     * decoupled from PRId Round 72/112th-113th findings): skip this
     * for a "P" twin whose real "I" counterpart is also present in
     * the modlist - see module_has_i_twin()'s own comment above for
     * the full real citation trail.
     *
     * This used to also be gated on `st->cop0[15] >= 16u` (only apply
     * the real-hardware twin-selection when PRId matches the retail
     * value), on the theory that this project's PRId should stay at
     * its safe value of 0 until a future round could model the real
     * early-ROM device/driver-table validation routine at
     * 0xBFC4A000-0xBFC4A488 (92nd/93rd findings) well enough to set
     * PRId=0x1f without dead-ending the boot.
     *
     * Round 72 (112th/113th findings) established that gate was
     * solving the wrong layer of the problem. This project's IOP
     * module loader (task #92) is a SYNTHESIZED substitute for real
     * BIOS ROM boot code, not an interpretation of it - it only ever
     * runs when the IOP's PC has escaped into memory this project
     * doesn't model as real, fetchable ROM/RAM content (see this
     * function's own fallback-trigger, `iop_core.c`'s `iop_step()`).
     * With PRId left at 0, real BIOS ROM code never reaches (and this
     * project therefore never has to interpret) the 0xBFC4A000+
     * device-table routine at all - this loader's synthesized module
     * list is what actually runs instead. Since that whole routine
     * is bypassed either way while PRId=0, tying the (separate,
     * already-fully-diagnosed - 89th/90th findings) P/I twin-export-
     * shadowing fix to PRId's value was conflating two independent
     * questions: "which twin's exports should a synthesized loader
     * prefer" (answer: always the "I" twin when present - that's
     * what every real retail console's genuine PRId=0x1f actually
     * selects, per ps2sdk's own cited P/I convention) and "is it safe
     * to let real ROM code re-derive that same answer for itself"
     * (answer: not yet - the device-table routine is still
     * unmodeled). This project's PRId register itself is intentionally
     * left unmodified (see iop_core.c's own citation trail) - only
     * the synthesized loader's twin-selection outcome is corrected
     * here, independent of PRId, since it never depends on actually
     * executing the real ROM code that would otherwise derive it. */
    if (!module_has_i_twin(name)) {
        for (int i = 0; i < out->export_count; i++) {
            export_registry_add(out->exports[i].name,
                                 out->exports[i].addr + 20u /* fptrs[0] - see iop_elf.h layout */,
                                 out->exports[i].fptr_count);
        }
    }

    return out->entry;
}

/* Resolves one already-loaded module's imports against whatever is
 * currently in the export registry. Deferred out of load_only_one()
 * (see that function's header comment) so this can run AFTER every
 * listed module has been loaded, regardless of which order they
 * appear in the boot list. */
static void link_imports_one(iop_state_t *st, const iop_elf_load_result_t *res)
{
    for (int i = 0; i < res->import_count; i++) {
        const iop_elf_import_table_t *imp = &res->imports[i];
        const export_registry_entry_t *exp = export_registry_find(imp->name);

        uint32_t stub_base = imp->addr + 20u; /* stubs[0] - see iop_elf.h layout */
        for (uint32_t s = 0; s < imp->stub_count; s++) {
            uint32_t stub_addr = stub_base + s * 8u;
            uint32_t ori_word = iop_mem_read32(st, stub_addr + 4u);
            uint32_t ord = ori_word & 0xFFFFu; /* low 16 bits carry the function ordinal - see iop_elf.h/DECLARE_IMPORT */

            /* Round 109 (task #172/#247/#249 continuation): five
             * specific real kernel APIs, identified by their real
             * (library name, ordinal) pair exactly as real IOP
             * import tables identify any callee - RegisterIntrHandler
             * (intrman#4)/ReleaseIntrHandler(intrman#5)/
             * RegisterExceptionHandler(excepman#4)/
             * RegisterDefaultExceptionHandler(excepman#6)/
             * ReleaseExceptionHandler(excepman#7), ordinals cited
             * directly from ps2sdk's intrman.h/excepman.h - see
             * core/hw/iop_hle_intr.h for the full design and
             * citations - are redirected to this project's own
             * project-authored HLE gates instead of the real,
             * resolved INTRMAN/EXCEPMAN code address, so real module
             * calls to them populate this project's own clean-room
             * handler-registration table instead of falling into
             * real BIOS-internal bookkeeping this project has never
             * modeled. Checked BEFORE the normal resolution below so
             * it takes priority when both would otherwise apply (exp
             * would be non-NULL for these too, since INTRMAN/EXCEPMAN
             * really do export them - see the 42nd/135th findings). */
            uint32_t intr_sentinel = iop_hle_intr_sentinel_for_import(imp->name, ord);
            /* Round 389: same precedent, extended to THREADMAN's two
             * real exported libraries (thbase/thsemap) - see
             * core/hw/iop_hle_thread.h for the full design and the
             * real (library, ordinal) citations. Checked at the same
             * priority as the intr_sentinel check just above (before
             * normal resolution), for the identical reason: real
             * module code calling e.g. CreateThread should reach this
             * project's own real scheduler, not fall through to
             * THREADMAN's own real ROM code whose internal scheduling
             * bookkeeping this project has never modeled. */
            uint32_t thread_sentinel = iop_hle_thread_sentinel_for_import(imp->name, ord);
            /* Round 421 (task #160, docs/STATUS.md Round 420 root
             * cause): same precedent again, extended to SYSMEM's real
             * heap-management exports (AllocSysMemory/FreeSysMemory/
             * QueryMemSize/QueryMaxFreeMemSize/QueryTotalFreeMemSize)
             * - see core/hw/iop_hle_heap.h for the full design. Real
             * module calls to these now reach this project's own
             * already-tested synthetic heap model (iop_heap.c, Round
             * 401) instead of real, un-coordinated SYSMEM ROM code
             * whose heap arena collides with this project's own
             * separate module-loading bump_alloc() arena. */
            uint32_t heap_sentinel = iop_hle_heap_sentinel_for_import(imp->name, ord);
            if (intr_sentinel != 0) {
                uint32_t j_instr = 0x08000000u | ((intr_sentinel >> 2) & 0x03FFFFFFu);
                iop_mem_write32(st, stub_addr, j_instr);
                g.stats.imports_resolved++;
            } else if (heap_sentinel != 0) {
                uint32_t j_instr = 0x08000000u | ((heap_sentinel >> 2) & 0x03FFFFFFu);
                iop_mem_write32(st, stub_addr, j_instr);
                g.stats.imports_resolved++;
            } else if (thread_sentinel != 0) {
                uint32_t j_instr = 0x08000000u | ((thread_sentinel >> 2) & 0x03FFFFFFu);
                iop_mem_write32(st, stub_addr, j_instr);
                g.stats.imports_resolved++;
            } else if (exp && ord < exp->fptr_count) {
                uint32_t target = iop_mem_read32(st, exp->fptr_table_addr + ord * 4u);
                uint32_t j_instr = 0x08000000u | ((target >> 2) & 0x03FFFFFFu);
                iop_mem_write32(st, stub_addr, j_instr); /* overwrite "jr $ra" with "j target" - see the cited PS2 BIOS book's exact description of this mechanism */
                g.stats.imports_resolved++;
            } else {
                /* Left as the original "jr $ra" (a safe, harmless
                 * no-op return) - not fabricated further. Genuinely
                 * expected sometimes: e.g. an import this project's
                 * real-BIOS-derived boot list doesn't actually
                 * provide at all (as opposed to "not yet loaded" -
                 * now that loading is front-loaded, a still-
                 * unresolved import means the exporting module
                 * genuinely isn't in this boot list, not just an
                 * ordering gap). */
                g.stats.imports_unresolved++;
            }
        }
    }
}

/* Round 29 continued (31st change): front-loads EVERY module in the
 * boot list - parses, relocates, and registers exports for all of
 * them - before running ANY module's entry point, then resolves every
 * module's imports in a separate second pass once every export table
 * is known. This is option (b) from the 27th finding in
 * docs/STATUS.md (task #124/#132's LOADCORE registration-list
 * closure): the hypothesis that real hardware's own boot order loads/
 * relocates multiple modules' images before running any entry point,
 * letting static per-module registration data accumulate first,
 * instead of this project's previous one-module-at-a-time interleaving
 * (load module N, run its entry point to completion, only THEN load
 * module N+1).
 *
 * WHAT THIS DOES NOT CLAIM: this does NOT fabricate any function
 * pointer, struct layout, or registration-list entry - it only
 * changes WHEN this project's own already-existing, already-cited
 * ELF loading/relocation/export-registration logic runs relative to
 * entry-point execution. Every address used is a real, computed
 * relocation result from iop_elf_load(), exactly as before. Whether
 * this actually changes LOADCORE's own real init code's behavior
 * (its registration-list check is a separate, still-unreverse-
 * engineered mechanism from the import/export linking this function
 * touches - see the 27th finding) is an open empirical question this
 * change lets the project actually test, rather than a claim made
 * here. */
static void load_all_modules(iop_state_t *st)
{
    for (int i = 0; i < g.modlist_count; i++) {
        g.entry_points[i] = load_only_one(st, g.modlist[i], &g.elf_results[i]);
    }
    for (int i = 0; i < g.modlist_count; i++) {
        if (g.entry_points[i] != 0) link_imports_one(st, &g.elf_results[i]);
    }
    g.all_loaded = 1;
}

/* Round 29 continued (task #151/#155): populates boot_info[0x18]/
 * [0x1C] with a REAL registration list, in the exact real format
 * this round's live pcsx2-mcp reference-debugger investigation
 * reverse-engineered from real LOADCORE init code (see docs/
 * STATUS.md's 34th/35th findings) - replacing the previous honest
 * "always zero" placeholder (see BOOT_INFO_STRUCT_SIZE's own comment
 * above) with real, structurally-verified content, instead of the
 * safe bypass this project has relied on since task #148/#152.
 *
 * THE REAL FORMAT (traced instruction-by-instruction on a live,
 * real, fully-booted PCSX2 reference instance, not fabricated):
 * LOADCORE's real init code reloads boot_info[0x1C] as a source
 * pointer and boot_info[0x18] as a word count minus one, memcpy's
 * (boot_info[0x18]+1) words from that pointer into a local buffer,
 * SKIPS the first two words unconditionally (their real meaning was
 * not determined by this round's tracing - the walk loop's own first
 * action is to peek at word index 2 and advance past word 0/1
 * regardless of what it finds there), then walks the remaining words
 * one at a time: bit0=1 is a "phase tag" (tag = word>>2, no further
 * effect this round could trace); bit0=0 is a REAL POINTER to a
 * module image header, which real LOADCORE code validates as either
 * a real MIPS COFF header (magic 0x162, "MIPSELMAGIC") or - the path
 * this project's already-loaded modules actually satisfy - a
 * standard Elf32_Ehdr: byte offset +4 (the combined EI_CLASS/EI_DATA
 * bytes) == 0x0101 (ELFCLASS32 | ELFDATA2LSB<<8), +0x12 (e_machine)
 * == 8 (EM_MIPS), +0x2A (e_phentsize) == 0x20 (real Elf32_Phdr size),
 * +0x2C (e_phnum) == 2 (matches this project's own iop_elf.h's cited
 * real Sony IOP convention of exactly 2 program headers: PT_LOAD +
 * the vendor PT_MIPS_IOPMOD segment). The list is zero-terminated
 * (walk continues while the current word is nonzero).
 *
 * WHAT THIS BUILDS: since every module iop_elf_load() front-loads
 * (load_all_modules(), above) is a real ELF32/MIPS image copied
 * byte-for-byte from the real BIOS into IOP RAM at elf_results[i]
 * .load_addr - and a standard Elf32_Ehdr always starts at byte 0 of
 * that image, per the ELF format itself - this function does NOT
 * fabricate any header bytes. It only writes POINTERS: one real
 * bit0=0 pointer word per successfully-loaded module, each pointing
 * straight at that module's own already-resident, already-real
 * Elf32_Ehdr (elf_results[i].load_addr itself, which iop_elf.h's own
 * loader already validated has real e_machine=8/e_phnum=2 fields -
 * see that file's citations). Preceded by two placeholder words this
 * round could not determine the real meaning of (set to 0, matching
 * this project's own established precedent - see
 * BOOT_INFO_OFF_SCRATCH_PTR's comment - of using a safe, explicitly-
 * labeled placeholder rather than a fabricated "real" value when a
 * genuine unknown remains), and followed by one zero terminator word.
 *
 * HONEST SCOPE: this is a well-supported, but NOT yet empirically
 * confirmed, hypothesis about what boot_info[0x18]/[0x1C] must
 * contain - unlike, say, the ELF loader's own citations (iop_elf.h),
 * this exact array shape (particularly the two skipped leading
 * words, and whether "phase tag" entries are also expected somewhere
 * in a real boot) was inferred from tracing ONE real LOADCORE build
 * on ONE live reference instance running ONE game, not cross-checked
 * against a second independent public source. Task #151's own
 * regression/host-native testing (see docs/STATUS.md) reports
 * whether this actually changes real-BIOS IOP behavior for the
 * better, on the same honest, empirical footing as the front-loading
 * refactor's own "implemented, tested, found not to fix it" result -
 * this is not assumed to work merely because it is well-reasoned.
 * The existing panic-loop/trap-stub bypasses (task #148/#152) are
 * deliberately left in place regardless of outcome, as a safety net
 * for whatever this doesn't resolve. */
static void build_real_registration_list(iop_state_t *st)
{
    int n = 0;
    for (int i = 0; i < g.modlist_count; i++) {
        if (g.entry_points[i] != 0) n++;
    }

    uint32_t total_words = (uint32_t)n + 3u; /* 2 leading placeholder words + n pointers + 1 terminator */
    uint32_t list_addr = bump_alloc(total_words * 4u);

    uint32_t w = 0;
    iop_mem_write32(st, list_addr + w * 4u, 0u); w++; /* leading word 0 - real meaning not determined this round */
    iop_mem_write32(st, list_addr + w * 4u, 0u); w++; /* leading word 1 - real meaning not determined this round */
    for (int i = 0; i < g.modlist_count; i++) {
        if (g.entry_points[i] == 0) { g.registration_list_slot_addr[i] = 0u; continue; }
        /* elf_results[i].load_addr is always 16-byte aligned
         * (bump_alloc()'s own guarantee, above) so bit0 is
         * naturally 0 - a real header pointer, per the format
         * derivation above, not a phase-tag word. */
        iop_mem_write32(st, list_addr + w * 4u, g.elf_results[i].load_addr);
        g.registration_list_slot_addr[i] = list_addr + w * 4u; /* task #158 */
        w++;
    }
    iop_mem_write32(st, list_addr + w * 4u, 0u); w++; /* terminator */

    iop_mem_write32(st, g.boot_info_addr + BOOT_INFO_OFF_LIST_COUNT, total_words - 1u);
    iop_mem_write32(st, g.boot_info_addr + BOOT_INFO_OFF_LIST_PTR, list_addr);

    g.stats.registration_list_entries = (uint32_t)n;
}

/* Round 29 continued (task #158): see docs/STATUS.md's 38th finding
 * for the full derivation. The 37th finding established that
 * LOADCORE's real registration-list walk directly `jalr`s into each
 * recognized entry's real module entry point - it is an ACTIVE call-
 * dispatch mechanism, not passive bookkeeping. This project's own
 * external sequencer (advance_to_next_module(), below, and this
 * function's caller in iop_module_loader_boot()) ALSO independently
 * runs every module's entry point once. Left unpatched, the real
 * list build_real_registration_list() supplies still shows a module
 * as a live, callable pointer entry even after this project's own
 * sequencer has already started or finished running it - meaning
 * LOADCORE's own walk could call that module's real entry a SECOND
 * time (already-run modules, e.g. SYSMEM, always first) or
 * recursively call itself (the module currently mid-execution, e.g.
 * LOADCORE reaching its own slot in its own list).
 *
 * This function patches a module's own slot in the real list from a
 * real header POINTER (bit0=0) to an inert TAG word (bit0=1) the
 * INSTANT that module starts executing - whether it is module 0
 * starting for the first time (iop_module_loader_boot()) or any
 * later module advance_to_next_module() is about to jump into. A
 * module's slot is therefore a live pointer ONLY while it has not
 * yet started - exactly matching the real bit0=1/bit0=0 tag/pointer
 * distinction already reverse-engineered (34th/35th findings): once
 * a module has started (is running or has finished), it is no longer
 * something LOADCORE's own walk should actively dispatch into again.
 *
 * The tag word chosen is 0x00000003 (bit0=1, low nibble=3): the real
 * walk loop only takes any further action on a tag word when its low
 * NIBBLE (not just bit0) equals decimal 1 (see the loop's own
 * `andi v0,v1,0xF; bne v0,s3` check, s3=1, traced in the 37th
 * finding's disassembly) - any other nibble value is a pure no-op,
 * inert marker. 0x00000003's nibble is 3, deliberately avoiding the
 * one nibble value (1) known to trigger extra, not-yet-understood
 * side effects (saving `word>>2` into a variable this round's tracing
 * never determined the use of). */
static void mark_module_dispatched(int modlist_idx)
{
    if (modlist_idx < 0 || modlist_idx >= g.modlist_count) return;
    uint32_t slot = g.registration_list_slot_addr[modlist_idx];
    if (slot == 0u) return; /* module never loaded - no slot exists */
    /* iop_core_get_state() is declared via core/iop/iop_core.h,
     * already included transitively through this file's own header -
     * safe to call here since this address always comes from
     * bump_alloc(), always within plain IOP RAM, never an MMIO-
     * mapped address. */
    iop_state_t *st = iop_core_get_state();
    iop_mem_write32(st, slot, 0x00000003u);
}

int iop_module_loader_boot(iop_state_t *st)
{
    if (g.attempted) return 0;
    g.attempted = 1;

    if (!st->bios || !st->bios->data || st->bios->size == 0) return 0;
    if (!locate_and_parse_romdir(st->bios->data, st->bios->size)) return 0;

    const romdir_entry_t *btconf = romdir_find("IOPBTCONF");
    if (!btconf) btconf = romdir_find("IOPBTCON2");
    if (!btconf || btconf->size == 0) return 0;
    if ((uint64_t)btconf->payload_off + btconf->size > st->bios->size) return 0;

    parse_boot_config(st->bios->data + btconf->payload_off, btconf->size);
    if (g.modlist_count == 0) return 0;

#ifdef IOP_MODLOADER_DEBUG
    fprintf(stderr, "[modloader] modlist_count=%d\n", g.modlist_count);
    for (int dbgi = 0; dbgi < g.modlist_count; dbgi++)
        fprintf(stderr, "[modloader]   [%d] '%s'\n", dbgi, g.modlist[dbgi]);
#endif

    g.trampoline_addr = bump_alloc(8);
    /* Content is never actually fetched-and-decoded (the trap check
     * in iop_module_loader_try_handle() runs before fetch, exactly
     * like iop_hle_bios_try_handle()) - a self-jump is written purely
     * as a defensive fallback in case something ever reaches this
     * address unexpectedly. */
    iop_mem_write32(st, g.trampoline_addr, 0x08000000u | ((g.trampoline_addr >> 2) & 0x03FFFFFFu));

    g.boot_info_addr = bump_alloc(BOOT_INFO_STRUCT_SIZE);
    for (uint32_t off = 0; off < BOOT_INFO_STRUCT_SIZE; off += 4)
        iop_mem_write32(st, g.boot_info_addr + off, 0u);
    iop_mem_write32(st, g.boot_info_addr + BOOT_INFO_OFF_RAM_MB, BOOT_INFO_RAM_MB);
    {
        /* Dedicated, zero-initialized scratch word offset 0x0C points
         * at - see BOOT_INFO_STRUCT_SIZE's comment above. Allocated
         * separately (not as part of the struct itself) so it isn't
         * disturbed if the struct's own size/layout changes later. */
        uint32_t scratch = bump_alloc(4);
        iop_mem_write32(st, scratch, 0u);
        iop_mem_write32(st, g.boot_info_addr + BOOT_INFO_OFF_SCRATCH_PTR, scratch);
    }

    /* Round 29 continued (31st change): front-load every listed
     * module before running any entry point - see load_all_modules()'s
     * header comment above. */
    load_all_modules(st);

    /* Round 29 continued (task #151/#155): populate boot_info[0x18]/
     * [0x1C] with a real registration list - see
     * build_real_registration_list()'s header comment above. Must run
     * AFTER load_all_modules() (needs every module's real load_addr)
     * and before the first entry point runs (LOADCORE's real init,
     * wherever it sits in the boot list, re-reads boot_info fresh
     * from $a0 every time it runs - see the 34th/35th findings). */
    build_real_registration_list(st);

    g.modlist_index = 0;
    while (g.modlist_index < g.modlist_count && g.entry_points[g.modlist_index] == 0) g.modlist_index++;
    if (g.modlist_index >= g.modlist_count) return 0; /* not even one module in the list could be loaded */

    st->gpr[31] = g.trampoline_addr;
    st->gpr[29] = INITIAL_SP; /* $sp - see INITIAL_SP's comment above */
    st->gpr[4]  = g.boot_info_addr; /* $a0 - see BOOT_INFO_RAM_MB's comment above */
    mark_module_dispatched(g.modlist_index); /* task #158 - see that function's header comment */
    st->pc = g.entry_points[g.modlist_index];
    st->next_pc = st->pc + 4;
    g.booted_ok = 1;
    return 1;
}

/* Round 29 continued (28th change): LOADCORE panic-loop recognition -
 * see docs/STATUS.md's 27th finding for the full root-cause story
 * (task #124/#132). Real LOADCORE module-loader code reaches a
 * genuine, deliberate real-BIOS "panic: write status code 2 to
 * physical RAM address 0, then spin forever" sequence when its own
 * internal multi-phase module/library self-registration list turns
 * up empty - which happens in this project's emulation because this
 * very loader runs exactly one module's ELF and entry point at a
 * time, so no other module has had a chance to register anything
 * into LOADCORE's internal table by the time LOADCORE's own init
 * reaches this check. This is a genuine, structural difference from
 * real hardware's own boot order (real boot almost certainly lets
 * other modules register before LOADCORE's own init reaches this
 * point - see STATUS.md), not a value this project can safely
 * fabricate: the real function-pointer dispatch loop that WOULD read
 * genuine registration entries calls through `jalr` with a real code
 * address read from each entry - an incorrect guess there does not
 * fail safely, it can jump into arbitrary emulated memory as code
 * (unlike this project's earlier, safe pointer-only defensive fixes:
 * INITIAL_SP, boot_info offset 0x0C, both of which only needed a
 * pointer to land somewhere harmless).
 *
 * WHAT THIS DOES INSTEAD: recognizes the exact, distinctive 4-word
 * instruction sequence real LOADCORE code executes at this panic
 * point - `lui $v1,0x8000; addiu $v0,zero,2; sb $v0,($v1); j <self>`
 * (self = a jump back to the sb instruction's own address, forming
 * the infinite loop) - by its literal encoded bytes, not a hardcoded
 * address, so this survives the panic sequence loading at a
 * different address if some other real BIOS build ever shifts
 * LOADCORE's own load offset. On recognizing it, treats reaching
 * this exact point exactly like a module returning through this
 * loader's own trampoline mechanism (already used to sequence every
 * other module's boot): proceeds to the next module in the real
 * IOPBTCONF list instead of letting the real panic sequence execute
 * and spin forever.
 *
 * THIS IS AN EXPLICIT, DOCUMENTED ENGINEERING DECISION ABOUT THIS
 * PROJECT'S OWN EXTERNAL MODULE-SEQUENCING SHORTCUT, NOT A CLAIM
 * ABOUT REAL HARDWARE BEHAVIOR. Real hardware's own LOADCORE most
 * likely never reaches this exact panic on a real console boot -
 * this project simply cannot yet safely replicate the real
 * registration mechanism that would prevent it (see above), so
 * instead of leaving the emulated IOP stuck forever in a real,
 * working-as-designed BIOS panic loop, this loader's own external
 * sequencer takes over at EXACTLY this recognized point, the same
 * honest way it already takes over at its own trampoline return
 * address. */
#define LOADCORE_PANIC_LUI_V1_8000 0x3C038000u /* lui $v1, 0x8000 ($v1=r3) */
#define LOADCORE_PANIC_ADDIU_V0_2  0x24020002u /* addiu $v0, $zero, 2 */
#define LOADCORE_PANIC_SB_V0_V1    0xA0620000u /* sb $v0, 0($v1) ($v1=r3 base) */

static int is_loadcore_panic_loop(iop_state_t *st, uint32_t pc)
{
    if (iop_mem_read32(st, pc)      != LOADCORE_PANIC_LUI_V1_8000) return 0;
    if (iop_mem_read32(st, pc + 4u) != LOADCORE_PANIC_ADDIU_V0_2)  return 0;
    if (iop_mem_read32(st, pc + 8u) != LOADCORE_PANIC_SB_V0_V1)    return 0;

    uint32_t j_instr = iop_mem_read32(st, pc + 12u);
    if ((j_instr >> 26) != 0x02u) return 0; /* must be a real J-type opcode */
    uint32_t j_target = ((j_instr & 0x03FFFFFFu) << 2) | ((pc + 16u) & 0xF0000000u);
    if (j_target != pc + 8u) return 0; /* must jump back to the sb instruction (self-loop) */

    return 1;
}

/* Round 29 continued (32nd change): recognizes a SECOND, distinct
 * "unconditional trap stub" - see docs/STATUS.md's 29th/30th/31st
 * findings (task #151) for the full derivation. Unlike LOADCORE's
 * panic loop above (a direct self-jump), this one is reached through
 * a REAL R3000A exception re-vectoring mechanism: a genuine syscall
 * from a later module (first observed: INTRMANP calling
 * ExitCriticalSection, $a0=2) falls through to the still-unclaimed
 * general exception vector, which LOADCORE's own real init code has
 * by then installed with this exact byte-for-byte prologue, ending in
 * an UNCONDITIONAL TGE (Trap if Greater or Equal, rs==rt so the trap
 * condition is always true) - which immediately re-vectors back to
 * the SAME address forever with zero observable state change
 * (verified via repeated single-step sampling: identical Status/
 * Cause/$k0/$at values every cycle). This is the SAME underlying
 * architectural gap as #124/#132 (LOADCORE's real registration list
 * is empty), surfacing through a different, real syscall-driven path
 * instead of the direct panic-loop jump above.
 *
 * The first 10 words are matched by their EXACT literal bytes (same
 * approach as is_loadcore_panic_loop() - a real, disassembled,
 * verified sequence, not a guess): NOP; SW $k0,0x410($zero); a real
 * but functionally-inert MFHI $zero (nonzero, don't-care shift
 * amount field); MFC0 $at,Status; NOP; SW $at,0x408($zero); a real
 * but functionally-inert ADD $zero,$zero,$zero (nonzero, don't-care
 * shift amount field); NOP; NOP; ANDI $k0,$k0,0x3C. The 11th word is
 * checked STRUCTURALLY rather than by one exact value (SPECIAL,
 * funct=0x30/TGE, rs==rt) because the same stub template was
 * observed reused at a nearby address with a different trap "code"
 * field (0x800000E8: code=3, vs. this one's code=2 at 0x800000A8) -
 * matching the *shape* of "always traps" lets this recognize the
 * same template wherever it recurs, without weakening the byte-exact
 * match on the part that's actually load-bearing (the real register
 * saves and Status read).
 *
 * WHY THIS IS SAFE (identical reasoning to the panic-loop bypass
 * above): every one of the ten SPECIAL/COP0/store instructions this
 * recognizes is real, already-disassembled, already-understood, and
 * has zero externally observable effect on anything this project's
 * interpreter reads back afterward (the two SW targets, 0x410/0x408,
 * are never read by anything else this project has traced; $k0/$at
 * are scratch registers by MIPS convention, not preserved across a
 * real exception anyway). Recognizing the pattern AT ITS START and
 * advancing straight to the next module produces the exact same
 * final, honest outcome as letting the CPU execute all eleven words
 * and then recognizing the trap itself - it does not fabricate,
 * skip past, or alter anything a later module could observe. */
static int is_unconditional_trap_stub(iop_state_t *st, uint32_t pc)
{
    static const uint32_t words[10] = {
        0x00000000u, /* nop */
        0xAC1A0410u, /* sw $k0, 0x410($zero) */
        0x00000090u, /* mfhi $zero (sa=2, functionally inert - rd=0) */
        0x40016000u, /* mfc0 $at, $12 (Status) */
        0x00000000u, /* nop */
        0xAC010408u, /* sw $at, 0x408($zero) */
        0x000000A0u, /* add $zero,$zero,$zero (sa=5, functionally inert - rd=0) */
        0x00000000u, /* nop */
        0x00000000u, /* nop */
        0x335A003Cu, /* andi $k0, $k0, 0x3C */
    };
    for (int i = 0; i < 10; i++) {
        if (iop_mem_read32(st, pc + (uint32_t)i * 4u) != words[i]) return 0;
    }

    uint32_t trap_word = iop_mem_read32(st, pc + 40u);
    if ((trap_word >> 26) != 0u) return 0;       /* must be SPECIAL */
    if ((trap_word & 0x3Fu) != 0x30u) return 0;  /* must be TGE (funct 0x30) */
    uint32_t rs = (trap_word >> 21) & 0x1Fu;
    uint32_t rt = (trap_word >> 16) & 0x1Fu;
    if (rs != rt) return 0; /* must be unconditional: signed rs>=rt always true when rs==rt */

    return 1;
}

/* Round 29 continued (task #157): a THIRD, distinct real panic tail -
 * see docs/STATUS.md's 36th finding for the full derivation. Reached
 * from WITHIN LOADCORE's own real registration-list-walk code once
 * task #151/#155's build_real_registration_list() supplies a real,
 * non-empty boot_info[0x18]/[0x1C] list: live real-BIOS testing
 * showed LOADCORE genuinely walks the real entries this project now
 * provides (no immediate rejection - directionally confirms the 34th/
 * 35th findings' format understanding), but some deeper validation
 * this round didn't fully characterize ultimately still fails,
 * landing in a SECOND real "write a status byte, then spin forever"
 * idiom - structurally the same shape as is_loadcore_panic_loop()'s
 * own target (task #148), but reached via a different real call site:
 * here only the tail 3 words repeat (`sb $v0,($v1)` / `j <self>` /
 * NOP delay slot) - the $v1 "panic status address" and $v0 "status
 * code" registers are already set up by whatever earlier, real,
 * call-site-specific code led here, unlike the original 4-word
 * sequence's own inline `lui $v1,0x8000; addiu $v0,zero,2` setup - so
 * is_loadcore_panic_loop() (which matches those specific 4 words)
 * does not and should not recognize this one; this is a genuinely
 * separate detector, not a generalization of that one.
 *
 * WHY THIS IS SAFE (identical reasoning to the other two bypasses):
 * matches the exact real SB+J+NOP bytes at their literal encoded
 * values (register operands v0/v1 fixed, since that's what's
 * observed; the runtime VALUES those registers hold are whatever
 * upstream real code computed and are not required to be any
 * specific value - only the instruction encoding is matched); the
 * store target and stored byte are never read back by anything this
 * project's interpreter or later modules depend on (same class of
 * argument as the original panic-loop bypass: a real, dead-end status
 * write, not live state). */
static int is_registration_walk_panic_loop(iop_state_t *st, uint32_t pc)
{
    if (iop_mem_read32(st, pc) != 0xA0620000u) return 0; /* sb $v0, 0($v1) */

    uint32_t j_addr = pc + 4u;
    uint32_t j_instr = iop_mem_read32(st, j_addr);
    if ((j_instr >> 26) != 0x02u) return 0; /* must be a real J-type opcode */
    uint32_t j_target = ((j_instr & 0x03FFFFFFu) << 2) | ((j_addr + 8u) & 0xF0000000u);
    if (j_target != pc) return 0; /* must jump back to the sb instruction (self-loop) */

    if (iop_mem_read32(st, j_addr + 4u) != 0x00000000u) return 0; /* delay slot nop */

    return 1;
}

/* Shared by both advance paths below: moves g.modlist_index forward
 * to the next module that actually has a valid (front-loaded) entry
 * point, and if found, sets up registers/pc exactly like
 * iop_module_loader_boot() did for the first module. Returns 1 if it
 * found one and redirected execution, 0 if the list is exhausted. */
static int advance_to_next_module(iop_state_t *st)
{
    g.modlist_index++;
    while (g.modlist_index < g.modlist_count && g.entry_points[g.modlist_index] == 0) g.modlist_index++;
    if (g.modlist_index >= g.modlist_count) return 0;

    st->gpr[31] = g.trampoline_addr;
    st->gpr[29] = INITIAL_SP; /* $sp - see INITIAL_SP's comment above */
    st->gpr[4]  = g.boot_info_addr; /* $a0 - see BOOT_INFO_RAM_MB's comment above */
    mark_module_dispatched(g.modlist_index); /* task #158 - see that function's header comment */
    st->pc = g.entry_points[g.modlist_index];
    st->next_pc = st->pc + 4;
    return 1;
}

/* Task #170/#172: real, documented SIF protocol signal - see
 * ps2sdk's ee/kernel/include/sifdma.h: SIF_STAT_BOOTEND = 0x40000,
 * "Bootup completed". Real hardware's IOP writes this bit into
 * SIF_SMFLG once its own boot-time module loading has finished, and
 * real EE-side kernel code (confirmed via this project's own
 * diagnostic tracing - see docs/STATUS.md's 46th finding) spins on
 * exactly this bit before continuing past its own boot sequence. This
 * project's IOP module loader already has a well-defined, deliberate
 * concept of "the boot sequence is complete" (the three halt sites
 * below), so setting this real, citable bit at exactly that point -
 * ORed onto whatever SIF_SMFLG already holds - is a precedented,
 * non-fabricated fix, not a guess.
 *
 * Round 91 (131st finding, task #172/#247 continuation) correction:
 * the header comment above previously claimed "task #165 already
 * established SIF_STAT_SIFINIT=0x10000 gets set for real via the
 * genuine SIFMAN/SIFCMD handshake" - this was based on an address-
 * computation error in an earlier round's manual disassembly (KSEG1
 * addresses of the form 0xB0xxxxxx were incorrectly assumed to alias
 * the same physical address as the equivalent 0xA0xxxxxx address;
 * they do not - bit 28 survives the real `addr & 0x1FFFFFFF` KSEG
 * mask and is NOT redundant between the 0x8/0x9/0xA halves and the
 * 0x9/0xB halves of KSEG0/KSEG1). Re-disassembling the real kernel's
 * SIF-driver-init routine (EE pc=0x80006198, confirmed live via
 * host-native instruction-level tracing) with the corrected masking
 * shows it polls real phys 0x1000F230 (SIF_SMFLAG, this project's own
 * sif.c) for bit 0x00010000 (SIF_STAT_SIFINIT) WITHOUT ever first
 * writing to SIF_SMFLAG itself - so sif.c's existing reactive
 * SIFINIT-reassertion path (sif_mmio_write32's SIF_SMFLAG case, gated
 * on the EE writing bit 0x00040000) can never fire for this call
 * site, and SIF_STAT_SIFINIT was in fact never being set at all for
 * this real boot path prior to this fix - confirmed by a 35M-
 * instruction host-native run that hit the busy-wait 7,577,906 times
 * without exiting. Real hardware's SIFMAN module is what sets this
 * bit once IOP-side SIF init genuinely completes; this project's own
 * IOP module loader completing (the same event mark_iop_boot_complete
 * already represents for BOOTEND/CMDINIT below) is the equivalent
 * real milestone, so SIFINIT is added here unconditionally rather
 * than left to the narrower reactive path (which remains, unchanged,
 * for the documented _LoadExecPS2 reset-and-resignal case). */
static void mark_iop_boot_complete(void)
{
    /* Task #172 continued: SIF_STAT_CMDINIT (0x20000, "SIFCMD
     * initialized" per ps2sdk's sifdma.h - same citable source as
     * SIF_STAT_BOOTEND above) is added here too. This project's own
     * IOP module loader already represents SIFCMD's real module as
     * having run to completion as part of its front-loaded module
     * list (see the 43rd/44th findings - SIFCMD was one of the
     * modules whose real syscall trap this project bypassed), and
     * real SIFCMD's own init routine is what's responsible for
     * setting exactly this bit on real hardware - so this is a
     * legitimate consequence of work this project already models, not
     * a new fabrication. SIF_STAT_SIFINIT (0x10000, "SIFMAN
     * initialized", same ps2sdk source) is added here too as of Round
     * 91 (131st finding) - see the corrected citation above. */
    uint32_t smflag = 0;
    sif_iop_mmio_read32(0x1D000030u, &smflag);
    sif_iop_mmio_write32(0x1D000030u, smflag | 0x00010000u /* SIF_STAT_SIFINIT */
                                             | 0x00040000u /* SIF_STAT_BOOTEND */
                                             | 0x00020000u /* SIF_STAT_CMDINIT */);
    /* task #212 continuation (82nd/83rd findings): record that real
     * IOP module loading has genuinely completed at least once, so
     * sif.c's SIF_SMFLAG EE-write handler can honestly re-signal these
     * same real bits if OSDSYS's own real code later clears them as
     * part of a _LoadExecPS2-triggered reset - see the full citation
     * in sif.h above sif_note_iop_boot_completed_once()'s declaration. */
    sif_note_iop_boot_completed_once();

    /* Round 263/264/265 (task #423, 303rd/304th/305th finding):
     * REVERTED after further measurement - see the full account in
     * docs/STATUS.md's 305th finding. Short version: setting
     * `DMAC_STAT` bit 0x80 (SIF2) here genuinely unblocks the
     * `0x8000CFD0` OR-condition (real, cited mechanism - see the
     * removed code's own citation, preserved in git history at commit
     * a4e66d9) and, combined with Round 264's `SIF_F260` fix, avoids
     * the real kernel panic Round 263 first hit. BUT Round 265 found
     * that leaving this `DMAC_STAT` bit permanently set (nothing in
     * this project ever acknowledges/clears it, since no real SIF2
     * transfer ever actually completes to trigger a real ack) creates
     * a genuine, persistent DMAC interrupt condition
     * (`dma_dmac_interrupt_pending()`) that combines with the EE's
     * own real, frequently-firing timer interrupt (COP0 Cause
     * observed as `0x8800` = IP7 timer | IP3 DMAC at the exact
     * moment this fires) into a self-sustaining interrupt storm:
     * direct A/B measurement showed 1,285,710 dispatches of the real
     * exception vector in a 42M-instruction run WITH this fix, all of
     * it spent re-entering/re-exiting the exception handler and NEVER
     * once reaching OSDSYS's real per-frame dispatcher
     * (`0x8000CF88`, hit count exactly 0) - versus 4,188,801 real,
     * productive dispatcher visits in the SAME budget WITHOUT this
     * fix (SIF_F260's reactive rule and the broadened SBUS shortcut
     * alone are sufficient to unblock real forward progress, and do
     * so far more successfully than this fix does). Real, cited
     * mechanism or not, this fix produces a strictly worse outcome
     * than not having it - so it's removed. If a future round finds
     * the real, missing "service and acknowledge the SIF2 completion"
     * counterpart (the piece this project doesn't model, matching
     * Round 263's original honest conclusion that a real DATA
     * payload, not just a status bit, is what's actually needed
     * here), it should be re-added together with that fix, not alone. */
}

int iop_module_loader_try_handle(iop_state_t *st, uint32_t pc)
{
    if (g.booted_ok && is_loadcore_panic_loop(st, pc)) {
        g.stats.panic_loops_bypassed++;
        if (advance_to_next_module(st)) return 1;

        static char panic_msg[160];
        snprintf(panic_msg, sizeof(panic_msg),
                 "module boot sequence complete (via LOADCORE panic-loop bypass): "
                 "%u/%u real modules loaded, %u run to completion, %u panic-loop bypass(es) (task #124/#132/#148)",
                 (unsigned)g.stats.modules_loaded, (unsigned)g.modlist_count,
                 (unsigned)g.stats.modules_run_to_completion, (unsigned)g.stats.panic_loops_bypassed);
        mark_iop_boot_complete();
        /* Round 93 (133rd/134th finding, task #172 continuation):
         * this bypass path used to leave the IOP permanently halted,
         * but it represents the exact same real event ("no more real
         * modules to run") as the genuine-completion site below,
         * which task #238 already fixed to use `idle` instead -
         * real IOP hardware never halts regardless of which code
         * pattern this project's own module loader used to recognize
         * "boot is done". Matching that site's own three-part fix
         * (idle=1, IEc=1, exception_pending=0 - see its doc comment
         * for the full citation trail) here too, for consistency. */
        st->idle = 1;
        st->cop0[12] |= 0x1u; /* Status.IEc = 1 */
        st->exception_pending = 0;
        /* Round 519: disabled - see iop_hle_thread.c header comment on iop_hle_thread_retire_root_thread() */
        strncpy(st->halt_reason, panic_msg, sizeof(st->halt_reason) - 1);
        st->halt_reason[sizeof(st->halt_reason) - 1] = '\0';
        return 1;
    }

    if (g.booted_ok && is_unconditional_trap_stub(st, pc)) {
        /* Round 95 (136th finding, task #252): distinguish WHY the
         * empty vector was reached before deciding this means "module
         * complete". See docs/STATUS.md for the full derivation - a
         * live host-native diagnostic this round found the SAME dead
         * vector bytes are reached both by a genuine SYSCALL falling
         * through (the original 29th/30th-finding scenario, correctly
         * treated as "this module's init is done") AND by a genuine
         * hardware INTERRUPT exception (Cause.ExcCode==0, live once
         * Status.IEc/IM2 are set - task #217/88th finding) - and
         * treating an interrupted module the same way as a module
         * that deliberately fell through a syscall was silently
         * discarding dozens of modules' real init code every time ANY
         * interrupt fired while they were running (confirmed via
         * diagnostic: with the old behavior, modules 12-84 of this
         * project's real IOPBTCONF list were being cut off after at
         * most one instruction, before this fix).
         *
         * An interrupted module's own code is fully resumable and was
         * doing real work - unlike a syscall, which is the calling
         * code's own deliberate request. So ExcCode==0 does NOT get
         * treated as module-complete: this project acknowledges
         * (clears) exactly the I_STAT bits that are both pending and
         * enabled in I_MASK (the same bits that caused Cause.IP2 to
         * be set in the first place - the minimal, generic "an
         * unhandled interrupt must be acked or it refires forever"
         * default any real OS needs here, not a guess at PS2-specific
         * semantics, since no real handler exists to cite), then RFEs
         * (the same real Status-stack-pop formula already established
         * for IOP RFE, task #113) back to EPC - resuming the
         * INTERRUPTED module's own real code exactly where it left
         * off, instead of abandoning it.
         *
         * Verified via host-native diagnostic against the real
         * SCPH-10000 BIOS: modules_run_to_completion rose from a
         * small fraction of 29 to 28/29 (only 1 module still needs
         * the trap-stub bypass, now correctly only for the original
         * syscall-fallthrough scenario), and the EE's own real
         * boot progress advanced from its long-stuck 0x8000CFD8
         * resting point (131st/132nd findings) to a new, further
         * address (0x8000F814) - genuine additional real BIOS code
         * now executes on both CPUs. */
        uint32_t exccode = st->cop0[13] & 0x7Cu;
        /* Round 116 (task #271/#172 continuation, 156th finding): the
         * Round 95 fix above (exccode==0 => "genuinely interrupted,
         * resumable module code, RFE back to EPC") silently assumed
         * EPC always points at real, resumable module code distinct
         * from this stub's own fixed address. A host-native
         * diagnostic this round (20M/45M-slice runs against the real
         * SCPH-10000 BIOS, both landing on a byte-for-byte identical
         * frozen final state) proved that assumption false: once EPC
         * itself equals this exact trap-stub's own address (pc), the
         * RFE below jumps straight back into this SAME stub, which
         * re-detects exccode==0 (Cause.ExcCode is left at 0 - never
         * reset to anything else by this branch), re-acks an already-
         * empty firing mask (a no-op the second time), and RFEs to
         * the SAME never-updated EPC again - a permanent, self-
         * sustaining loop that freezes pc at this one address forever
         * (confirmed live: a 64-entry PC ring-buffer sampled at the
         * end of both diagnostic runs showed all 64 most recent PC
         * values identical, all Status/Cause/EPC fields frozen, and
         * zero further real syscalls/interrupts dispatched after the
         * loop starts - not a slow real polling loop, an immediate
         * fixed point). This is architecturally the exact same "no
         * real module code left to resume" dead end the exccode!=0
         * branch below already handles (advance_to_next_module()/
         * mark_iop_boot_complete()) - EPC==pc means the "interrupted"
         * context was itself already sitting at this dead-end stub
         * when the interrupt fired, so there is no real resumable
         * code to RFE back into. Falling through to the exact same
         * module-complete handling used below is the same precedent
         * already established for the exccode!=0 case, not a new
         * mechanism. */
        if (exccode == 0u && st->cop0[14] != pc) {
            g.stats.trap_stubs_bypassed++;
            iop_intc_state_t *intc = iop_intc_get_state();
            uint32_t firing = intc->istat & intc->imask;
            intc->istat &= ~firing;
            st->cop0[12] = (st->cop0[12] & ~0x0Fu) | ((st->cop0[12] & 0x3Cu) >> 2); /* RFE Status pop */
            st->pc = st->cop0[14]; /* EPC */
            st->next_pc = st->pc + 4u;
            st->exception_pending = 0;
            return 1;
        }

        g.stats.trap_stubs_bypassed++;
        if (advance_to_next_module(st)) return 1;

        static char trap_msg[176];
        snprintf(trap_msg, sizeof(trap_msg),
                 "module boot sequence complete (via unconditional-trap-stub bypass): "
                 "%u/%u real modules loaded, %u run to completion, %u trap-stub bypass(es) (task #151/#152)",
                 (unsigned)g.stats.modules_loaded, (unsigned)g.modlist_count,
                 (unsigned)g.stats.modules_run_to_completion, (unsigned)g.stats.trap_stubs_bypassed);
        mark_iop_boot_complete();
        /* Round 93 (133rd/134th finding) - same fix as the panic-loop
         * bypass above, same reasoning: identical real event, just
         * recognized via a different code pattern. */
        st->idle = 1;
        st->cop0[12] |= 0x1u; /* Status.IEc = 1 */
        st->exception_pending = 0;
        /* Round 519: disabled - see iop_hle_thread.c header comment on iop_hle_thread_retire_root_thread() */
        strncpy(st->halt_reason, trap_msg, sizeof(st->halt_reason) - 1);
        st->halt_reason[sizeof(st->halt_reason) - 1] = '\0';
        return 1;
    }

    if (g.booted_ok && is_registration_walk_panic_loop(st, pc)) {
        g.stats.registration_walk_panics_bypassed++;
        if (advance_to_next_module(st)) return 1;

        static char reg_panic_msg[192];
        snprintf(reg_panic_msg, sizeof(reg_panic_msg),
                 "module boot sequence complete (via registration-walk panic bypass): "
                 "%u/%u real modules loaded, %u run to completion, %u registration-walk panic bypass(es) (task #151/#155/#157)",
                 (unsigned)g.stats.modules_loaded, (unsigned)g.modlist_count,
                 (unsigned)g.stats.modules_run_to_completion, (unsigned)g.stats.registration_walk_panics_bypassed);
        mark_iop_boot_complete();
        /* Round 93 (133rd/134th finding) - same fix as the two
         * bypasses above, same reasoning. */
        st->idle = 1;
        st->cop0[12] |= 0x1u; /* Status.IEc = 1 */
        st->exception_pending = 0;
        /* Round 519: disabled - see iop_hle_thread.c header comment on iop_hle_thread_retire_root_thread() */
        strncpy(st->halt_reason, reg_panic_msg, sizeof(st->halt_reason) - 1);
        st->halt_reason[sizeof(st->halt_reason) - 1] = '\0';
        return 1;
    }

    if (!g.booted_ok || pc != g.trampoline_addr) return 0;

    /* Round 425 (task #164, docs/STATUS.md Round 424/425), corrected
     * by Round 426 (task #165): once every module is exhausted, `pc`
     * stays parked at `g.trampoline_addr` forever (nothing moves it
     * away), so this trap re-fires every time IOP returns here with
     * nothing left to dispatch - including legitimately after a real
     * interrupt wakes IOP from idle, its handler runs, and RFEs back
     * to this exact saved EPC. Round 425's first attempt gated the
     * ENTIRE completion block (including `st->idle = 1`) behind a
     * single one-shot flag - live-traced in Round 426 and found to be
     * too broad: after the first real wake, `idle` never got set back
     * to 1, permanently defeating the intended idle/wake cycle (IOP
     * would spin calling this now-harmless no-op every single step
     * instead of genuinely idling again). Corrected split: re-parking
     * into idle (`st->idle = 1`) is a real, repeatable, correct action
     * that must run on every one of these re-entries; only the one-
     * time module-completion bookkeeping below (the message,
     * `mark_iop_boot_complete()`, and the Status.IEc/exception_pending
     * leftover-artifact corrections - see their own comments) is
     * actually meant to run exactly once. */
    if (g.idle_transition_done) {
        st->idle = 1;
        return 1;
    }

    g.stats.modules_run_to_completion++;
    if (advance_to_next_module(st)) return 1;

    /* From here on, every module has genuinely run to completion for
     * the first time - the one-time completion/idle-transition path.
     * Setting the flag here (not above) is what keeps modules 2..N's
     * own normal per-module dispatch working - see this function's
     * own header trap-check comment for the full reasoning. */
    g.idle_transition_done = 1;

    /* Every module in the real BIOS's own IOPBTCONF/IOPBTCON2 list
     * has now been front-loaded (task #92, extended by the 31st
     * change) and had its real entry point executed to completion by
     * this project's actual IOP interpreter - a genuine milestone.
     *
     * Task #179 continued (54th finding): this used to be an
     * unconditional halt() here, on the reasoning that "no public
     * reference describes what real hardware's OWN loadcore loop
     * does immediately after this point, so this project stops here
     * honestly rather than guessing." That reasoning was incomplete -
     * real IOP hardware never halts at all; its kernel scheduler
     * always has at minimum an idle thread to fall back to, and stays
     * interrupt-responsive indefinitely. Halting here was hiding a
     * real, confirmed effect: an EE-side wait loop (0x8000F768, see
     * STATUS.md's 53rd finding) polls DMAC_STAT/INTC_STAT bits that
     * only real IOP-side hardware activity can ever set, and nothing
     * can raise them once the IOP stops running. Switched to the
     * `idle` flag (see its doc comment in iop_core.h) instead of
     * `halted` - same halt_reason-style message kept for diagnostics,
     * but the core keeps ticking (interrupt-responsive, no fetch/
     * decode/execute of fabricated "idle loop" instruction content). */
    static char msg[128];
    snprintf(msg, sizeof(msg),
             "module boot sequence complete: %u/%u real modules loaded, %u run to completion (task #92, idle since task #179)",
             (unsigned)g.stats.modules_loaded, (unsigned)g.modlist_count,
             (unsigned)g.stats.modules_run_to_completion);
    mark_iop_boot_complete();
    st->idle = 1;

    /* Round 93 (133rd finding, task #172 continuation): this project's
     * own idle-mode doc comment (iop_core.h's `idle` field) already
     * promises "stays interrupt-responsive indefinitely" - but a
     * live diagnostic found Status.IEc (cop0[12] bit 0) reads 0 right
     * at this exact transition (confirmed stable/unchanged across a
     * 60M-instruction run, since idle mode never fetches/executes,
     * so nothing could have changed it afterward - this is genuinely
     * the value at entry, not later drift). Status=0x414 decodes to
     * IEc=0/KUc=0 (current) with IEp=1/KUp=0 and IEo=1/KUo=0
     * (previous/old) - the textbook signature of "took an exception,
     * pushed the interrupt-enable stack, never executed a matching
     * RFE" (real R3000A COP0 Status semantics - see this project's
     * own RFE implementation, task #113). The last-run module's
     * front-loaded trampoline jumps straight to the boot-complete
     * path without necessarily having unwound every exception it
     * took along the way, so IEc is left wherever it happened to
     * land.
     *
     * A real kernel's idle thread is definitionally interrupt-
     * responsive - no real OS ever parks its idle loop with
     * interrupts globally masked, since it could then never service
     * even its own scheduler tick. This isn't a guess about specific
     * IOP-side protocol details; it's the same universal invariant
     * this project's own idle-mode doc comment already asserted.
     * Explicitly enabling interrupts here makes that already-decided
     * design intent actually true, rather than silently broken by an
     * unrelated leftover exception-nesting artifact from module
     * bring-up. */
    st->cop0[12] |= 0x1u; /* Status.IEc = 1 */

    /* Round 93 continued (133rd finding): live diagnostic confirms
     * `exception_pending` is ALSO already stuck at 1 at this exact
     * transition (some earlier module's own exception handling never
     * reached a matching RFE before the loader's trampoline forcibly
     * cut over to the boot-complete path) - reproducibly, every run.
     * This second leftover artifact silently defeats the interrupt-
     * wake logic even after the IEc fix above: iop_core_step()'s
     * idle branch only clears `idle` on a `!pending_before &&
     * exception_pending` transition, which can never fire again once
     * exception_pending is already 1 forever, even though
     * iop_check_hw_interrupt() keeps re-vectoring `pc` on every idle
     * step once IEc is set (a real, if narrow, correctness gap in
     * that guard's "was this already pending" check - visible here
     * for the first time because IEc was 0 before this round, so the
     * guard's behavior when re-triggered was previously unreachable).
     * Same reasoning as the IEc fix: entering the synthetic idle
     * state is meant to represent a clean handoff to the real
     * kernel's own idle thread, not a continuation of some other,
     * already-finished module's incidental exception bookkeeping -
     * clearing it here makes that already-decided design intent
     * actually work. */
    st->exception_pending = 0;

    /* Round 519: disabled - see iop_hle_thread.c header comment on iop_hle_thread_retire_root_thread() */

    strncpy(st->halt_reason, msg, sizeof(st->halt_reason) - 1);
    st->halt_reason[sizeof(st->halt_reason) - 1] = '\0';
    return 1;
}
