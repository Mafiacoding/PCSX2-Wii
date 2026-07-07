/*
 * vu.c - see include/core/hw/vu.h for scope notes and references.
 */

#include "core/hw/vu.h"
#include <string.h>

static vu1_state_t g_vu1;

void vu1_init(void)
{
    memset(&g_vu1, 0, sizeof(g_vu1));
    /* VF00 is hardwired to (0,0,0,1.0f) on real hardware, matching
     * this project's existing VU0 macro-mode convention (ee_core.c's
     * vu0_vf_read_lane) - see docs/STATUS.md's "round 13" section for
     * the same fact cited there. Stored directly here (rather than
     * via a read-time special case like VU0's helpers) since VU1
     * micro mode has no other read path yet to route through. */
    g_vu1.vf[0][3] = 0x3F800000u; /* float bit pattern of 1.0f */
}

vu1_state_t *vu1_get_state(void) { return &g_vu1; }

static inline uint32_t vu_rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void vu_wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void vu1_micro_write32(uint32_t addr, uint32_t value)
{
    uint32_t off = addr & (VU1_MICRO_SIZE - 1u);
    vu_wr_le32(g_vu1.micro + off, value);
}

/* See vu.h's header comment for the full citation and scope. */
int vu_micro_step(uint32_t vf[32][4], uint32_t *vi,
                   uint8_t *mem, uint32_t mem_mask,
                   uint8_t *micro, uint32_t micro_mask,
                   uint32_t *tpc, uint32_t *branch_delay, uint32_t *branch_target,
                   uint32_t *ebit_delay,
                   uint64_t *instructions_executed, uint64_t *unimplemented_opcodes_seen)
{
    (void)vf; (void)mem; (void)mem_mask; (void)branch_target;

    uint32_t off = *tpc & micro_mask;
    uint32_t lower = vu_rd_le32(micro + off);       /* ptr[0] */
    uint32_t upper = vu_rd_le32(micro + off + 4u);  /* ptr[1] */

    *tpc = (off + 8u) & micro_mask;

    /* E flag (bit 30 of the upper word) - real hardware executes
     * exactly one more instruction after this one before actually
     * stopping (the classic VU "E-bit delay slot") - see header
     * comment for the exact countdown arithmetic this mirrors. */
    if (upper & 0x40000000u)
        *ebit_delay = 2u;

    /* I flag (bit 31 of the upper word): only the upper instruction
     * executes this pair; the lower word's raw bits become the real
     * $I$ register (VI[21], REG_I per PCSX2's VU.h VURegFlags enum). */
    if (upper & 0x80000000u) {
        vi[21] = lower;
    }

    /* No real per-opcode decode table exists for this project (see
     * header comment) - every instruction pair's actual FMAC/integer/
     * branch body is a logged no-op. Real control flow (TPC advance,
     * E-bit/I-bit handling above, branch delay-slot mechanism below)
     * is unaffected by this - it's genuinely correct regardless of
     * what the undecoded opcode "really" does. */
    (*unimplemented_opcodes_seen)++;
    (*instructions_executed)++;

    /* Branch delay-slot mechanism (same 1-instruction-delay pattern
     * as the E-bit, per PCSX2's `_vu0Exec`) - nothing sets
     * *branch_delay > 0 yet (no branch opcode is decoded this round),
     * but the mechanism itself is implemented correctly so a future
     * round's branch decode can just set branch_delay/branch_target
     * and this keeps working unchanged. */
    if (*branch_delay > 0) {
        if (--(*branch_delay) == 0)
            *tpc = *branch_target;
    }

    if (*ebit_delay > 0) {
        if (--(*ebit_delay) == 0)
            return 1; /* stopped */
    }
    return 0;
}

/* Safety cap on a single MSCAL/MSCNT run - this project's own guard
 * against a genuinely-infinite microprogram (e.g. malformed/garbage
 * micro memory with no E-bit ever set), not a real hardware behavior. */
#define VU_EXEC_STEP_CAP 65536u

void vu1_exec_micro(uint32_t start_addr)
{
    g_vu1.tpc = (start_addr << 3) & (VU1_MICRO_SIZE - 1u);
    g_vu1.running = 1;

    for (uint32_t i = 0; i < VU_EXEC_STEP_CAP; i++) {
        int stopped = vu_micro_step(g_vu1.vf, g_vu1.vi,
                                     g_vu1.mem, VU1_MEM_SIZE - 1u,
                                     g_vu1.micro, VU1_MICRO_SIZE - 1u,
                                     &g_vu1.tpc, &g_vu1.branch_delay, &g_vu1.branch_target,
                                     &g_vu1.ebit_delay,
                                     &g_vu1.instructions_executed, &g_vu1.unimplemented_opcodes_seen);
        if (stopped)
            break;
    }

    g_vu1.running = 0;
}
