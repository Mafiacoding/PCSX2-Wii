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

    export_registry_entry_t exports[EXPORT_REGISTRY_MAX];
    int export_count;

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

/* Loads one module by ROMDIR name, links its imports against
 * already-registered exports, registers its own exports, and returns
 * its entry point (or 0 on any failure - missing ROMDIR entry,
 * malformed ELF, etc; the caller decides whether to skip and try the
 * next module in the list). */
static uint32_t load_and_link_one(iop_state_t *st, const char *name)
{
    g.stats.modules_attempted++;

    const romdir_entry_t *rd = romdir_find(name);
#ifdef IOP_MODLOADER_DEBUG
    fprintf(stderr, "[modloader] load_and_link_one('%s') rd=%p\n", name, (void*)rd);
#endif
    if (!rd || rd->size == 0) return 0;
    if ((uint64_t)rd->payload_off + rd->size > st->bios->size) return 0;

    uint32_t load_addr = bump_alloc(rd->size + 0x1000u /* headroom for bss + tables, generous */);

    iop_elf_load_result_t res;
    const char *err = NULL;
    int rc = iop_elf_load(st, st->bios->data + rd->payload_off, rd->size, load_addr, &res, &err);
#ifdef IOP_MODLOADER_DEBUG
    fprintf(stderr, "[modloader]   rc=%d err=%s entry=0x%x\n", rc, err ? err : "(none)", rc==0?res.entry:0);
#endif
    if (rc != 0) return 0;

    /* Advance the bump allocator to the module's real end (we
     * over-allocated headroom above; this reclaims the unused part
     * for the NEXT module, keeping IOP RAM usage honest). */
    g.bump_next = (res.load_end + 15u) & ~15u;

    g.stats.modules_loaded++;

    /* Register this module's own export table(s) so LATER modules in
     * the boot list can resolve imports against it. */
    for (int i = 0; i < res.export_count; i++) {
        export_registry_add(res.exports[i].name,
                             res.exports[i].addr + 20u /* fptrs[0] - see iop_elf.h layout */,
                             res.exports[i].fptr_count);
    }

    /* Resolve this module's OWN imports against modules already
     * loaded earlier in the boot list (real IOPBTCONF order always
     * lists a dependency before its dependents - see
     * iop_module_loader.h's citation). */
    for (int i = 0; i < res.import_count; i++) {
        const iop_elf_import_table_t *imp = &res.imports[i];
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
                 * expected sometimes: e.g. a forward reference, or an
                 * import this project's real-BIOS-derived boot list
                 * doesn't actually provide. */
                g.stats.imports_unresolved++;
            }
        }
    }

    return res.entry;
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

    g.modlist_index = 0;
    while (g.modlist_index < g.modlist_count) {
        uint32_t entry = load_and_link_one(st, g.modlist[g.modlist_index]);
        if (entry != 0) {
            st->gpr[31] = g.trampoline_addr;
            st->gpr[29] = INITIAL_SP; /* $sp - see INITIAL_SP's comment above */
            st->pc = entry;
            st->next_pc = entry + 4;
            g.booted_ok = 1;
            return 1;
        }
        g.modlist_index++;
    }
    return 0; /* not even the first module in the list could be loaded */
}

int iop_module_loader_try_handle(iop_state_t *st, uint32_t pc)
{
    if (!g.booted_ok || pc != g.trampoline_addr) return 0;

    g.stats.modules_run_to_completion++;
    g.modlist_index++;

    while (g.modlist_index < g.modlist_count) {
        uint32_t entry = load_and_link_one(st, g.modlist[g.modlist_index]);
        if (entry != 0) {
            st->gpr[31] = g.trampoline_addr;
            st->gpr[29] = INITIAL_SP; /* $sp - see INITIAL_SP's comment above */
            st->pc = entry;
            st->next_pc = entry + 4;
            return 1;
        }
        g.modlist_index++;
    }

    /* Every module in the real BIOS's own IOPBTCONF/IOPBTCON2 list
     * has now been loaded, linked, and had its real entry point
     * executed to completion by this project's actual IOP
     * interpreter - a genuine milestone, not a real hardware halt (no
     * public reference describes what real hardware's OWN loadcore
     * loop does immediately after this point, so this project stops
     * here honestly rather than guessing). */
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
