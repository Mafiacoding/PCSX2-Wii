/* Round 964 (task #887/937/938, user-directed follow-up: user supplied
 * a hypothesis, "RegisterVblankHandler is the missing IOP mechanism").
 * FACT-CHECK + DIRECT VERIFICATION, per this project's standing anti-
 * fabrication rule (any fix must be backed by real disassembly or a
 * real citable source, never guessed).
 *
 * The user's specific claims (syscall number ~20, lives in INTRMAN,
 * generic pseudocode) do NOT match reality, but the user's core idea
 * - "there's a second registration API separate from
 * RegisterIntrHandler, and it's currently unaccounted for" - is
 * CONFIRMED CORRECT by a real, dated, user-uploaded source this round
 * (uploads/vblank.c, by "[RO]man", the exact same reverse-engineering
 * author this project already trusts for the Round 259 EECONF.C
 * citation). That source is the real "vblank"/"Vblank_service" IOP
 * module (loaded @ 0x00011F00-0x00012900 per its own header comment -
 * exactly containing Round 962/963's 0x00012274/0x0001232c addresses)
 * and shows:
 *   - export ordinal 8 = RegisterVblankHandler(int number, int
 *     priority, int(*func)(struct VBHS*), struct VBHS *this)
 *   - export ordinal 9 = ReleaseVblankHandler(int number, int(*func)())
 *   - its own start() calls RegisterIntrHandler(0,1,intrh_vblank,&v)/
 *     RegisterIntrHandler(11,1,intrh_evblank,&v) ONCE, installing
 *     itself as the sole raw ISR - EXACTLY matching Round 962's
 *     empirical finding (irq0/irq11 handler = 0x00012274/0x0001232c)
 *   - intrh_vblank/intrh_evblank walk v.list0/v.list11 (a struct VBHS
 *     with statusFlag@0, count@4, list0@8, list11@16, free@24,
 *     items[16]@32) calling node->function(node->this) -
 *     node->function is at DCLL offset 12, node->this at offset 16 -
 *     EXACTLY matching Round 963's disassembly-derived offsets
 *     (node[+12]=jalr target, node[+16]=a0 arg). This is a byte-exact,
 *     independent confirmation of Round 963's finding, using a real
 *     citable source instead of just raw disassembly.
 *
 * This tool answers the decisive remaining question: is v.list0/
 * v.list11 EMPTY (sentinel points to itself) or does it contain real
 * chained entries, on a genuine SCPH-50004+GT3 fresh cold boot? Method:
 * single-step IOP execution (same technique as r961_iop_finegrain.c)
 * watching for pc==0x00012274 (intrh_vblank) or pc==0x0001232c
 * (intrh_evblank); on first hit, capture $a0 (=the real `v` struct
 * pointer, per vblank.c's own RegisterIntrHandler(...,&v) call), then
 * read IOP RAM at v+8/v+16 (list0.next/list11.next) directly - if
 * either differs from the sentinel address itself (v+8 or v+16), a
 * real handler is genuinely chained in; if equal, the list is
 * genuinely empty and nothing has called RegisterVblankHandler yet.
 * Read-only, no tracked-source changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios> <disc> [budget_slices]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = (argc >= 4) ? strtoull(argv[3], NULL, 10) : 150000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    uint32_t v_addr_from_vblank = 0, v_addr_from_evblank = 0;
    uint64_t hit0_at_ee_instr = 0, hit11_at_ee_instr = 0;
    int seen0 = 0, seen11 = 0;

    /* Coarse phase: run in interleaved chunks until first registration
     * (per Round 962, both handlers appear by ee_instr~80M), then
     * switch to fine single-stepping only near likely dispatch time
     * to actually catch pc==handler with a bounded step budget. */
    uint64_t done = 0;
    const uint64_t CHUNK = 10000000ull;
    while (done < budget && !ee->halted && !iop->halted) {
        system_run_interleaved(CHUNK);
        done += CHUNK;
        if (iop_hle_intr_get_intr_handler(0) != 0 && iop_hle_intr_get_intr_handler(11) != 0) {
            fprintf(stderr, "[R964] both irq0/irq11 handlers registered by slices=%llu ee_instr=%llu\n",
                    (unsigned long long)done, (unsigned long long)ee->instructions_executed);
            break;
        }
    }

    /* Fine phase: single-step until we directly observe pc hit the
     * real intrh_vblank/intrh_evblank entry, capturing $a0 (=&v) at
     * that exact moment - this is ground truth, not inferred. */
    uint64_t fine_budget = 20000000ull;
    for (uint64_t i = 0; i < fine_budget && !iop->halted; i++) {
        if (!seen0 && iop->pc == 0x00012274u) {
            v_addr_from_vblank = iop->gpr[4]; /* $a0 */
            hit0_at_ee_instr = ee->instructions_executed;
            seen0 = 1;
            fprintf(stderr, "[R964] pc==0x00012274 (intrh_vblank) hit at step=%llu ee_instr=%llu a0(&v)=0x%08x\n",
                    (unsigned long long)i, (unsigned long long)hit0_at_ee_instr, v_addr_from_vblank);
        }
        if (!seen11 && iop->pc == 0x0001232cu) {
            v_addr_from_evblank = iop->gpr[4];
            hit11_at_ee_instr = ee->instructions_executed;
            seen11 = 1;
            fprintf(stderr, "[R964] pc==0x0001232c (intrh_evblank) hit at step=%llu ee_instr=%llu a0(&v)=0x%08x\n",
                    (unsigned long long)i, (unsigned long long)hit11_at_ee_instr, v_addr_from_evblank);
        }
        if (seen0 && seen11) break;
        iop_core_step();
    }

    if (!seen0 && !seen11) {
        fprintf(stderr, "[R964] NEITHER handler pc was hit during the %llu-step fine window - "
                "dispatch may occur less often than this window covers, or IOP was idle "
                "the whole time. Not conclusive; would need a larger fine_budget.\n",
                (unsigned long long)fine_budget);
        return 0;
    }

    uint32_t v_addr = seen0 ? v_addr_from_vblank : v_addr_from_evblank;
    if (seen0 && seen11 && v_addr_from_vblank != v_addr_from_evblank) {
        fprintf(stderr, "[R964] WARNING: &v differs between the two calls (0x%08x vs 0x%08x) - "
                "unexpected, both should share the same static struct per vblank.c\n",
                v_addr_from_vblank, v_addr_from_evblank);
    }

    fprintf(stderr, "\n[R964] real VBHS struct address (from live $a0) = 0x%08x\n", v_addr);
    uint32_t statusFlag = iop_mem_read32(iop, v_addr + 0);
    uint32_t count      = iop_mem_read32(iop, v_addr + 4);
    uint32_t list0_next = iop_mem_read32(iop, v_addr + 8);
    uint32_t list0_prev = iop_mem_read32(iop, v_addr + 12);
    uint32_t list11_next = iop_mem_read32(iop, v_addr + 16);
    uint32_t list11_prev = iop_mem_read32(iop, v_addr + 20);
    uint32_t free_next = iop_mem_read32(iop, v_addr + 24);

    fprintf(stderr, "[R964] v.statusFlag=0x%08x v.count=%u\n", statusFlag, count);
    fprintf(stderr, "[R964] v.list0:  next=0x%08x prev=0x%08x  (sentinel self-addr=0x%08x) -> %s\n",
            list0_next, list0_prev, v_addr + 8,
            (list0_next == v_addr + 8) ? "EMPTY (no real handler chained)" : "NON-EMPTY (real handler(s) chained!)");
    fprintf(stderr, "[R964] v.list11: next=0x%08x prev=0x%08x  (sentinel self-addr=0x%08x) -> %s\n",
            list11_next, list11_prev, v_addr + 16,
            (list11_next == v_addr + 16) ? "EMPTY (no real handler chained)" : "NON-EMPTY (real handler(s) chained!)");
    fprintf(stderr, "[R964] v.free.next=0x%08x (sentinel self-addr=0x%08x) -> %u of 16 pool slots used\n",
            free_next, v_addr + 24, (free_next == v_addr + 24) ? 16u : 0u /* rough: just checks fully-free vs not */);

    /* If list0 is non-empty, walk it and print each node's function/this
     * pointer (real driver callback identity). */
    if (list0_next != v_addr + 8) {
        fprintf(stderr, "[R964] list0 chain contents:\n");
        uint32_t p = list0_next;
        int guard = 0;
        while (p != v_addr + 8 && guard++ < 16) {
            uint32_t func = iop_mem_read32(iop, p + 12);
            uint32_t thisp = iop_mem_read32(iop, p + 16);
            uint32_t prio = iop_mem_read32(iop, p + 8);
            fprintf(stderr, "[R964]   node=0x%08x priority=%u function=0x%08x this=0x%08x\n",
                    p, prio, func, thisp);
            p = iop_mem_read32(iop, p + 0);
        }
    }
    if (list11_next != v_addr + 16) {
        fprintf(stderr, "[R964] list11 chain contents:\n");
        uint32_t p = list11_next;
        int guard = 0;
        while (p != v_addr + 16 && guard++ < 16) {
            uint32_t func = iop_mem_read32(iop, p + 12);
            uint32_t thisp = iop_mem_read32(iop, p + 16);
            uint32_t prio = iop_mem_read32(iop, p + 8);
            fprintf(stderr, "[R964]   node=0x%08x priority=%u function=0x%08x this=0x%08x\n",
                    p, prio, func, thisp);
            p = iop_mem_read32(iop, p + 0);
        }
    }
    return 0;
}
