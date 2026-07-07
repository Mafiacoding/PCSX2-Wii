/*
 * PCSX2-Wii - experimental PS2 emulator port skeleton
 *
 * Boot entry point. Initializes Wii video/console via libogc, mounts
 * SD/USB via libfat, loads a PS2 BIOS image, and hands off to the EE
 * interpreter core. This is NOT a functional PS2 emulator - see
 * docs/STATUS.md for an honest description of what actually works.
 *
 * NATIVE WII TEST MENU (added on request, "damit wir testen koennen ob
 * es ueberhaupt laeuft"): everything below the "wii_console_setup"
 * helper and above "int main" is a small, self-contained, native Wii
 * UI drawn directly into the XFB (reusing gs_wii_output.c's already-
 * tested RGB->YCbCr conversion) - it is NOT part of PS2 emulation and
 * does not pretend to be. Its only job is to give a clear, immediate,
 * PCSX2/PS2-BIOS-styled visual signal that the .dol actually boots,
 * initializes video/pad, and stays interactively responsive on real
 * hardware/Dolphin - independent of whether a BIOS image is present
 * or how far the EE/IOP interpreters get. The menu's "BIOS Boot Test"
 * action is what actually exercises the real emulator core and
 * reports real instruction counts/halt reasons back on screen.
 */

#include <gccore.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>

#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
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

/* ====================================================================
 * Native Wii test-menu drawing helpers (direct XFB pixel writes, YUV
 * packed 2-px-per-word format, via gs_wii_output.c's already-tested
 * gs_rgb8_pair_to_ycbcr() - see that file's header for the citation).
 * Not part of PS2 GS emulation - this writes straight into the real
 * Wii framebuffer, independent of the emulated GS local memory.
 * ==================================================================== */

static inline void put_pixel_pair(uint32_t x_even, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    if (y >= rmode->xfbHeight || x_even >= rmode->fbWidth) return;
    uint32_t *row = (uint32_t *)((uint8_t *)xfb + (size_t)y * rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    row[x_even / 2] = gs_rgb8_pair_to_ycbcr(r, g, b, r, g, b);
}

static void fill_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b)
{
    x0 &= ~1u;
    if (w & 1u) w++;
    uint32_t y_end = y0 + h;
    uint32_t x_end = x0 + w;
    for (uint32_t y = y0; y < y_end && y < rmode->xfbHeight; y++)
        for (uint32_t x = x0; x < x_end && x < rmode->fbWidth; x += 2)
            put_pixel_pair(x, y, r, g, b);
}

static void draw_border(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                         uint32_t t, uint8_t r, uint8_t g, uint8_t b)
{
    fill_rect(x0, y0, w, t, r, g, b);
    fill_rect(x0, y0 + h - t, w, t, r, g, b);
    fill_rect(x0, y0, t, h, r, g, b);
    fill_rect(x0 + w - t, y0, t, h, r, g, b);
}

static void draw_gradient_background(uint8_t top_r, uint8_t top_g, uint8_t top_b,
                                      uint8_t bot_r, uint8_t bot_g, uint8_t bot_b)
{
    uint32_t h = rmode->xfbHeight;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t r = (uint8_t)((int)top_r + ((int)bot_r - (int)top_r) * (int)y / (int)h);
        uint8_t g = (uint8_t)((int)top_g + ((int)bot_g - (int)top_g) * (int)y / (int)h);
        uint8_t b = (uint8_t)((int)top_b + ((int)bot_b - (int)top_b) * (int)y / (int)h);
        fill_rect(0, y, rmode->fbWidth, 1, r, g, b);
    }
}

static void flush_screen(void)
{
    DCFlushRange(xfb, rmode->fbWidth * rmode->xfbHeight * VI_DISPLAY_PIX_SZ);
}

/* Approximate character-cell size of libogc's default console font
 * (8x16), used only to line printf-based text labels up next to the
 * pixel-drawn boxes below - not pixel-exact, just close enough for a
 * readable test screen. */
