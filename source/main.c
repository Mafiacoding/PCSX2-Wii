/*
 * PCSX2-Wii - experimental PS2 emulator port skeleton
 *
 * Boot entry point. Initializes Wii video/console via libogc, mounts
 * SD/USB via libfat, loads a PS2 BIOS image, and hands off to the EE
 * interpreter core. This is NOT a functional PS2 emulator - see
 * docs/STATUS.md for an honest description of what actually works.
 *
 * REAL BOOT FLOW IS NOW THE DEFAULT (task #126, "main.c von Demo auf
 * echten Boot-Flow umstellen"): as of this round, `main()` no longer
 * starts by showing a menu and waiting for the user to opt into a
 * "BIOS Boot Test" - it immediately calls `run_real_boot_flow()`,
 * which mounts storage, loads the real BIOS, and runs the actual
 * EE/IOP interleaved scheduler continuously (in bounded, screen-
 * refreshing chunks so the UI stays responsive and provably alive -
 * see draw_boot_progress_hud()/draw_heartbeat()). Every chunk checks
 * the REAL GS privileged registers (via gs_get_state()); the moment
 * PMODE indicates an active display circuit, it checks WHICH circuit
 * (EN1 vs EN2 - fixed Round 212/task #366, see the fix's own comment
 * further down in this file for why this matters: a real PCSX2
 * session at the real BIOS splash used Circuit 2, not Circuit 1) and
 * decodes that circuit's REAL DISPFB hardware fields (FBP in
 * 2048-word units, FBW in 64-pixel units - converted to this
 * project's own gs_mem.h word/pixel convention, per that header's own
 * note that this conversion is the caller's job) and blits the REAL
 * GS local memory content the BIOS/game itself configured - not a
 * canned test pattern. As of this round (see docs/STATUS.md's "Round
 * 29 continued" sections), GS registers stay at their power-on-zero
 * state through the traced boot window, so this path is not yet
 * exercised in practice - but it is real, correct scaffolding for
 * whenever GS setup does occur, rather than a synthetic substitute.
 *
 * NATIVE WII TEST MENU: everything below the "wii_console_setup"
 * helper and above "int main" is a small, self-contained, native Wii
 * UI drawn directly into the XFB (reusing gs_wii_output.c's already-
 * tested RGB->YCbCr conversion) - it is NOT part of PS2 emulation and
 * does not pretend to be. It remains available AFTER the automatic
 * real boot flow finishes/is stopped (press B to interrupt it early),
 * as a secondary diagnostic surface (re-run the boot flow, the fixed-
 * pattern GS/GIF pipeline demo, or an About screen) - it is no longer
 * the primary/gating path to actually running the emulator core.
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
#include "core/hw/iop_cdvd.h"
#include "core/hw/iop_cdrom_legacy.h"
#include "core/hw/gs.h"
#include "core/hw/gs_mem.h"
#include "core/hw/gs_wii_output.h"
#include "core/hw/dma.h"
#include "core/hw/gif.h"
#include "core/hw/iop_sio2.h"

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

/* Round 272 (task #423, 313th finding): translate the real Wii
 * GameCube-style controller's held-button mask (libogc's real
 * PAD_BUTTON_ / PAD_TRIGGER_ bits, from PAD_ButtonsHeld()) into a
 * real PS2 digital-pad pressed-mask (IOP_PAD_BTN_*, psx-spx-cited,
 * already defined in iop_sio2.h) and feed it to this project's own
 * already-implemented, real host-side pad API
 * (iop_sio2_pad_set_buttons(), Round 184/195).
 *
 * This is a port-level control-mapping DESIGN CHOICE, not a claim
 * about real PS2 hardware behavior - there is no "real" Wii-to-PS2
 * button mapping to cite, since the Wii never shipped a PS2 pad
 * pass-through. The mapping below follows the common-sense
 * face-button correspondence used by most emulators/ports that
 * support both pads (Wii A/B/X/Y sit in the same physical ring
 * position as PS2 Cross/Circle/Square/Triangle when both pads are
 * held the same way), and is documented here explicitly as a design
 * choice so it is never confused with a cited hardware fact:
 *   Wii A      -> PS2 Cross    (both are the primary "confirm" button)
 *   Wii B      -> PS2 Circle
 *   Wii X      -> PS2 Square
 *   Wii Y      -> PS2 Triangle
 *   Wii Start  -> PS2 Start
 *   Wii D-pad  -> PS2 D-pad (direct correspondence)
 *   Wii L/R    -> PS2 L1/R1
 *   Wii Z      -> PS2 Select
 *
 * Round 272 also tested (host-native scratch diagnostic, not shipped
 * here) whether simply holding Cross from boot start unblocks
 * OSDSYS's idle loop or triggers any CD-ROM auto-boot activity - it
 * does not (0 SIO2 register writes either way, identical trace to the
 * no-press baseline - see docs/STATUS.md's 313th finding for the
 * full account). This wiring is shipped anyway because it is a real,
 * correct, useful feature for actual interactive use on real Wii
 * hardware once further boot progress is made - not because it was
 * found to unblock anything on its own. */
