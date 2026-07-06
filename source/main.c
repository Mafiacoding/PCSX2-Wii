/*
 * PCSX2-Wii - experimental PS2 emulator port skeleton
 *
 * Boot entry point. Initializes Wii video/console via libogc, mounts
 * SD/USB via libfat, loads a PS2 BIOS image, and hands off to the EE
 * interpreter core. This is NOT a functional PS2 emulator - see
 * docs/STATUS.md for an honest description of what actually works.
 */

#include <gccore.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>

#include "core/ee/ee_core.h"
#include "core/system.h"
#include "core/bios_loader.h"
#include "core/hw/gs_mem.h"
#include "core/hw/gs_wii_output.h"
#include "core/hw/dma.h"
#include "core/hw/gif.h"

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

static void wii_console_setup(void)
{
    VIDEO_Init();
    PAD_Init();

    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

    CON_Init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
             rmode->fbWidth * VI_DISPLAY_PIX_SZ);

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
}

int main(int argc, char **argv)
{
    wii_console_setup();

    printf("\n\x1b[2;0HPCSX2-Wii (experimental) - EE core bring-up\n");
    printf("=============================================\n\n");

    if (!fatInitDefault()) {
        printf("[!] fatInitDefault() failed - no SD/USB storage found.\n");
        printf("    Insert an SD card with /pcsx2/bios/*.bin and reboot.\n");
        goto halt;
    }
    printf("[+] Storage mounted.\n");

    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));

    if (bios_load("sd:/pcsx2/bios/SCPH39001.bin", &bios) != 0 &&
        bios_load("sd:/pcsx2/bios/bios.bin", &bios) != 0) {
        printf("[!] Could not load a PS2 BIOS image from sd:/pcsx2/bios/\n");
        printf("    Place a legally-dumped PS2 BIOS there (e.g. bios.bin).\n");
        goto halt;
    }

    printf("[+] BIOS loaded: %s\n", bios.name);
    printf("    size=%u bytes  rom_ver=%s\n", (unsigned)bios.size, bios.version_string);

    /* Both the EE and IOP boot from the same physical BIOS ROM on
     * real hardware, and are wired together here via system_init()
     * so they can actually run alongside each other - see
     * core/system.h. Before this, the IOP core existed but never
     * ran at all from main.c; now both cores step interleaved, which
     * is what a real SIF handshake needs (one side polling a flag
     * the other side sets can't work if the cores run one after the
     * other instead of together). */
    system_init(&bios, &bios);

    printf("\n[+] EE + IOP cores initialized (interleaved scheduler).\n");
    printf("[+] Entering interleaved fetch/decode/execute loop (interpreters only)...\n\n");

    /* Runs both interpreters starting at their respective BIOS reset
     * vectors, alternating one instruction at a time (see
     * core/system.h for why, and its current known limitations - not
     * clock-rate accurate). No recompiler is engaged by default - see
     * core/recompiler for the experimental PPC dynarec proof-of-concept
     * and its limitations. max_slices=0: no cap, run until both halt
     * (real hardware has no such cap either). */
    system_run_interleaved(0);

    /* First real "pixels reach the actual screen" milestone: this is
     * NOT the BIOS splash screen (nothing in the GIF/DMA pipeline
     * actually runs yet - see docs/ROADMAP.md), just a fixed test
     * pattern proving the GS-local-memory -> YCbCr -> Wii XFB path
     * genuinely works end-to-end on real hardware/Dolphin, not only
     * in the host-native unit tests (tests/test_gs_output.c). */
    {
        gs_mem_init();
        const uint32_t bar_w = rmode->fbWidth / 4;
        const uint32_t colors[4] = {
            0x000000FFu, /* red   (0xAABBGGRR) */
            0x0000FF00u, /* green */
            0x00FF0000u, /* blue  */
            0x00FFFFFFu, /* white */
        };
        for (int bar = 0; bar < 4; bar++) {
            for (uint32_t y = 0; y < 40; y++) {
                for (uint32_t x = 0; x < bar_w; x++) {
                    gs_mem_write_psmct32(0, rmode->fbWidth,
                                          bar * bar_w + x, y, colors[bar]);
                }
            }
        }
        gs_blit_psmct32_to_xfb(xfb, rmode->fbWidth, 0, 0,
                                0, rmode->fbWidth, 0, 0,
                                rmode->fbWidth, 40);
        DCFlushRange(xfb, rmode->fbWidth * 40 * VI_DISPLAY_PIX_SZ);
        printf("[+] Pixel pipeline demo: 4-color test bars written to the real framebuffer.\n");
    }

    /* Second milestone: drive the SAME pixel path through the REAL
     * DMA -> GIF -> rasterizer pipeline instead of writing GS memory
     * directly - i.e. a genuine (if hand-built, not BIOS-driven) GIF
     * packet, delivered exactly the way real EE code would deliver
     * one: written into EE RAM, then kicked via dma_channel_kick() on
     * the GIF channel (normal mode), which calls gif_process_
     * quadwords() through the sink wired up in ee_core_init(). Draws
     * a Gouraud-shaded triangle (PRIM's real IIP bit) below the color
     * bars, proving the GS Gouraud-shading work from this session
     * exercises the real pipeline, not just host-native tests. */
    {
        static uint8_t pkt[16 * (1 + 3 + 3 * 2)]; /* tag + (FRAME_1+XYOFFSET_1+PRIM) + 3*(RGBAQ+XYZ2) */
        memset(pkt, 0, sizeof(pkt));
        int off = 0;

#define WLE32(p, v) do { \
    uint32_t _v = (uint32_t)(v); \
    (p)[0] = (uint8_t)(_v);       (p)[1] = (uint8_t)(_v >> 8); \
    (p)[2] = (uint8_t)(_v >> 16); (p)[3] = (uint8_t)(_v >> 24); \
} while (0)
#define APPEND_AD(data_lo, data_hi, addr) do { \
    WLE32(pkt + off,      (data_lo)); \
    WLE32(pkt + off + 4,  (data_hi)); \
    WLE32(pkt + off + 8,  (addr));    \
    WLE32(pkt + off + 12, 0);         \
    off += 16; \
} while (0)

        const int n_verts = 3;
        const int nloop = 3 + 2 * n_verts; /* FRAME_1, XYOFFSET_1, PRIM, then (RGBAQ+XYZ2) per vertex */
        WLE32(pkt + off,     (uint32_t)nloop | (1u << 15));
        WLE32(pkt + off + 4, (0u << 26) | (1u << 28)); /* PACKED mode, NREG=1 */
        WLE32(pkt + off + 8, GIF_REG_AD);
        WLE32(pkt + off + 12, 0);
        off += 16;

        uint32_t fbw_field = rmode->fbWidth / 64u; /* FRAME_1's FBW is in units of 64px */
        APPEND_AD((fbw_field << 9), 0, GS_REG_FRAME_1);
        APPEND_AD(0, 0, GS_REG_XYOFFSET_1);
        APPEND_AD((uint32_t)PRIM_TYPE_TRIANGLE | PRIM_IIP_MASK, 0, GS_REG_PRIM);

        /* Triangle below the color bars: (40,60)-(220,60)-(40,200),
         * red/green/blue vertices - same Gouraud demo as
         * tests/test_gif_gouraud.c, now driven through real DMA. */
        static const uint32_t vcolor[3] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u }; /* red, green, blue */
        static const int32_t vpos[3][2] = { { 40, 60 }, { 220, 60 }, { 40, 200 } };
        for (int i = 0; i < n_verts; i++) {
            uint32_t rgba = vcolor[i];
            uint32_t rgbaq_lo = (rgba & 0xFFu) | (((rgba >> 8) & 0xFFu) << 8) |
                                (((rgba >> 16) & 0xFFu) << 16) | (((rgba >> 24) & 0xFFu) << 24);
            APPEND_AD(rgbaq_lo, 0, GS_REG_RGBAQ);
            APPEND_AD((uint32_t)(vpos[i][0] << 4), (uint32_t)(vpos[i][1] << 4), GS_REG_XYZ2);
        }
