/* ee_dve.c - see include/core/hw/ee_dve.h for the full citation and
 * scope trail (Round 950, task #447/#536/#887). Faithful port of real
 * PCSX2's pcsx2/Memory.cpp ba0R16()/ba0W16()/memReset() DVE stub. */
#include "core/hw/ee_dve.h"
#include <string.h>

static uint16_t s_ba[0x100];
static uint16_t s_dve_regs[0x100];
static int s_ba_command_executing;
static int s_ba_error_detected;
static uint16_t s_ba_current_reg;

void ee_dve_init(void)
{
    memset(s_ba, 0, sizeof(s_ba));
    s_ba[0xA] = 1; /* Power on - real PCSX2 memReset() default */
    s_ba_command_executing = 0;
    s_ba_error_detected = 0;
    s_ba_current_reg = 0;

    memset(s_dve_regs, 0, sizeof(s_dve_regs));
    s_dve_regs[0x7e] = 0x1C; /* "status OK" - real PCSX2 memReset() default */
}

int ee_dve_mmio_read16(uint32_t addr, uint16_t *out)
{
    if (addr < EE_DVE_BASE || addr >= EE_DVE_BASE + EE_DVE_SIZE)
        return 0;

    if (addr == 0x1A000006u) {
        /* Real PCSX2 ba0R16(): bit0 = error, bit1 = ready. The
         * ready bit becomes set only after the 3rd poll following a
         * Start-Execute write, then command_executing latches off -
         * this is what our observed 0x80007CD8 loop is waiting on. */
        uint16_t return_val = (uint16_t)(s_ba[0x6] & 2u);
        if (s_ba_error_detected)
            return_val |= 1u;

        if (s_ba[0x6] < 3 && s_ba_command_executing)
            s_ba[0x6]++;
        else
            s_ba_command_executing = 0;

        *out = return_val;
        return 1;
    }

    /* Real PCSX2's own fallback path masks the address with 0x1F
     * rather than the intra-register offset - ported verbatim (see
     * ee_dve.h's header comment on why this quirk is kept, not
     * "fixed"). */
    *out = s_ba[addr & 0x1Fu];
    return 1;
}

int ee_dve_mmio_write16(uint32_t addr, uint16_t val)
{
    if (addr < EE_DVE_BASE || addr >= EE_DVE_BASE + EE_DVE_SIZE)
        return 0;

    uint32_t masked_mem = (addr - EE_DVE_BASE) & 0xFFu;

    if (masked_mem == 0x6u) {
        s_ba[0x6] &= (uint16_t)~3u;
    } else {
        s_ba[masked_mem] = val;
    }

    if (masked_mem == 0x00u) { /* Command Execute Reg */
        if (s_ba[0x2] == 0x4Fu || s_ba[0x2] == 0x41u) {
            s_ba_error_detected = 1;
        } else if (s_ba[masked_mem] & 0x80u) { /* Start executing */
            if (s_ba[0x2] == 0x43u) { /* Write Mode */
                int size = (int)(s_ba[masked_mem] & 0xFu);
                s_ba_current_reg = s_ba[0x10];
                size--;
                for (int i = 0; i < size; i++)
                    s_dve_regs[s_ba_current_reg] = s_ba[0x12 + i];
                s_ba_command_executing = 1;
                s_ba_error_detected = 0;
            } else if (s_ba[0x2] == 0x42u) { /* Read Mode */
                int size = (int)(s_ba[masked_mem] & 0xFu);
                for (int i = 0; i < size; i++)
                    s_ba[0x10 + i] = s_dve_regs[s_ba_current_reg];
                s_ba_command_executing = 1;
                s_ba_error_detected = 0;
            }
        }
    } else if (masked_mem == 0xAu) { /* Power/Standby Reg */
        s_ba_error_detected = (val == 0) ? 1 : 0;
    }

    return 1;
}