static uint16_t wii_pad_to_ps2_pad(uint16_t wii_held)
{
    uint16_t ps2 = 0;
    if (wii_held & PAD_BUTTON_A)     ps2 |= IOP_PAD_BTN_CROSS;
    if (wii_held & PAD_BUTTON_B)     ps2 |= IOP_PAD_BTN_CIRCLE;
    if (wii_held & PAD_BUTTON_X)     ps2 |= IOP_PAD_BTN_SQUARE;
    if (wii_held & PAD_BUTTON_Y)     ps2 |= IOP_PAD_BTN_TRIANGLE;
    if (wii_held & PAD_BUTTON_START) ps2 |= IOP_PAD_BTN_START;
    if (wii_held & PAD_BUTTON_UP)    ps2 |= IOP_PAD_BTN_UP;
    if (wii_held & PAD_BUTTON_DOWN)  ps2 |= IOP_PAD_BTN_DOWN;
    if (wii_held & PAD_BUTTON_LEFT)  ps2 |= IOP_PAD_BTN_LEFT;
    if (wii_held & PAD_BUTTON_RIGHT) ps2 |= IOP_PAD_BTN_RIGHT;
    if (wii_held & PAD_TRIGGER_L)    ps2 |= IOP_PAD_BTN_L1;
    if (wii_held & PAD_TRIGGER_R)    ps2 |= IOP_PAD_BTN_R1;
    if (wii_held & PAD_TRIGGER_Z)    ps2 |= IOP_PAD_BTN_SELECT;
    return ps2;
}

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
    "Re-run Boot Flow",
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

/* Round 209 (task 366/371): real disc-image mounting for the ACTUAL
 * persistent boot flow, not just throwaway host-native diagnostics.
 * Rounds 170/207 already proved iso_loader.c/iop_cdvd_mount_iso()/
 * iop_cdrom_legacy_mount_iso() correctly parse a real PS2 disc image
 * and serve real sector data - but until this round, main.c never
 * called either mount function at all, so the actual Wii build had
 * no disc to read even if real BIOS/EELOAD code tried to read one.
 * Mirrors the existing BIOS search-path convention: sd:/pcsx2/games/,
 * checked once. A missing disc is NOT a hard error - real PS2
 * hardware also boots fine with no disc inserted (it shows the
 * browser/opening screen instead of a disc-boot fast path); only a
 * real BIOS is mandatory to proceed at all. */
static int   g_disc_checked = 0;
static int   g_disc_ok = 0;

/* Round 29 continued (task #126): real BIOS boot as the PRIMARY,
 * automatic action - see the top-of-file header comment for the full
 * rationale. Runs the actual EE/IOP interleaved scheduler in bounded
 * chunks (so the UI keeps redrawing/responding every ~200k IOP
 * instructions instead of blocking for tens of millions at once),
 * checks the REAL GS privileged registers each chunk, and blits the
 * REAL GS local memory content the instant a display gets configured
 * - not a canned test pattern. Bounded by a generous but finite
 * safety cap (not "a real limit" - this project's own diagnostics
 * have run well past it - just a guard against a truly-never-ending
 * loop with no way out other than power-cycling); the user can also
 * hold B at any time to stop early and drop into the secondary test
 * menu below. */