#undef APPEND_AD
#undef WLE32

        ee_state_t *ee = ee_core_get_state();
        const uint32_t pkt_ram_addr = 0x00100000u; /* 1MB in - unused scratch region for this demo */
        memcpy(ee->ram + pkt_ram_addr, pkt, (size_t)off);

        dma_state_t *dma = dma_get_state();
        dma->chan[DMA_CHANNEL_GIF].chcr = 0; /* NORMAL mode (mod=0) */
        dma->chan[DMA_CHANNEL_GIF].madr = pkt_ram_addr;
        dma->chan[DMA_CHANNEL_GIF].qwc  = (uint32_t)(off / 16);
        dma_channel_kick(DMA_CHANNEL_GIF);

        gs_blit_psmct32_to_xfb(xfb, rmode->fbWidth, 0, 40,
                                0, rmode->fbWidth, 0, 40,
                                rmode->fbWidth, 180);
        DCFlushRange((uint8_t *)xfb + 40 * rmode->fbWidth * VI_DISPLAY_PIX_SZ,
                     rmode->fbWidth * 180 * VI_DISPLAY_PIX_SZ);
        printf("[+] Real GIF-packet demo: Gouraud triangle drawn via dma_channel_kick()"
               " (DMA channel error: %u).\n", (unsigned)dma->chan[DMA_CHANNEL_GIF].last_error);
    }

halt:
    printf("\nHalted. Press RESET/Power to exit.\n");
    while (1) {
        VIDEO_WaitVSync();
    }

    return 0;
}
