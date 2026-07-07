#ifndef PCSX2WII_VU_H
#define PCSX2WII_VU_H

#include <stdint.h>

/*
 * vu.h - VU0/VU1 "micro mode" data/instruction memory + microcode
 * interpreter (task: "VU0/VU1 data memory + VU microcode
 * interpreter").
 *
 * IMPORTANT - how this relates to the EXISTING VU0 code in ee_core.c/
 * ee_core.h (round 13, "VU0 vector datapath"): that work is VU0
 * "macro mode" - VU0 acting as the EE's COP2 coprocessor, executing
 * ONE vector instruction per EE instruction issued via MFC2/CFC2/
 * MTC2/CTC2/QMFC2/QMTC2/VSUB/VISWR/VSQI/etc, using the EE's own MIPS-
 * style 32-bit instruction encoding (rs/rt/rd/funct fields). This
 * file is a COMPLETELY SEPARATE thing: VU0/VU1 "micro mode" - an
 * asynchronous microprogram (uploaded via VIF's MPG command, kicked
 * by MSCAL/MSCNT/MSCALF) that runs on its own, using a totally
 * different, VU-native 64-bit-per-instruction ISA (NOT MIPS at all -
 * every micro-mode instruction is a pair of 32-bit words, "lower" and
 * "upper", each with their own opcode field and register layout).
 * Real hardware has both running on the SAME VU0 (sharing its VF/VI
 * register file and local data memory) - this project reuses the
 * EXISTING vu0_vf/cop2_ctrl/vu0_mem fields in ee_state_t for VU0
 * micro mode's register/data-memory access for exactly that reason
 * (see ee_core.c's vu0_exec_micro/vu0_micro_write32). VU1 has no
 * macro-mode/COP2 presence at all on real hardware (it's not attached
 * to the EE directly, only reachable via VIF1/MSCAL and the GIF/
 * XGKICK path) - so VU1 gets its own fully self-contained state,
 * `vu1_state_t` below.
 *
 * Real, cited structure (live-fetched PCSX2 source this round):
 *   - `pcsx2/VUmicro.h`: VU0_MEMSIZE/VU0_PROGSIZE = 0x1000 (4KB each),
 *     VU1_MEMSIZE/VU1_PROGSIZE = 0x4000 (16KB each) - real, documented
 *     PS2 VU sizes (also cross-checked against PS2Tek).
 *   - `pcsx2/VU0microInterp.cpp` (`_vu0Exec`): each micro-instruction
 *     is 8 bytes - `ptr[0]` = LOWER word, `ptr[1]` = UPPER word, TPC
 *     advances by 8 per instruction. UPPER word flag bits: bit 31
 *     (0x80000000) = I flag (only the upper instruction executes this
 *     pair; the LOWER word's raw 32 bits become the real "$I$"
 *     register, VI[21] per `VU.h`'s `REG_I` - used by FMAC
 *     broadcast-immediate instructions), bit 30 (0x40000000) = E flag
 *     (end of microprogram - real hardware executes exactly ONE MORE
 *     instruction after this one, the classic VU "E-bit delay slot",
 *     before actually stopping - verified from the exact ebit
 *     countdown arithmetic in `_vu0Exec`). Real hardware also has M/D/
 *     T flags (bits 29/28/27) gating INTC/FBRST-based debug
 *     interrupts - NOT implemented here (no VU-side interrupt
 *     delivery exists in this project yet, same "real bit position
 *     cited, side effect not modeled" pattern as round 12's FBRST/
 *     CTC2 handling). Branches use the same 1-instruction-delay-slot
 *     mechanism as the E-bit (`VU->branch`countdown in `_vu0Exec`).
 *
 * WHAT IS NOT IMPLEMENTED, and why (read before extending): the real
 * per-opcode-number-to-mnemonic mapping (PCSX2's
 * `VU0_LOWER_OPCODE[128]`/`VU0_UPPER_OPCODE[64]` function-pointer
 * tables - a 7-bit lower opcode field `code>>25` and a 6-bit upper
 * opcode field `code&0x3f`) is defined in a PCSX2 source file this
 * project could not locate this round despite fetching `VU.h`,
 * `VUmicro.h`, `VUmicro.cpp`, `VUops.h`, `VUops.cpp` (which implements
 * each instruction's BODY as a `_vuADDx`/`_vuNOP`/etc-style function,
 * but not the index-to-function table itself), `VUmicroMem.cpp`, and
 * `VU1micro.cpp`. Per this project's no-fabrication policy, this file
 * does NOT guess which numeric opcode value corresponds to which real
 * instruction. What IS implemented is everything structural that
 * doesn't depend on that table: real memory sizes, real TPC/branch/
 * E-bit/I-bit control flow (byte-exact against the cited source
 * above), and MPG actually writing real microprogram bytes into real
 * micro-instruction memory (previously these bytes went nowhere - see
 * vif.h's own scope note). Every fetched instruction pair is stepped
 * over correctly (control flow keeps working, TPC advances by 8,
 * E-bit termination fires at the right time) but its actual FMAC/
 * integer-ALU/branch BODY is a logged no-op (`unimplemented_opcodes_seen`)
 * rather than a guessed one. This turns MSCAL/MSCNT/MSCALF from total
 * no-ops (vif.c's prior state) into a genuine fetch-execute-until-E-bit
 * loop over the real uploaded microprogram - a real, narrower, honest
 * step forward, not a full VU implementation.
 */