#define BOOT_CHUNK_SLICES 200000ull  /* IOP-instruction budget per redraw, keeps the UI responsive */
#define BOOT_TOTAL_CAP    2000000000ull /* generous overall safety cap, not a real limit - see comment above */

/* Round 119 (task #172/#274): this used to be a static function
 * defined right here. It was moved into gs_wii_output.c/.h
 * (gs_decode_dispfb) so it could actually be unit-tested host-
 * natively - everything in this file depends on <gccore.h> (the real
 * Wii SDK), which isn't available in this project's host-native test
 * environment, so nothing defined in main.c has ever been directly
 * testable. The logic is byte-for-byte unchanged, only the name/
 * location changed - see gs_wii_output.h's doc comment for the real
 * PS2 GS DISPFB1/DISPFB2 register field layout this implements. */
#define decode_dispfb gs_decode_dispfb

/* Live progress HUD for the automatic real boot flow - redrawn every
 * chunk so the user can see real instruction counts advancing (proof
 * the emulator core is genuinely executing, not frozen), whether each
 * core has halted (and why), and whether the BIOS/game has configured
 * a real GS display yet. */
static void draw_boot_progress_hud(uint64_t ee_instr, uint64_t iop_instr,
                                    int ee_halted, int iop_halted,
                                    const char *ee_reason, const char *iop_reason,
                                    int display_active)
{
    goto_rc(16, 40);
    printf("PCSX2-Wii - Real BIOS Boot                                        \n");
    printf("===================================                              \n\n");
    printf("EE  instructions executed: %-16llu halted=%d              \n",
           (unsigned long long)ee_instr, ee_halted);
    printf("    %-64s\n", ee_halted ? ee_reason : "(still executing real instructions)");
    printf("IOP instructions executed: %-16llu halted=%d              \n",
           (unsigned long long)iop_instr, iop_halted);
    printf("    %-64s\n", iop_halted ? iop_reason : "(still executing real instructions)");
    printf("\n");
    printf("GS display: %-58s\n",
           display_active ? "configured by BIOS/game - showing real GS memory below"
                           : "not configured yet (see docs/STATUS.md's Round 29 notes)");
    printf("\nHold B to stop and open the test menu (re-run, GS/GIF demo, about).\n");
}

/* The real boot flow itself - see the top-of-file header comment and
 * the doc comment above draw_boot_progress_hud() for the full design.
 * Runs automatically once at startup (called from main()) and is also
 * reachable again from the test menu ("Re-run Boot Flow"). */
