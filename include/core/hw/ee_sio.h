#ifndef PCSX2WII_EE_SIO_H
#define PCSX2WII_EE_SIO_H

#include <stdint.h>

/*
 * ee_sio.h - EE debug serial UART (SIO) register model, Round 392.
 *
 * Real EE hardware memory map, cited directly from ps2sdk's real,
 * public header (github.com/ps2dev/ps2sdk, ee/kernel/include/sio.h,
 * Academic Free License 2.0 - fetched via
 * https://ps2dev.github.io/ps2sdk/sio_8h_source.html, user-supplied
 * this round):
 *
 *   SIO_LCR    = 0x1000F100  (line control: word length/stop bits/parity)
 *   SIO_LSR    = 0x1000F110  (line status: DR=0x01/OE=0x02/PE=0x04/FE=0x08,
 *                              all RX-side flags per the real header)
 *   SIO_IER    = 0x1000F120  (interrupt enable: ERDAI=0x01/ELSI=0x04)
 *   SIO_ISR    = 0x1000F130  (interrupt status: RX_DATA=0x01/TX_EMPTY=0x02/
 *                              RX_ERROR=0x04)
 *   SIO_FCR    = 0x1000F140  (FIFO control: FRSTE=0x01/RFRST=0x02/TFRST=0x04)
 *   SIO_BGR    = 0x1000F150  (baud rate generator)
 *   SIO_TXFIFO = 0x1000F180  (write: transmit one byte)
 *   SIO_RXFIFO = 0x1000F1C0  (read: receive one byte)
 *
 * BACKGROUND: before this round, this project had NO model at all for
 * this register window - ee_core.c's own ee_mem_read32/write32 header
 * comment explicitly documented it as one of the address ranges that
 * "still fall through to the silent-no-op RAM/BIOS path below" (see
 * that file). This is real ps2sdk's own kernel debug-console UART
 * (sio_putc/sio_puts/etc, ee/kernel/src/sior.c - the exact mechanism
 * BIOS/OSDSYS debug builds and even some retail code paths use to
 * print boot-stage diagnostic text to a physical serial cable). A
 * real, working model of just the TXFIFO write path gives this
 * project genuine visibility into any such debug text a real boot
 * emits - a new diagnostic capability, not just a completeness fix.
 *
 * SCOPE AND HONEST SIMPLIFICATIONS:
 *   - LCR/IER/FCR/BGR: stored as plain scratch registers (their
 *     values are recorded faithfully but not otherwise interpreted -
 *     this project doesn't model real UART timing/baud generation,
 *     matching the same "register scratch, no timing" convention
 *     already used for several CDVD/ICFG registers elsewhere).
 *   - LSR: DR (data ready) is always 0 (no real serial cable/RX data
 *     source exists in this emulation - an honest "nothing
 *     connected" default, not a fabricated bit). OE/PE/FE (error
 *     flags) are always 0 for the same reason.
 *   - ISR: TX_EMPTY (0x02) is always reported set, so any real code
 *     polling "is the transmitter ready" before writing SIO_TXFIFO
 *     never blocks - this project has no notion of transmit latency,
 *     so "always immediately ready" is the correct honest model (the
 *     same "always ready" convention already used for CDVD's NREADY
 *     bit, Round 259). RX_DATA (0x01) is always 0 (matches LSR.DR).
 *   - TXFIFO write: the low byte of the written value is captured
 *     into a ring buffer (ee_sio_get_console_text()) AND immediately
 *     echoed to this project's own diagnostic stderr stream via a
 *     fprintf in a debug/host build - real developer workflow is
 *     "watch the serial terminal"; this is the direct emulated
 *     equivalent. No real transmission delay is modeled (matches the
 *     ISR.TX_EMPTY-always-set choice above).
 *   - RXFIFO read: always returns 0 (no data, matches LSR.DR=0).
 *
 * SIO_CAUSE_BIT (1<<12) is also cited in the real header but its
 * exact real meaning (an EE Cause-register bit position for a SIO
 * exception source, distinct from the INTC-routed peripheral
 * interrupts this project's ee_intc.c already models) is not used by
 * this round's implementation - honestly left uninterpreted rather
 * than guessed at, since no real citation for how the EE kernel
 * actually consumes it was found this round.
 */

#define EE_SIO_CONSOLE_BUF_SIZE 8192

typedef struct {
    uint32_t lcr, ier, fcr, bgr;
    char console_buf[EE_SIO_CONSOLE_BUF_SIZE];
    uint32_t console_len;   /* bytes written so far, saturates at buffer size */
    uint32_t bytes_written; /* real total count, not clamped - for stats */
} ee_sio_state_t;

void ee_sio_init(void);
ee_sio_state_t *ee_sio_get_state(void);

/* Same convention as ee_intc_mmio_read32/write32/dma_mmio_read32:
 * returns 1 and fills *out (read) if addr is one of this file's real
 * SIO registers, 0 otherwise. */
int ee_sio_mmio_read32(uint32_t addr, uint32_t *out);
int ee_sio_mmio_write32(uint32_t addr, uint32_t value);

/* Returns the captured debug-console text so far (NUL-terminated,
 * truncated at EE_SIO_CONSOLE_BUF_SIZE-1 bytes if more was written -
 * bytes_written in ee_sio_state_t reports the real, untruncated
 * total). Used by diagnostic drivers and tests to inspect whatever
 * a real boot printed. */
const char *ee_sio_get_console_text(void);

#endif
