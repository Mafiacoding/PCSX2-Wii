/*
 * test_ee_sio.c - host-native test for the Round 392 EE debug SIO
 * UART register model. See include/core/hw/ee_sio.h for the full
 * design rationale and citations (ps2sdk ee/kernel/include/sio.h).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "core/hw/ee_sio.h"
#include "core/ee/ee_core.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

#define EE_SIO_LCR    0x1000F100u
#define EE_SIO_LSR    0x1000F110u
#define EE_SIO_IER    0x1000F120u
#define EE_SIO_ISR    0x1000F130u
#define EE_SIO_FCR    0x1000F140u
#define EE_SIO_BGR    0x1000F150u
#define EE_SIO_TXFIFO 0x1000F180u
#define EE_SIO_RXFIFO 0x1000F1C0u

int main(void)
{
    /* --- direct ee_sio_mmio_* unit tests (no EE interpreter needed) --- */
    {
        ee_sio_init();
        uint32_t out;

        CHECK(ee_sio_mmio_write32(EE_SIO_LCR, 0x03u) == 1, "LCR write recognized");
        CHECK(ee_sio_mmio_read32(EE_SIO_LCR, &out) == 1 && out == 0x03u, "LCR real scratch-register round trip");

        CHECK(ee_sio_mmio_write32(EE_SIO_IER, 0x05u) == 1, "IER write recognized");
        CHECK(ee_sio_mmio_read32(EE_SIO_IER, &out) == 1 && out == 0x05u, "IER real scratch-register round trip");

        CHECK(ee_sio_mmio_write32(EE_SIO_FCR, 0x07u) == 1, "FCR write recognized");
        CHECK(ee_sio_mmio_read32(EE_SIO_FCR, &out) == 1 && out == 0x07u, "FCR real scratch-register round trip");

        CHECK(ee_sio_mmio_write32(EE_SIO_BGR, 0x2Cu) == 1, "BGR write recognized");
        CHECK(ee_sio_mmio_read32(EE_SIO_BGR, &out) == 1 && out == 0x2Cu, "BGR real scratch-register round trip");

        CHECK(ee_sio_mmio_read32(EE_SIO_LSR, &out) == 1 && out == 0u, "LSR always reports 0 (no real RX source, DR/OE/PE/FE all clear)");
        CHECK(ee_sio_mmio_read32(EE_SIO_ISR, &out) == 1 && (out & 0x02u) != 0u, "ISR always reports TX_EMPTY set (no transmit latency modeled)");
        CHECK(ee_sio_mmio_read32(EE_SIO_ISR, &out) == 1 && (out & 0x01u) == 0u, "ISR RX_DATA always clear, matches LSR.DR=0");
        CHECK(ee_sio_mmio_read32(EE_SIO_RXFIFO, &out) == 1 && out == 0u, "RXFIFO always reads 0, no real data source");

        int unrelated;
        unrelated = ee_sio_mmio_read32(0x1000F200u, &out);
        CHECK(unrelated == 0, "an unrelated address outside the real SIO register window is correctly not claimed");
    }

    /* --- TXFIFO capture: the actual diagnostic payoff of this round --- */
    {
        ee_sio_init();
        const char *msg = "OK\n";
        for (const char *p = msg; *p; p++) {
            int handled = ee_sio_mmio_write32(EE_SIO_TXFIFO, (uint32_t)(unsigned char)*p);
            CHECK(handled == 1, "TXFIFO write recognized");
        }
        CHECK(strcmp(ee_sio_get_console_text(), "OK\n") == 0, "TXFIFO bytes captured into the console buffer in real transmit order");
        CHECK(ee_sio_get_state()->bytes_written == 3u, "bytes_written stat matches the real number of bytes transmitted");

        /* Real hardware TXFIFO is a byte-wide register - only the low
         * 8 bits of a written 32-bit value are the real transmitted
         * byte; high bits must be ignored, not corrupt the capture. */
        ee_sio_init();
        ee_sio_mmio_write32(EE_SIO_TXFIFO, 0xDEADBE41u); /* low byte 0x41 = 'A' */
        CHECK(strcmp(ee_sio_get_console_text(), "A") == 0, "only the real low byte of a TXFIFO write is captured, high garbage bits ignored");
    }

    /* --- console buffer truncation is safe (bounded, NUL-terminated) --- */
    {
        ee_sio_init();
        for (int i = 0; i < EE_SIO_CONSOLE_BUF_SIZE + 100; i++)
            ee_sio_mmio_write32(EE_SIO_TXFIFO, (uint32_t)'x');
        const char *text = ee_sio_get_console_text();
        CHECK(strlen(text) < EE_SIO_CONSOLE_BUF_SIZE, "console buffer never overflows past its real fixed size");
        CHECK(ee_sio_get_state()->bytes_written == (uint32_t)(EE_SIO_CONSOLE_BUF_SIZE + 100), "bytes_written keeps counting the real total even after the display buffer saturates");
    }

    /* --- real integration: EE interpreter's own ee_mem_write32/read32
     * correctly route through the KSEG1-mirrored SIO address, the
     * same real BIOS/ps2sdk convention already established for INTC/
     * timers (task #171/172's own KSEG0/1 masking fix) --- */
    {
        ee_state_t *ee = ee_core_get_state();
        memset(ee, 0, sizeof(*ee));
        ee->ram = (uint8_t *)calloc(1, 32 * 1024 * 1024);
        ee_sio_init();

        /* Real code addresses hardware registers via their KSEG1
         * (uncached) mirror, e.g. 0xB000F180, not the bare physical
         * 0x1000F180 literal - exactly the same real convention this
         * project's own ee_hw_mmio_addr() already masks for every
         * other MMIO subsystem. */
        ee_mem_write32(ee, 0xB000F180u, (uint32_t)'H');
        ee_mem_write32(ee, 0xB000F180u, (uint32_t)'i');
        CHECK(strcmp(ee_sio_get_console_text(), "Hi") == 0, "a real KSEG1-mirrored EE MMIO write reaches the SIO TXFIFO model correctly");

        uint32_t v = ee_mem_read32(ee, 0xB000F130u); /* ISR */
        CHECK((v & 0x02u) != 0u, "a real KSEG1-mirrored EE MMIO read of ISR sees TX_EMPTY set");

        free(ee->ram);
    }

    printf("\n%d failures\n", failures);
    return failures ? 1 : 0;
}
