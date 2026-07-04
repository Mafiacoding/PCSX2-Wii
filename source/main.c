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
#include "core/bios_loader.h"
#include "core/hw/gs_mem.h"
#include "core/hw/gs_wii_output.h"

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

    ee_core_init(&bios);

    printf("\n[+] EE interpreter core initialized.\n");
    printf("[+] Entering fetch/decode/execute loop (interpreter only)...\n\n");

    /* Runs the R5900 interpreter starting at the BIOS reset vector.
     * No recompiler is engaged by default - see core/recompiler for the
     * experimental PPC dynarec proof-of-concept and its limitations. */
    ee_core_run(&bios);

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

halt:
    printf("\nHalted. Press RESET/Power to exit.\n");
    while (1) {
        VIDEO_WaitVSync();
    }

    return 0;
}