#define CHAR_W 8
#define CHAR_H 16
static void goto_rc(uint32_t px, uint32_t py) { printf("\x1b[%u;%uH", (unsigned)(py / CHAR_H) + 1, (unsigned)(px / CHAR_W) + 1); }

#define MENU_ITEM_COUNT 3
static const char *menu_labels[MENU_ITEM_COUNT] = {
    "BIOS Boot Test",
    "GS / GIF Demo",
    "About",
};
static const uint8_t box_colors[MENU_ITEM_COUNT][3] = {
    { 30, 70, 160 },
    { 30, 140, 80 },
    { 120, 55, 150 },
};

#define BOX_W 160u
#define BOX_H 110u
#define BOX_GAP 30u
#define BOX_Y 190u

static void box_geometry(int index, uint32_t *out_x)
{
    uint32_t total_w = MENU_ITEM_COUNT * BOX_W + (MENU_ITEM_COUNT - 1) * BOX_GAP;
    uint32_t start_x = (rmode->fbWidth > total_w) ? (rmode->fbWidth - total_w) / 2 : 0;
    *out_x = start_x + (uint32_t)index * (BOX_W + BOX_GAP);
}

/* Draws the full PS2/PCSX2-Browser-styled menu screen: dark navy
 * gradient background, a title bar, one box per menu item (bright
 * border on the currently selected one), labels beneath each box, and
 * a status footer. Text is drawn via printf/ANSI cursor positioning
 * on top of the pixel-drawn regions (both write into the same XFB). */
static void draw_menu_screen(int selected, int bios_found, const char *status_line)
{
    draw_gradient_background(12, 18, 55, 0, 0, 8);

    fill_rect(0, 0, rmode->fbWidth, 46, 22, 30, 85);
    flush_screen();

    goto_rc(16, 8);
    printf("PCSX2-Wii  -  System Test Menu");
    goto_rc(16, 26);
    printf("(experimental EE/IOP interpreter bring-up - see docs/STATUS.md)");

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        uint32_t bx;
        box_geometry(i, &bx);
        fill_rect(bx, BOX_Y, BOX_W, BOX_H, box_colors[i][0], box_colors[i][1], box_colors[i][2]);
        if (i == selected)
            draw_border(bx, BOX_Y, BOX_W, BOX_H, 4, 250, 220, 40);
        else
            draw_border(bx, BOX_Y, BOX_W, BOX_H, 2, 10, 10, 30);
        flush_screen();

        goto_rc(bx + 10, BOX_Y + BOX_H + 14);
        printf("%s", menu_labels[i]);
    }

    uint32_t foot_y = rmode->xfbHeight > 110 ? rmode->xfbHeight - 100 : 400;
    goto_rc(20, foot_y);
    printf("BIOS: %s                              ",
           bios_found ? "found on SD/USB" : "not found (Boot Test will report this)");
    goto_rc(20, foot_y + CHAR_H);
    printf("D-Pad Left/Right: choose    A: run    B: back to menu           ");
    goto_rc(20, foot_y + 2 * CHAR_H);
    printf("%-64s", status_line ? status_line : "");
}

/* Small liveness indicator: a pulsing colored square + frame counter
 * in the top-right corner, redrawn every loop iteration regardless of
 * menu state - the whole point of this screen is proving the app is
 * genuinely still running (not frozen) on real hardware. */
static void draw_heartbeat(uint32_t frame)
{
    static const uint8_t colors[4][3] = {
        { 210, 60, 60 }, { 60, 210, 60 }, { 60, 60, 210 }, { 220, 200, 50 },
    };
    int idx = (int)((frame / 20u) % 4u);
    uint32_t hb_x = rmode->fbWidth > 40 ? rmode->fbWidth - 34 : 0;
    fill_rect(hb_x, 12, 22, 22, colors[idx][0], colors[idx][1], colors[idx][2]);
    DCFlushRange((uint8_t *)xfb + (size_t)12 * rmode->fbWidth * VI_DISPLAY_PIX_SZ,
                 rmode->fbWidth * 22 * VI_DISPLAY_PIX_SZ);

    static const char spinner[4] = { '|', '/', '-', '\\' };
    goto_rc(hb_x > 80 ? hb_x - 80 : 0, 12);
    printf("alive %c  frame %-8u", spinner[frame % 4u], (unsigned)frame);
}