static void run_real_boot_flow(void)
{
    draw_gradient_background(6, 6, 22, 0, 0, 6);
    flush_screen();
    goto_rc(16, 40);
    printf("Booting real PS2 BIOS...\n");

    if (!g_fat_mounted)
        g_fat_mounted = fatInitDefault() ? 1 : 0;
    if (!g_fat_mounted) {
        printf("\n[!] fatInitDefault() failed - no SD/USB storage found.\n");
        printf("    Insert an SD card with /pcsx2/bios/*.bin and restart to\n");
        printf("    boot a real BIOS. Press A or B to open the test menu.\n");
        wait_for_button(PAD_BUTTON_A | PAD_BUTTON_B);
        return;
    }

    if (!g_bios_ok) {
        memset(&g_bios, 0, sizeof(g_bios));
        g_bios_ok = (bios_load("sd:/pcsx2/bios/SCPH39001.bin", &g_bios) == 0 ||
                     bios_load("sd:/pcsx2/bios/SCPH10000.bin", &g_bios) == 0 ||
                     bios_load("sd:/pcsx2/bios/bios.bin", &g_bios) == 0);
    }
    if (!g_bios_ok) {
        printf("\n[!] Could not load a PS2 BIOS image from sd:/pcsx2/bios/\n");
        printf("    Place a legally-dumped PS2 BIOS there (e.g. bios.bin) and\n");
        printf("    restart. Press A or B to open the test menu.\n");
        wait_for_button(PAD_BUTTON_A | PAD_BUTTON_B);
        return;
    }

    printf("[+] BIOS loaded: %s  size=%u  rom_ver=%s\n",
           g_bios.name, (unsigned)g_bios.size, g_bios.version_string);

    /* Round 209: real disc mount, once, best-effort. Mounted on BOTH
     * real register interfaces since it is not yet established which
     * one (if either) real BIOS/kernel code actually uses to read
     * SYSTEM.CNF (Round 205-207) - giving the real boot every real
     * chance to succeed regardless of which path it tries. Missing
     * disc is logged, not fatal - matches real PS2 boot-with-no-disc
     * behavior. */
    if (!g_disc_checked) {
        g_disc_checked = 1;
        int cdvd_rc   = iop_cdvd_mount_iso("sd:/pcsx2/games/game.bin");
        if (cdvd_rc != 0)
            cdvd_rc = iop_cdvd_mount_iso("sd:/pcsx2/games/game.iso");
        int legacy_rc = iop_cdrom_legacy_mount_iso("sd:/pcsx2/games/game.bin");
        if (legacy_rc != 0)
            legacy_rc = iop_cdrom_legacy_mount_iso("sd:/pcsx2/games/game.iso");
        g_disc_ok = (cdvd_rc == 0) || (legacy_rc == 0);
        if (g_disc_ok) {
            if (cdvd_rc == 0)
                iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD, Round 170's cited constant */);
            printf("[+] Disc image mounted: sd:/pcsx2/games/game.bin (or .iso)\n");
        } else {
            printf("[i] No disc image found at sd:/pcsx2/games/ - booting BIOS only\n");
            printf("    (place a real PS2 disc dump there as game.bin/game.iso\n");
            printf("    for a real disc-boot attempt; real hardware also boots\n");
            printf("    fine with no disc, just without a game to run).\n");
        }
    }

    if (!g_system_started) {
        system_init(&g_bios, &g_bios);
        g_system_started = 1;
        gs_init();
        gs_mem_init();
    }

    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    gs_state_t  *gs  = gs_get_state();

    uint32_t frame = 0;
    uint64_t total_slices = 0;
    int stopped_by_user = 0;

    iop_sio2_pad_connect();

    for (;;) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        uint16_t held = PAD_ButtonsHeld(0);
        iop_sio2_pad_set_buttons(wii_pad_to_ps2_pad(held));

        if (!(ee->halted && iop->halted)) {
            system_run_interleaved(BOOT_CHUNK_SLICES);
            total_slices += BOOT_CHUNK_SLICES;
        }

        /* PMODE bits 0/1 = EN1/EN2 (circuit 1/2 enabled) - real,
         * documented GS register semantics, not a guess.
         *
         * Round 212 fix (task #366/#172, 252nd finding): this used to
         * always call decode_dispfb(gs->dispfb1, ...) regardless of
         * which circuit PMODE actually enabled. A real PCSX2 debugger
         * session at the real BIOS splash screen (user-provided
         * screenshots) showed PMODE=0x66 - EN1=0, EN2=1 - with
         * DISPFB2/DISPLAY2 populated with real, structured values
         * (DISPFB2's FBW field decodes to 640px, a genuine PS2
         * resolution) while DISPFB1/DISPLAY1 stayed exactly zero, the
         * same "DISPLAY1 never written" symptom this project has
         * chased since the 94th/126th/223rd findings. That symptom is
         * consistent with Circuit 1 legitimately never being used by
         * this BIOS at all - Circuit 2 is what actually drives the
         * picture. The old code's `display_active` check already
         * correctly went true on EN2 alone (mask 0x3 covers both
         * bits), but the blit itself was hardcoded to Circuit 1's
         * dispfb, so even a real, hardware-accurate GS setup writing
         * only Circuit 2 would have silently produced no picture here.
         * Fixed by choosing the circuit to blit from based on which
         * EN bit is actually set (EN1 preferred if both are somehow
         * set, matching real hardware's Circuit-1-is-primary
         * convention; EN2 used otherwise) instead of assuming
         * Circuit 1. gs_state_t/gs.c already modeled dispfb2/display2
         * correctly at their real addresses (0x12000090/0x120000A0) -
         * only this call site needed the fix. */
        int en1 = (gs->pmode & 0x1u) != 0;
        int en2 = (gs->pmode & 0x2u) != 0;
        int display_active = en1 || en2;
        if (display_active) {
            uint64_t active_dispfb = en1 ? gs->dispfb1 : gs->dispfb2;
            uint32_t bp_words, bw_pixels;
            decode_dispfb(active_dispfb, &bp_words, &bw_pixels);
            if (bw_pixels > 0) {
                uint32_t blit_h = rmode->xfbHeight > 140 ? rmode->xfbHeight - 140 : 0;
                gs_blit_psmct32_to_xfb(xfb, rmode->fbWidth, 0, 140,
                                       bp_words, bw_pixels, 0, 0,
                                       rmode->fbWidth, blit_h);
                DCFlushRange((uint8_t *)xfb + (size_t)140 * rmode->fbWidth * VI_DISPLAY_PIX_SZ,
                             rmode->fbWidth * blit_h * VI_DISPLAY_PIX_SZ);
            }
        }

        draw_boot_progress_hud(ee->instructions_executed, iop->instructions_executed,
                                ee->halted, iop->halted, ee->halt_reason, iop->halt_reason,
                                display_active);
        draw_heartbeat(frame++);

        if (held & PAD_BUTTON_B) { stopped_by_user = 1; break; }
        if (ee->halted && iop->halted) break;
        if (total_slices >= BOOT_TOTAL_CAP) break;
    }

    goto_rc(16, 400);
    if (stopped_by_user)
        printf("Stopped by user (B held).                                          \n");
    else if (ee->halted && iop->halted)
        printf("Both cores halted on their own - see reasons above.                \n");
    else
        printf("Reached the safety instruction cap - still executing real code.    \n");
    printf("\nPress A or B to open the test menu.\n");
    wait_for_button(PAD_BUTTON_A | PAD_BUTTON_B);
}