#define VU1_MEM_SIZE   0x4000u /* 16KB - PCSX2's VU1_MEMSIZE */
#define VU1_MICRO_SIZE 0x4000u /* 16KB - PCSX2's VU1_PROGSIZE */

typedef struct {
    uint32_t vf[32][4]; /* VF0-31, 4 lanes (raw float bit patterns) each */
    uint32_t vi[32];    /* only 0-15 are real VU1 integer regs; the rest
                         * mirror this project's VU0 cop2_ctrl[32]
                         * convention of reserving indices per PCSX2's
                         * VURegFlags enum (VU.h) - e.g. index 21 =
                         * REG_I, used by the I-flag case below - even
                         * though VU1 has no EE-COP2 macro-mode path to
                         * set most of them. */
    uint8_t  mem[VU1_MEM_SIZE];
    uint8_t  micro[VU1_MICRO_SIZE];

    uint32_t tpc;            /* byte offset into micro[] of the next instruction pair */
    uint32_t branch_delay;   /* 0 = none pending - see header comment */
    uint32_t branch_target;
    uint32_t ebit_delay;     /* 0 = not stopping; see header comment */
    uint8_t  running;

    uint64_t instructions_executed;
    uint64_t unimplemented_opcodes_seen;
} vu1_state_t;

void vu1_init(void);
vu1_state_t *vu1_get_state(void);

/* Called from vif.c's VIF_CMD_MPG handling: writes one little-endian
 * 32-bit micro-instruction-memory word at byte offset `addr` into
 * VU1's micro[] (real hardware wraps via the VU1_PROGSIZE-1 mask). */
void vu1_micro_write32(uint32_t addr, uint32_t value);

/* Called from vif.c's MSCAL/MSCNT/MSCALF handling: runs VU1's
 * microprogram starting at instruction-pair index `start_addr` (the
 * real MSCAL/MSCNT IMM field value - byte offset = start_addr*8, per
 * a live fetch of PCSX2's `vu1ExecMicro()`: `VU1.VI[REG_TPC].UL = addr;
 * ...SetStartPC(TPC << 3)`) until a real E-bit-flagged instruction has
 * fully retired (with the real one-more-instruction delay - see the
 * header comment), or a safety instruction cap is hit (this project's
 * own guard against a genuinely-infinite microprogram, not a real
 * hardware behavior). */
void vu1_exec_micro(uint32_t start_addr);

/* Shared step function - executes exactly one 8-byte VU instruction
 * pair and advances *tpc by 8 (masked to micro_mask). Used by both
 * vu1_exec_micro() (fed vu1_state_t's own fields) and ee_core.c's
 * vu0_exec_micro() (fed ee_state_t's vu0_vf/cop2_ctrl/vu0_mem/
 * vu0_micro fields directly - see ee_core.h) so the real control-flow
 * logic is written and tested exactly once. Returns 1 if the
 * microprogram has now fully stopped (E-bit fired), 0 if it should
 * keep running. `vi` must point to an array of at least 32
 * uint32_t (only index 21, REG_I, is ever written by this function -
 * see the header comment's I-flag description). */
int vu_micro_step(uint32_t vf[32][4], uint32_t *vi,
                   uint8_t *mem, uint32_t mem_mask,
                   uint8_t *micro, uint32_t micro_mask,
                   uint32_t *tpc, uint32_t *branch_delay, uint32_t *branch_target,
                   uint32_t *ebit_delay,
                   uint64_t *instructions_executed, uint64_t *unimplemented_opcodes_seen);

#endif