/* Blocks (still servicing the heartbeat animation) until one of the
 * given buttons is pressed. Used by every menu action screen so the
 * user can read the result text before returning to the menu. */
static void wait_for_button(uint16_t mask)
{
    uint32_t frame = 0;
    for (;;) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        uint16_t down = PAD_ButtonsDown(0);
        draw_heartbeat(frame++);
        if (down & mask) return;
    }
}

static int   g_fat_mounted = 0;
static int   g_bios_ok = 0;
static int   g_system_started = 0;
static bios_image_t g_bios;

/* Menu action 1: actually exercises the real EE/IOP interpreter core
 * (mounts SD/USB, loads a BIOS image if present, runs the interleaved
 * scheduler up to a bounded instruction cap so the UI doesn't appear
 * to hang, and reports real instruction counts + halt reasons). This
 * is the part of the menu that answers "does the emulator core itself
 * actually run on real hardware", as opposed to just "does the .dol
 * boot and draw a screen" (which the menu itself already proves). */
#define DEMO_STEP_CAP 2000000ull /* bounded so the UI stays responsive - real hardware/this project's own diagnostics have run into the tens of millions of instructions before halting; see docs/STATUS.md */

static void action_bios_boot_test(void)
{
    draw_gradient_background(6, 6, 22, 0, 0, 6);
    goto_rc(16, 40);
    printf("BIOS Boot Test\n");
    printf("==============\n\n");

    if (!g_fat_mounted) {
        g_fat_mounted = fatInitDefault() ? 1 : 0;
    }
    if (!g_fat_mounted) {
        printf("[!] fatInitDefault() failed - no SD/USB storage found.\n");
        printf("    Insert an SD card with /pcsx2/bios/*.bin and try again.\n");
    } else {
        printf("[+] Storage mounted.\n");
        if (!g_bios_ok) {
            memset(&g_bios, 0, sizeof(g_bios));
            g_bios_ok = (bios_load("sd:/pcsx2/bios/SCPH39001.bin", &g_bios) == 0 ||
                         bios_load("sd:/pcsx2/bios/SCPH10000.bin", &g_bios) == 0 ||
                         bios_load("sd:/pcsx2/bios/bios.bin", &g_bios) == 0);
        }
        if (!g_bios_ok) {
            printf("[!] Could not load a PS2 BIOS image from sd:/pcsx2/bios/\n");
            printf("    Place a legally-dumped PS2 BIOS there (e.g. bios.bin).\n");
        } else {
            printf("[+] BIOS loaded: %s\n", g_bios.name);
            printf("    size=%u bytes  rom_ver=%s\n", (unsigned)g_bios.size, g_bios.version_string);

            if (!g_system_started) {
                system_init(&g_bios, &g_bios);
                g_system_started = 1;
                printf("\n[+] EE + IOP cores initialized (interleaved scheduler).\n");
                printf("[+] Running interleaved for up to %llu instruction-slices"
                       " (bounded test cap, not a real limit)...\n\n", DEMO_STEP_CAP);

                int both_halted = system_run_interleaved(DEMO_STEP_CAP);

                ee_state_t *ee = ee_core_get_state();
                iop_state_t *iop = iop_core_get_state();
                printf("[+] EE : instructions_executed=%llu  halted=%d\n",
                       (unsigned long long)ee->instructions_executed, ee->halted);
                printf("       reason: %s\n", ee->halted ? ee->halt_reason : "(still running - hit the test cap)");
                printf("[+] IOP: instructions_executed=%llu  halted=%d\n",
                       (unsigned long long)iop->instructions_executed, iop->halted);
                printf("       reason: %s\n", iop->halted ? iop->halt_reason : "(still running - hit the test cap)");
                printf("\n%s\n", both_halted
                       ? "Both cores halted on their own (see reasons above)."
                       : "Test cap reached first - both cores were still executing real instructions.");
            } else {
                ee_state_t *ee = ee_core_get_state();
                iop_state_t *iop = iop_core_get_state();
                printf("\n(Already run this session.)\n");
                printf("[+] EE : instructions_executed=%llu  halted=%d  reason: %s\n",
                       (unsigned long long)ee->instructions_executed, ee->halted,
                       ee->halted ? ee->halt_reason : "(cap reached)");
                printf("[+] IOP: instructions_executed=%llu  halted=%d  reason: %s\n",
                       (unsigned long long)iop->instructions_executed, iop->halted,
                       iop->halted ? iop->halt_reason : "(cap reached)");
            }
        }
    }

    printf("\nPress A or B to return to the menu.\n");
    wait_for_button(PAD_BUTTON_A | PAD_BUTTON_B);
}

