/*
 * pcsx2-wii diagnostic ELF (Round 507)
 * Boots under real PCSX2 via System > Start File, dumps real BIOS/kernel
 * ground truth to the on-screen debug console (scr_printf) one screen at
 * a time (paced with sleep()s) so it can be read via manual screenshots
 * without relying on host: log files or horizontal scrolling.
 *
 * Sections: rom0:ROMVER, rom0: directory listing (opendir/readdir), mc0:/mc1:
 * directory listing, sceCdGetDiskType/sceCdStatus.
 *
 * Uses POSIX file calls (open/read/close/opendir/readdir) as required by
 * this ps2sdk newlib port - direct fio-family and fileXio-family calls
 * are blocked by io_common.h's #error in this SDK version.
 */
#include <stdio.h>
#include <string.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <debug.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <libcdvd.h>

static void pause_screen(int secs)
{
    scr_printf("\n[ next screen in %d s ]\n", secs);
    sleep(secs);
    scr_clear();
    scr_setXY(0, 0);
}

static void dump_rom0_file(const char *path, const char *label)
{
    int fd;
    char buf[256];
    int n;

    scr_printf("%s (%s):\n", label, path);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        scr_printf("  open failed: %d\n", fd);
        return;
    }
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        scr_printf("  read failed: %d\n", n);
        return;
    }
    buf[n] = 0;
    scr_printf("  \"%s\"\n  (len=%d)\n", buf, n);
}

static void dump_dir(const char *path)
{
    DIR *d;
    struct dirent *de;
    int count = 0;

    scr_printf("Directory listing: %s\n", path);
    d = opendir(path);
    if (!d) {
        scr_printf("  opendir failed\n");
        return;
    }
    while ((de = readdir(d)) != NULL) {
        scr_printf("  %-24s type=%d\n", de->d_name, de->d_type);
        count++;
        if (count >= 14) {
            scr_printf("  ... (truncated at 14 entries)\n");
            break;
        }
    }
    closedir(d);
    scr_printf("  (%d entries shown)\n", count);
}

int main(int argc, char *argv[])
{
    int disktype, status;

    sceSifInitRpc(0);
    init_scr();
    scr_clear();
    scr_setXY(0, 0);

    scr_printf("pcsx2-wii Round 507 diagnostic ELF\n");
    scr_printf("argc=%d\n", argc);
    if (argc > 0 && argv && argv[0])
        scr_printf("argv[0]=\"%s\"\n", argv[0]);
    pause_screen(6);

    dump_rom0_file("rom0:ROMVER", "BIOS ROMVER");
    pause_screen(6);

    dump_dir("rom0:");
    pause_screen(6);

    dump_dir("mc0:/");
    pause_screen(4);

    dump_dir("mc1:/");
    pause_screen(4);

    disktype = sceCdGetDiskType();
    status = sceCdStatus();
    scr_printf("sceCdGetDiskType() = %d (0x%x)\n", disktype, disktype);
    scr_printf("sceCdStatus()      = %d (0x%x)\n", status, status);
    pause_screen(8);

    scr_printf("Diagnostic ELF complete. Looping.\n");
    while (1) {
        sleep(30);
    }

    return 0;
}
