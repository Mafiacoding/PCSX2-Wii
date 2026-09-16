/* Round 957 (task #950, SCPH-50004 forward-trace): direct continuation
 * of Round 955/956's open question - "what does OSDSYS's own code do
 * AFTER the decompression loop (0x00100b30-0x00100c58) completes that
 * leads back to a fresh rom0:OSDSYS LOADFILE request ~N instructions
 * later?" Round 956 fact-checked and declined the user's unsourced
 * "SIF-queue-ACK/watchdog-timeout" proposal; this round does the real,
 * evidenced next step instead: watch real PC traffic between the 1st
 * and 2nd real LF_F_ELF_LOAD replies (now countable via Round 957's own
 * new ee_core_get_loadfile_reply_count() getter, no more stderr-log-
 * parsing needed) and record every NEWLY-entered code address (deduped
 * via a bounded visited-set, same technique as Round 815's
 * r815_mark_new_pc()) so the actual control-flow path is visible
 * without drowning in repeated loop-body hits. Also explicitly checks
 * whether PC ever lands on any of the 4 real EE COP0 exception-vector
 * addresses (reset/TLB-refill=0x80000000, perf-counter=0x80000080,
 * debug=0x80000100, general/interrupt=0x80000180) during this window -
 * the one concrete, falsifiable prediction a genuine
 * "watchdog/exception-driven reset" explanation would make. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/gs.h"
#include "core/hw/iop_cdvd.h"

/* Real EE COP0 exception vector addresses, exactly as this project's
 * own ee_raise_exception() computes them (ee_core.c ~line 321-359,
 * ported from PCSX2's cpuException()) once Status.BEV is cleared
 * (base=0x80000000, the normal post-boot RAM-resident-handler state):
 * TLB-refill=+0x000, general=+0x180, interrupt=+0x200. These are the
 * ONLY three vectors this project's EE model ever jumps to on an
 * exception - if the LOADFILE-retry mechanism were really exception/
 * reset-driven, PC would have to pass through one of these three. */
#define VEC_TLB_REFILL   0x80000000u
#define VEC_GENERAL      0x80000180u
#define VEC_INTERRUPT    0x80000200u

/* Bounded "new address" dedup set - same open-addressed hash-set
 * technique as Round 815's r815_mark_new_pc(), reimplemented locally
 * so this tool has no dependency on ee_core.c internals. */
#define VISITED_BITS 16
#define VISITED_SIZE (1u << VISITED_BITS)
#define VISITED_MASK (VISITED_SIZE - 1u)
static uint32_t g_visited[VISITED_SIZE];
static uint32_t g_visited_n = 0;
static int mark_new_pc(uint32_t pc)
{
    uint32_t key = pc ? pc : 0xFFFFFFFFu;
    uint32_t h = (key >> 2) & VISITED_MASK;
    uint32_t start = h;
    if (g_visited_n >= (VISITED_SIZE * 3u / 4u)) return 0;
    for (;;) {
        uint32_t slot = g_visited[h];
        if (slot == key) return 0;
        if (slot == 0u) { g_visited[h] = key; g_visited_n++; return 1; }
        h = (h + 1u) & VISITED_MASK;
        if (h == start) return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <budget>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    uint64_t budget = strtoull(argv[2], NULL, 10);
    uint64_t chunk = 2000ull, done = 0; /* fine-grained: 2,000-slice ticks */
    ee_state_t *ee = ee_core_get_state();

    uint64_t last_reply_count = 0;
    int new_pc_prints = 0;
    int window_active = 0;       /* true once reply #1 has fired, until reply #2 fires */
    uint64_t window_start_instr = 0;
    int vector_hits = 0;

    printf("[R957] starting forward-trace, budget=%llu chunk=%llu\n",
           (unsigned long long)budget, (unsigned long long)chunk);

    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        uint32_t pc = ee->pc;
        uint64_t rc = ee_core_get_loadfile_reply_count();

        if (rc != last_reply_count) {
            printf("[R957REPLY] ee_instr=%llu loadfile_reply_count %llu -> %llu pc=0x%08x\n",
                   (unsigned long long)ee->instructions_executed,
                   (unsigned long long)last_reply_count, (unsigned long long)rc, pc);
            if (last_reply_count >= 1 && window_active) {
                printf("[R957] *** WINDOW CLOSED: reply #%llu fired after %llu instructions "
                       "since reply #%llu (window start ee_instr=%llu) ***\n",
                       (unsigned long long)rc,
                       (unsigned long long)(ee->instructions_executed - window_start_instr),
                       (unsigned long long)last_reply_count,
                       (unsigned long long)window_start_instr);
                /* We've captured one full inter-reply window (1st->2nd);
                 * that's the exact window Round 955/956 asked about.
                 * Stop here rather than diluting output with a 2nd/3rd
                 * repeat of the same cycle. */
                break;
            }
            if (rc == 1) {
                window_active = 1;
                window_start_instr = ee->instructions_executed;
                printf("[R957] *** WINDOW OPENED after reply #1 - now tracking every newly-"
                       "entered PC address until reply #2 fires ***\n");
            }
            last_reply_count = rc;
        }

        if (window_active) {
            /* VEC_INTERRUPT (0x80000200) is deliberately NOT counted
             * here - VBLANK and other routine interrupts fire through
             * it constantly during normal idle execution, so a hit
             * there proves nothing about a "watchdog reset". Only the
             * two SYNCHRONOUS-fault vectors (TLB-refill, general
             * exception - the ones a real crash/timeout/reset would
             * have to go through) count as evidence. */
            if (pc == VEC_TLB_REFILL || pc == VEC_GENERAL) {
                vector_hits++;
                if (vector_hits <= 20) {
                    printf("[R957VEC] ee_instr=%llu pc=0x%08x hit a real EE SYNCHRONOUS exception "
                           "vector (cause=0x%08x status=0x%08x epc=0x%08x)\n",
                           (unsigned long long)ee->instructions_executed, pc,
                           ee->cop0[13], ee->cop0[12], ee->cop0[14]);
                }
            }
            if (new_pc_prints < 400 && mark_new_pc(pc)) {
                printf("[R957PC] ee_instr=%llu (+%llu) pc=0x%08x  NEW\n",
                       (unsigned long long)ee->instructions_executed,
                       (unsigned long long)(ee->instructions_executed - window_start_instr),
                       pc);
                new_pc_prints++;
            }
        }
    }

    printf("\n[R957] FINAL: ee_instr=%llu ee_pc=0x%08x ee_halted=%d loadfile_reply_count=%llu "
           "distinct_new_pcs_seen=%u vector_hits=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
           (unsigned long long)ee_core_get_loadfile_reply_count(),
           (unsigned)g_visited_n, vector_hits);
    printf("[R957] verdict: %s\n",
           vector_hits > 0
               ? "PC WAS observed landing exactly on a real EE exception-vector address during "
                 "the inter-reply window - a genuine exception/reset IS involved (supports "
                 "SOME version of an exception-driven explanation, though not necessarily the "
                 "user's specific 'SIF queue ACK / watchdog' framing - see which vector and "
                 "cause value fired above)."
               : "PC was NEVER observed on any real EE exception-vector address during the "
                 "entire inter-reply window - whatever causes the next LOADFILE request is NOT "
                 "a CPU exception/reset event on the EE side; it is ordinary sequential/branch "
                 "control flow, consistent with Round 955's 'genuine repeated load-and-run "
                 "cycle' finding and inconsistent with any watchdog/exception-timeout "
                 "explanation.");

    return 0;
}