/* Menu action 2: the existing pixel-pipeline demos (fixed test bars
 * via direct GS-memory writes, then a Gouraud triangle driven through
 * a real hand-built GIF packet + dma_channel_kick()) - unchanged
 * logic from before this menu existed, just moved into its own
 * action function. */
static void action_gs_gif_demo(void)
{
    draw_gradient_background(6, 6, 22, 0, 0, 6);
    goto_rc(16, 40);
    printf("GS / GIF Pixel Pipeline Demo\n");
    printf("============================\n\n");
    printf("Drawing a fixed 4-color test pattern via direct GS-memory\n");
    printf("writes, then a Gouraud triangle via a real hand-built GIF\n");
    printf("packet through dma_channel_kick() - see docs/ROADMAP.md.\n\n");

    gs_mem_init();
    const uint32_t bar_w = rmode->fbWidth / 4;
    const uint32_t colors[4] = { 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0x00FFFFFFu };
    for (int bar = 0; bar < 4; bar++) {
        for (uint32_t y = 0; y < 40; y++) {
            for (uint32_t x = 0; x < bar_w; x++) {
                gs_mem_write_psmct32(0, rmode->fbWidth, bar * bar_w + x, y, colors[bar]);
            }
        }
    }
    gs_blit_psmct32_to_xfb(xfb, rmode->fbWidth, 0, 130, 0, rmode->fbWidth, 0, 0, rmode->fbWidth, 40);
    DCFlushRange((uint8_t *)xfb + (size_t)130 * rmode->fbWidth * VI_DISPLAY_PIX_SZ,
                 rmode->fbWidth * 40 * VI_DISPLAY_PIX_SZ);

    static uint8_t pkt[16 * (1 + 3 + 3 * 2)];
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
    const int nloop = 3 + 2 * n_verts;
    WLE32(pkt + off,     (uint32_t)nloop | (1u << 15));
    WLE32(pkt + off + 4, (0u << 26) | (1u << 28));
    WLE32(pkt + off + 8, GIF_REG_AD);
    WLE32(pkt + off + 12, 0);
    off += 16;

    uint32_t fbw_field = rmode->fbWidth / 64u;
    APPEND_AD((fbw_field << 9), 0, GS_REG_FRAME_1);
    APPEND_AD(0, 0, GS_REG_XYOFFSET_1);
    APPEND_AD((uint32_t)PRIM_TYPE_TRIANGLE | PRIM_IIP_MASK, 0, GS_REG_PRIM);

    static const uint32_t vcolor[3] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u };
    static const int32_t vpos[3][2] = { { 40, 30 }, { 220, 30 }, { 40, 170 } };
    for (int i = 0; i < n_verts; i++) {
        uint32_t rgba = vcolor[i];
        uint32_t rgbaq_lo = (rgba & 0xFFu) | (((rgba >> 8) & 0xFFu) << 8) |
                            (((rgba >> 16) & 0xFFu) << 16) | (((rgba >> 24) & 0xFFu) << 24);
        APPEND_AD(rgbaq_lo, 0, GS_REG_RGBAQ);
        APPEND_AD((uint32_t)(vpos[i][0] << 4), (uint32_t)(vpos[i][1] << 4), GS_REG_XYZ2);
    }
