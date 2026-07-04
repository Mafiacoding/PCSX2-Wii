#ifndef PCSX2WII_GS_H
#define PCSX2WII_GS_H

#include <stdint.h>

/*
 * GS (Graphics Synthesizer) privileged register block - skeleton
 * only, same spirit as core/hw/dma.h. Addresses cross-checked against
 * PCSX2's pcsx2/Hw.h. These are all genuinely 64-bit registers on
 * real hardware (accessed via SD/LD), unlike the DMA registers which
 * are 32-bit.
 *
 * This models the REGISTERS ONLY: values are latched and returned
 * faithfully so BIOS/game code that pokes PMODE/DISPFB/DISPLAY/etc.
 * during display setup doesn't halt the interpreter, but nothing is
 * actually rasterized - GS local memory (the 4MB eDRAM texture/frame-
 * buffer store) and primitive drawing don't exist yet. That's a much
 * bigger subproject - see docs/ROADMAP.md section 6, including real
 * PCSX2's GS/ line count for scale.
 */

typedef struct {
    uint64_t pmode;
    uint64_t smode1;
    uint64_t smode2;
    uint64_t srfsh;
    uint64_t synch1;
    uint64_t synch2;
    uint64_t syncv;
    uint64_t dispfb1;
    uint64_t display1;
    uint64_t dispfb2;
    uint64_t display2;
    uint64_t extbuf;
    uint64_t extdata;
    uint64_t extwrite;
    uint64_t bgcolor;
    uint64_t csr;
    uint64_t imr;
    uint64_t busdir;
    uint64_t siglblid;
} gs_state_t;

void gs_init(void);
gs_state_t *gs_get_state(void);

/* Returns 1 if addr is one of the known GS privileged registers and
 * handles the read/write; 0 otherwise. */
int gs_mmio_read64(uint32_t addr, uint64_t *out_val);
int gs_mmio_write64(uint32_t addr, uint64_t val);

#endif
