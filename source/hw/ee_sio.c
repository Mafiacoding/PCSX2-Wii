/*
 * ee_sio.c - EE debug serial UART register model. See ee_sio.h for
 * exact per-register semantics and citations.
 */
#include "core/hw/ee_sio.h"
#include <string.h>
#include <stdio.h>

#define EE_SIO_LCR    0x1000F100u
#define EE_SIO_LSR    0x1000F110u
#define EE_SIO_IER    0x1000F120u
#define EE_SIO_ISR    0x1000F130u
#define EE_SIO_FCR    0x1000F140u
#define EE_SIO_BGR    0x1000F150u
#define EE_SIO_TXFIFO 0x1000F180u
#define EE_SIO_RXFIFO 0x1000F1C0u

#define EE_SIO_LSR_DR 0x01u
#define EE_SIO_ISR_RX_DATA  0x01u
#define EE_SIO_ISR_TX_EMPTY 0x02u

static ee_sio_state_t g_sio;

void ee_sio_init(void)
{
    memset(&g_sio, 0, sizeof(g_sio));
}

ee_sio_state_t *ee_sio_get_state(void) { return &g_sio; }

const char *ee_sio_get_console_text(void)
{
    return g_sio.console_buf;
}

int ee_sio_mmio_read32(uint32_t addr, uint32_t *out)
{
    switch (addr) {
        case EE_SIO_LCR:
            *out = g_sio.lcr;
            return 1;
        case EE_SIO_LSR:
            /* DR (data ready) always 0 - no real RX source in this
             * emulation, see header comment. OE/PE/FE (bits 1-3)
             * likewise never set. */
            *out = 0u;
            return 1;
        case EE_SIO_IER:
            *out = g_sio.ier;
            return 1;
        case EE_SIO_ISR:
            /* TX_EMPTY always set (this project models no transmit
             * latency, so the transmitter is always immediately
             * ready); RX_DATA always clear (matches LSR.DR=0). */
            *out = EE_SIO_ISR_TX_EMPTY;
            return 1;
        case EE_SIO_FCR:
            *out = g_sio.fcr;
            return 1;
        case EE_SIO_BGR:
            *out = g_sio.bgr;
            return 1;
        case EE_SIO_RXFIFO:
            /* No data ever available, matches LSR.DR=0 - an honest
             * "nothing connected" default. */
            *out = 0u;
            return 1;
        default:
            return 0;
    }
}

int ee_sio_mmio_write32(uint32_t addr, uint32_t value)
{
    switch (addr) {
        case EE_SIO_LCR:
            g_sio.lcr = value;
            return 1;
        case EE_SIO_LSR:
            /* Real LSR is read-only status; writes are a real no-op
             * here (nothing in this project's own model needs the
             * write acknowledged further). */
            return 1;
        case EE_SIO_IER:
            g_sio.ier = value;
            return 1;
        case EE_SIO_ISR:
            /* Real hardware ISR is read-only status too; honored as
             * a no-op, matching LSR's own convention above. */
            return 1;
        case EE_SIO_FCR:
            g_sio.fcr = value;
            return 1;
        case EE_SIO_BGR:
            g_sio.bgr = value;
            return 1;
        case EE_SIO_TXFIFO: {
            /* The real byte a program is transmitting - captured
             * into the debug-console ring buffer (see header
             * comment). Only the low 8 bits are the real transmitted
             * byte (SIO_TXFIFO is a real byte-wide FIFO register;
             * this project's generic 32-bit MMIO write path still
             * routes here since no narrower write variant currently
             * exists for this address, matching this project's
             * existing convention elsewhere of taking the low byte
             * for byte-wide hardware FIFOs). */
            uint8_t byte = (uint8_t)(value & 0xFFu);
            g_sio.bytes_written++;
            if (g_sio.console_len + 1u < EE_SIO_CONSOLE_BUF_SIZE) {
                g_sio.console_buf[g_sio.console_len++] = (char)byte;
                g_sio.console_buf[g_sio.console_len] = '\0';
            }
            fputc((int)byte, stderr);
            return 1;
        }
        case EE_SIO_RXFIFO:
            /* Real register is read-only (receive); a write is a
             * real no-op. */
            return 1;
        default:
            return 0;
    }
}