#undef APPEND_AD
#undef WLE32

    /* This demo needs the EE core (for its RAM + DMA sink wiring) but
     * NOT a real BIOS - initialize a throwaway core if the boot test
     * above hasn't already done so. */
    if (!g_system_started) {
        bios_image_t empty;
        memset(&empty, 0, sizeof(empty));
        system_init(&empty, &empty);
    }
    ee_state_t *ee = ee_core_get_state();
    const uint32_t pkt_ram_addr = 0x00100000u;
    memcpy(ee->ram + pkt_ram_addr, pkt, (size_t)off);

    dma_state_t *dma = dma_get_state();
    dma->chan[DMA_CHANNEL_GIF].chcr = 0;
    dma->chan[DMA_CHANNEL_GIF].madr = pkt_ram_addr;
    dma->chan[DMA_CHANNEL_GIF].qwc  = (uint32_t)(off / 16);
    dma_channel_kick(DMA_CHANNEL_GIF);

    gs_blit_psmct32_to_xfb(xfb, rmode->fbWidth, 0, 170, 0, rmode->fbWidth, 0, 40, rmode->fbWidth, 180);
    DCFlushRange((uint8_t *)xfb + (size_t)170 * rmode->fbWidth * VI_DISPLAY_PIX_SZ,
                 rmode->fbWidth * 180 * VI_DISPLAY_PIX_SZ);

    goto_rc(16, 360);
    printf("Real GIF-packet demo drawn via dma_channel_kick() (DMA channel error: %u).\n",
           (unsigned)dma->chan[DMA_CHANNEL_GIF].last_error);
    printf("\nPress A or B to return to the menu.\n");
    wait_for_button(PAD_BUTTON_A | PAD_BUTTON_B);
}

static void action_about(void)
{
    draw_gradient_background(6, 6, 22, 0, 0, 6);
    goto_rc(16, 40);
    printf("About PCSX2-Wii\n");
    printf("===============\n\n");
    printf("Experimental PS2 EE/IOP interpreter port skeleton for Wii,\n");
    printf("built on devkitPPC + libogc.\n\n");
    printf("github.com/Mafiacoding/PCSX2-Wii\n\n");
    printf("This is NOT a functional PS2 emulator yet - it does not boot\n");
    printf("a real BIOS to the OSD splash screen. See docs/STATUS.md and\n");
    printf("docs/ROADMAP.md in the repo for an honest account of what\n");
    printf("actually works today (EE/IOP interpreters, DMA/GIF/GS register\n");
    printf("plumbing, a real IOP module/IRX loader, a real VU opcode\n");
    printf("table, an SPU2 register scaffold) and what is still missing.\n\n");
    printf("Press A or B to return to the menu.\n");
    wait_for_button(PAD_BUTTON_A | PAD_BUTTON_B);
}

int main(int argc, char **argv)
{
    wii_console_setup();

    int selected = 0;
    uint32_t frame = 0;
    draw_menu_screen(selected, g_bios_ok, "Use D-Pad Left/Right to choose, A to run.");

    for (;;) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        uint16_t down = PAD_ButtonsDown(0);
        int redraw = 0;

        if (down & PAD_BUTTON_LEFT) {
            selected = (selected + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
            redraw = 1;
        } else if (down & PAD_BUTTON_RIGHT) {
            selected = (selected + 1) % MENU_ITEM_COUNT;
            redraw = 1;
        } else if (down & PAD_BUTTON_A) {
            switch (selected) {
                case 0: action_bios_boot_test(); break;
                case 1: action_gs_gif_demo(); break;
                case 2: action_about(); break;
            }
            redraw = 1;
        }

        if (redraw)
            draw_menu_screen(selected, g_bios_ok, "Use D-Pad Left/Right to choose, A to run.");

        draw_heartbeat(frame++);
    }

    return 0;
}
