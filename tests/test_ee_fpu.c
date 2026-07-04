#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_lui(int rt, uint16_t imm) { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_mtc1(int rt, int fs) { return (0x11 << 26) | (0x04 << 21) | (rt << 16) | (fs << 11); }
static uint32_t enc_mfc1(int rt, int fs) { return (0x11 << 26) | (0x00 << 21) | (rt << 16) | (fs << 11); }
static uint32_t enc_cop1_s(int funct, int fd, int fs, int ft) {
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
static uint32_t enc_break(void) { return 0x0D; }
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

    /* Build float 2.0f (0x40000000) into r1, MTC1 into f0.
     * Build float 3.0f (0x40400000) into r2, MTC1 into f1. */
    wle32(p+pc, enc_lui(1, 0x4000)); pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0x0000)); pc += 4;
    wle32(p+pc, enc_mtc1(1, 0)); pc += 4;              /* f0 = 2.0 */

    wle32(p+pc, enc_lui(2, 0x4040)); pc += 4;
    wle32(p+pc, enc_ori(2, 2, 0x0000)); pc += 4;
    wle32(p+pc, enc_mtc1(2, 1)); pc += 4;              /* f1 = 3.0 */

    wle32(p+pc, enc_cop1_s(0x00, 2, 0, 1)); pc += 4;   /* f2 = ADD.S f0,f1 = 5.0 */
    wle32(p+pc, enc_cop1_s(0x02, 3, 1, 0)); pc += 4;   /* f3 = MUL.S f1,f0 = 6.0 */
    wle32(p+pc, enc_cop1_s(0x01, 4, 1, 0)); pc += 4;   /* f4 = SUB.S f1,f0 = 1.0 */
    wle32(p+pc, enc_cop1_s(0x03, 5, 1, 0)); pc += 4;   /* f5 = DIV.S f1,f0 = 1.5 */
    wle32(p+pc, enc_cop1_s(0x07, 6, 0, 0)); pc += 4;   /* f6 = NEG.S f0 = -2.0 */
    wle32(p+pc, enc_cop1_s(0x05, 7, 6, 0)); pc += 4;   /* f7 = ABS.S f6 = 2.0 */
    wle32(p+pc, enc_cop1_s(0x32, 0, 0, 7)); pc += 4;   /* C.EQ.S f0,f7 -> true (both 2.0) */

    wle32(p+pc, enc_mfc1(10, 2)); pc += 4;
    wle32(p+pc, enc_mfc1(11, 3)); pc += 4;
    wle32(p+pc, enc_mfc1(12, 4)); pc += 4;
    wle32(p+pc, enc_mfc1(13, 5)); pc += 4;
    wle32(p+pc, enc_mfc1(14, 7)); pc += 4;

    wle32(p+pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_core_run(&bios);
    ee_state_t *st = ee_core_get_state();

    float f10, f11, f12, f13, f14;
    uint32_t b;
    b = (uint32_t)st->gpr[10].ud0; memcpy(&f10, &b, 4);
    b = (uint32_t)st->gpr[11].ud0; memcpy(&f11, &b, 4);
    b = (uint32_t)st->gpr[12].ud0; memcpy(&f12, &b, 4);
    b = (uint32_t)st->gpr[13].ud0; memcpy(&f13, &b, 4);
    b = (uint32_t)st->gpr[14].ud0; memcpy(&f14, &b, 4);

    CHECK(f10 == 5.0f, "ADD.S: 2.0 + 3.0 = 5.0");
    CHECK(f11 == 6.0f, "MUL.S: 3.0 * 2.0 = 6.0");
    CHECK(f12 == 1.0f, "SUB.S: 3.0 - 2.0 = 1.0");
    CHECK(f13 == 1.5f, "DIV.S: 3.0 / 2.0 = 1.5");
    CHECK(f14 == 2.0f, "NEG.S+ABS.S: -(-2.0) via abs = 2.0");
    CHECK((st->fcr31 & 0x00800000u) != 0, "C.EQ.S set the FCR31 condition bit (2.0 == 2.0)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
