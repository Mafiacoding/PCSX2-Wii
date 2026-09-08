/*
 * Round 855 (task #855, user's "1 dann 2 dann 3" step 3): standalone
 * repro for a suspected real bug in reschedule()'s "nothing ready"
 * fallback (ee_hle_thread.c ~line 384-392): when the ONLY thread
 * self-blocks via SleepThread() and reschedule() finds nothing else
 * ready, does the live register file (st) actually stop advancing -
 * or does it keep fetching/decoding/executing the "sleeping" thread's
 * own subsequent code, because nothing ever swapped st's pc away from
 * where SleepThread's own EE_ADVANCE() left it?
 *
 * If it keeps executing, this is a real correctness bug distinct from
 * (but same root-cause family as) the already-shipped Round 630/782
 * null-jalr guard and the already-shipped WaitSema/Round-303/781
 * "tick while parked" fix - WaitSema pins pc at the syscall itself
 * (park-by-not-advancing), so it can never accidentally run past
 * itself. SleepThread does NOT have that protection.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/ee/ee_core.c"

static uint32_t enc_addiu(int rt, int rs, int16_t imm) { return (0x09 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_syscall(void) { return (0x0Cu); }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, 50));   pc += 4; /* $v1 = 50 (SleepThread) */
    wle32(p+pc, enc_syscall());         pc += 4; /* SYSCALL */
    wle32(p+pc, 0x0u);                  pc += 4; /* delay slot: NOP */
    /* MARKER instruction right after the syscall's delay slot: if the
     * "sleeping" thread's own code keeps executing, this ADDIU will
     * fire and $s0 (gpr 16) will become 0x1234. A genuinely-idle CPU
     * (real hardware, or a correctly-fixed emulator) must NEVER
     * execute this while the only thread is WAIT/SLEEP. */
    wle32(p+pc, enc_addiu(16, 0, 0x1234)); pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    /* Real hardware clears Status.ERL (and BEV) very early in the
     * real boot ROM sequence via an ERET/explicit MTC0, long before
     * any game/kernel thread code runs - this synthetic repro skips
     * that real boot preamble entirely and plants raw instructions
     * straight at the reset vector, so it must clear ERL itself here
     * to reach the same steady state real thread code always runs
     * under (reschedule()'s own EXL/ERL guard - Round 812 - would
     * otherwise mask the exact "next==0" path this repro targets). */
    st->cop0[12] &= ~0x00000004u; /* clear ERL (bit 2) */

    ee_core_step(); /* ADDIU $v1, 50 */
    ee_core_step(); /* SYSCALL -> SleepThread() self-block, nothing else ready */

    printf("[R855] after SleepThread self-block: pc=0x%08x gpr[16]=0x%08x tid=%d status=0x%x\n",
           st->pc, (uint32_t)st->gpr[16].ud0, ee_hle_thread_get_current_thread_id(),
           ee_hle_thread_get_status(ee_hle_thread_get_current_thread_id()));
    printf("[R855] st->idle=%d\n", st->idle);
    printf("[R855] st->cop0[12] (Status)=0x%08x\n", st->cop0[12]);

    /* Step several more times - a correctly-idle core should never
     * advance pc past the marker or execute it. */
    for (int i = 0; i < 5; i++) {
        ee_core_step();
        printf("[R855] step %d: pc=0x%08x gpr[16]=0x%08x halted=%d\n",
               i, st->pc, (uint32_t)st->gpr[16].ud0, st->halted);
    }

    if ((uint32_t)st->gpr[16].ud0 == 0x1234u) {
        printf("[R855] BUG CONFIRMED: the sleeping thread's own marker instruction executed (gpr[16]=0x1234) despite being the ONLY thread and marked WAIT/SLEEP.\n");
        return 0;
    }
    printf("[R855] no bug observed: marker never executed, gpr[16] stayed 0x%08x (idle=%d).\n", (uint32_t)st->gpr[16].ud0, st->idle);

    /* Round 855 resumption check: a real wakeup must still let the
     * thread continue normally afterward - idle must not become a
     * permanent, un-resumable dead end. */
    ee_hle_thread_debug_force_wakeup(1);
    printf("[R855] after force_wakeup(1): status=0x%x\n", ee_hle_thread_get_status(1));
    for (int i = 0; i < 3; i++) {
        ee_core_step();
        printf("[R855] resume-step %d: pc=0x%08x gpr[16]=0x%08x idle=%d\n", i, st->pc, (uint32_t)st->gpr[16].ud0, st->idle);
    }
    if ((uint32_t)st->gpr[16].ud0 == 0x1234u) {
        printf("[R855] RESUMPTION OK: thread correctly continued and executed its own marker instruction after being woken.\n");
    } else {
        printf("[R855] RESUMPTION FAILED: thread never continued after being woken - idle is a dead end, this would be a NEW bug.\n");
    }
    return 0;
}
