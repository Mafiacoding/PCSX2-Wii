/*
 * iop_module_loader.c - see include/core/hw/iop_module_loader.h for
 * scope, mechanism, and citations.
 */
#include "core/hw/iop_module_loader.h"
#include "core/hw/iop_elf.h"
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
#define INITIAL_SP (IOP_RAM_SIZE_CONST - 0x100u)
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

    iop_module_loader_stats_t stats;
} g;

void iop_module_loader_reset(void)
{
    memset(&g, 0, sizeof(g));
    g.bump_next = BUMP_BASE;
}

iop_module_loader_stats_t *iop_module_loader_get_stats(void) { return &g.stats; }

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
        strncpy(e->name, name, 10);
        e->name[10] = '\0';
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

    uint32_t load_addr = bump_alloc(rd->size + 0x1000u /* headroom for bss + tables, generous */);

    const char *err = NULL;
    int rc = iop_elf_load(st, st->bios->data + rd->payload_off, rd->size, load_addr, out, &err);
#ifdef IOP_MODLOADER_DEBUG
    fprintf(stderr, "[modloader]   rc=%d err=%s entry=0x%x\n", rc, err ? err : "(none)", rc==0?out->entry:0);
#endif
    if (rc != 0) return 0;

    /* Advance the bump allocator to the module's real end (we
     * over-allocated headroom above; this reclaims the unused part
     * for the NEXT module, keeping IOP RAM usage honest). */
    g.bump_next = (out->load_end + 15u) & ~15u;

    g.stats.modules_loaded++;

    /* Register this module's own export table(s) immediately (not
     * deferred) so EVERY module - including ones loaded earlier in
     * the same front-loading pass - can resolve imports against it
     * once link_imports_one() runs for everyone. */
    for (int i = 0; i < out->export_count; i++) {
        export_registry_add(out->exports[i].name,
                             out->exports[i].addr + 20u /* fptrs[0] - see iop_elf.h layout */,
                             out->exports[i].fptr_count);
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

            if (exp && ord < exp->fptr_count) {
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

    g.modlist_index = 0;
    while (g.modlist_index < g.modlist_count && g.entry_points[g.modlist_index] == 0) g.modlist_index++;
    if (g.modlist_index >= g.modlist_count) return 0; /* not even one module in the list could be loaded */

    st->gpr[31] = g.trampoline_addr;
    st->gpr[29] = INITIAL_SP; /* $sp - see INITIAL_SP's comment above */
    st->gpr[4]  = g.boot_info_addr; /* $a0 - see BOOT_INFO_RAM_MB's comment above */
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
    st->pc = g.entry_points[g.modlist_index];
    st->next_pc = st->pc + 4;
    return 1;
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
        st->halted = 1;
        strncpy(st->halt_reason, panic_msg, sizeof(st->halt_reason) - 1);
        st->halt_reason[sizeof(st->halt_reason) - 1] = '\0';
        return 1;
    }

    if (!g.booted_ok || pc != g.trampoline_addr) return 0;

    g.stats.modules_run_to_completion++;
    if (advance_to_next_module(st)) return 1;

    /* Every module in the real BIOS's own IOPBTCONF/IOPBTCON2 list
     * has now been front-loaded (task #92, extended by the 31st
     * change) and had its real entry point executed to completion by
     * this project's actual IOP interpreter - a genuine milestone,
     * not a real hardware halt (no public reference describes what
     * real hardware's OWN loadcore loop does immediately after this
     * point, so this project stops here honestly rather than
     * guessing). */
    static char msg[128];
    snprintf(msg, sizeof(msg),
             "module boot sequence complete: %u/%u real modules loaded, %u run to completion (task #92)",
             (unsigned)g.stats.modules_loaded, (unsigned)g.modlist_count,
             (unsigned)g.stats.modules_run_to_completion);
    st->halted = 1;
    strncpy(st->halt_reason, msg, sizeof(st->halt_reason) - 1);
    st->halt_reason[sizeof(st->halt_reason) - 1] = '\0';
    return 1;
}