/* Menu entry point wrapper - "Re-run Boot Flow" just re-enters the
 * same real boot flow above (harmless if it already ran: system_init/
 * gs_init are only called once via the g_system_started guard, so
 * this resumes/re-displays the SAME already-running cores rather than
 * restarting them). */
static void action_bios_boot_test(void)
{
    run_real_boot_flow();
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
    printf("This is NOT a functional PS2 emulator yet - a real BIOS does\n");
    printf("boot for real (the automatic boot flow you just saw runs the\n");
    printf("actual EE/IOP interpreters against it), but it does not yet\n");
    printf("reach the OSD splash screen. See docs/STATUS.md's \"Round 29\"\n");
    printf("sections for the current, honest state of that investigation.\n");
    printf("docs/ROADMAP.md has the full account of what actually works\n");
    printf("today (EE/IOP interpreters, DMA/GIF/GS register plumbing, a\n");
    printf("real IOP module/IRX loader, a real VU opcode table, an SPU2\n");
    printf("register scaffold) and what is still missing.\n\n");
    printf("Press A or B to return to the menu.\n");
    wait_for_button(PAD_BUTTON_A | PAD_BUTTON_B);
}

int main(int argc, char **argv)
{
    wii_console_setup();

    /* Task #126: the real BIOS boot flow is now the automatic,
     * primary action - runs immediately, before any menu is shown.
     * See the top-of-file header comment for the full rationale. The
     * test menu below remains available afterward (or immediately, if
     * the user holds B to stop the boot flow early) as a secondary
     * diagnostic surface. */
    run_real_boot_flow();

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
