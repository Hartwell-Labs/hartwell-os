/* Hartwell OS 1.0 — appliance runtime.
 *
 * Modes:
 *   hwo id            print this device's id
 *   hwo license       exit 0 if the device is licensed, 1 otherwise
 *   hwo ui            full-screen appliance UI on /dev/tty0 (never exits)
 *   hwo vcsa FILE     dump the real text console (/dev/vcsa) as hex
 *
 * No libc dependency on the OS itself beyond the syscalls busybox provides;
 * compiled static so it runs on any x86 Linux.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/kd.h>

#include "hwo.h"
#include "errors.h"
#include "monocypher.h"

/* Vendor EdDSA public key, injected by build.sh from keys/hwo-lic.pub. When a
 * licence file carries a `licence=<hex>` signature over this unit's id, that
 * signature is authoritative: it can be produced only with the vendor seed, so
 * it cannot be copied to another unit or forged from the public binary. The
 * typed HWO-YYYY-XXXX key remains as an offline serial for demos. */
#ifndef HWO_LIC_PUB_HEX
#define HWO_LIC_PUB_HEX ""
#endif

#define SECRET   "hartwell-os-v1-7f3a91c2"
#define KEY_PFX  "HWO-"

static int hex_nib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Verify `sighex` (128 hex chars) over `device` with the embedded vendor key. */
static int licence_sig_ok(const char *sighex, const char *device)
{
    static const char *pubhex = HWO_LIC_PUB_HEX;
    if (!pubhex[0] || strlen(pubhex) != 64) return 0;      /* no key embedded */
    if (!sighex || strlen(sighex) != 128) return 0;
    unsigned char pk[32], sig[64];
    for (int i = 0; i < 32; i++) {
        int hi = hex_nib(pubhex[2 * i]), lo = hex_nib(pubhex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        pk[i] = (unsigned char)((hi << 4) | lo);
    }
    for (int i = 0; i < 64; i++) {
        int hi = hex_nib(sighex[2 * i]), lo = hex_nib(sighex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        sig[i] = (unsigned char)((hi << 4) | lo);
    }
    return crypto_eddsa_check(sig, pk, (const unsigned char *)device, strlen(device)) == 0;
}

/* ---------- helpers ---------- */

static int read_file(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r' || buf[r - 1] == ' '))
        buf[--r] = 0;
    return 0;
}

static unsigned fnv1a(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static const char B32[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

/* 4 characters derived from device id + build secret. Deterministic, offline. */
static void sign4(const char *device, char out[5])
{
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s|%s", device, SECRET);
    unsigned h = fnv1a(tmp);
    for (int i = 0; i < 4; i++) {
        out[i] = B32[(h >> (5 * (3 - i))) & 31];
    }
    out[4] = 0;
}

static void device_id(char out[32])
{
    char uuid[128] = "";
    if (read_file("/sys/class/dmi/id/product_uuid", uuid, sizeof uuid) != 0 || !uuid[0])
        if (read_file("/etc/machine-id", uuid, sizeof uuid) != 0 || !uuid[0])
            strcpy(uuid, "unknown-device");
    unsigned h = fnv1a(uuid);
    snprintf(out, 32, "HWO-%04X-%04X", (h >> 16) & 0xFFFF, h & 0xFFFF);
}

/* key format HWO-YYYY-XXXX, suffix must match this device */
/* A person types this once, usually from an invoice: accept the key with or
 * without the dashes, in lower or upper case, and compare it canonically.
 * Rejecting "hwo 2026 v8ns" for the shape of the input, not its value, is a
 * support ticket that costs a sale. */
static int key_ok(const char *raw, const char *dev)
{
    char k[12];
    int n = 0;
    for (const char *p = raw; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '-' || c == ' ' || c == '_') continue;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        if (n >= 11) return 0;
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return 0;
        k[n++] = (char)c;
    }
    if (n != 11) return 0;
    if (strncmp(k, "HWO", 3) != 0) return 0;
    for (int i = 3; i < 7; i++)
        if (k[i] < '0' || k[i] > '9') return 0;
    char want[5];
    sign4(dev, want);
    k[11] = 0;
    return strcmp(k + 7, want) == 0;
}

/* license file: key=HWO-2026-XXXX  device=HWO-AAAA-BBBB */
/* a licence typed on the device is written to the HWO_CFG state partition when one is
 * mounted, so it survives reboot; the initramfs copy is then only a cache */
#define STATE_DIR "/state"

static int read_license(const char *path, char *buf, size_t n)
{
    return read_file(path, buf, n);
}

static int licensed(char *key_out, size_t key_n)
{
    char buf[512];
    if (read_license("/etc/hartwell/license", buf, sizeof buf) != 0 &&
        read_license(STATE_DIR "/license", buf, sizeof buf) != 0)
        return 0;

    char key[64] = "", dev[64] = "", sig[160] = "";
    char *p = strstr(buf, "key=");
    if (p) sscanf(p + 4, "%63s", key);
    p = strstr(buf, "device=");
    if (p) sscanf(p + 7, "%63s", dev);
    p = strstr(buf, "licence=");
    if (p) sscanf(p + 8, "%159s", sig);

    char mine[32];
    device_id(mine);
    static int noted;                 /* one line per reason per process */
    if (dev[0] && strcmp(dev, mine) != 0) {
        if (noted != 1) { noted = 1; hwo_note('E', "E2002", "licence is for %s, this unit is %s", dev, mine); }
        return 0;
    }

    /* signed licence: authoritative, unforgeable without the vendor seed */
    if (sig[0]) {
        if (!licence_sig_ok(sig, mine)) {
            if (noted != 2) { noted = 2; hwo_note('E', "E2002", "signature does not verify for %s", mine); }
            return 0;
        }
        if (key_out) snprintf(key_out, key_n, "signed");
        return 1;
    }

    /* typed serial key: offline convenience, bound to this unit's id */
    if (!key[0]) return 0;
    if (!key_ok(key, mine)) {
        if (noted != 3) { noted = 3; hwo_note('E', "E2003", "key %s rejected for %s", key, mine); }
        return 0;
    }
    if (key_out) snprintf(key_out, key_n, "%s", key);
    return 1;
}

/* ---------- UI ---------- */

static int tty = -1;

static void put(const char *s) { write(tty, s, strlen(s)); }

static void at(int row, int col, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    char out[320];
    snprintf(out, sizeof out, "\033[%d;%dH%s", row, col, line);
    write(tty, out, strlen(out));
}

static void status_line(void)
{
    char fleet[64] = "offline", since[64] = "0";
    read_file("/run/fleet.status", fleet, sizeof fleet);
    read_file("/run/fleet.since", since, sizeof since);

    char up[64] = "0";
    char buf[128];
    if (read_file("/proc/uptime", buf, sizeof buf) == 0) {
        double s = atof(buf);
        int h = (int)(s / 3600), m = ((int)s % 3600) / 60, sec = (int)s % 60;
        snprintf(up, sizeof up, "%02d:%02d:%02d", h, m, sec);
    }
    char key[64] = "";
    int lic = licensed(key, sizeof key);

    at(24, 2, "\033[0;32m%s\033[0m", lic ? "LICENSED" : "UNLICENSED");
    at(24, 13, "\033[0;36mfleet:\033[0m %-8s", fleet);
    at(24, 31, "\033[0;36muptime:\033[0m %s", up);
    at(24, 48, "\033[0;90m%s\033[0m", lic ? key : "no key on device");
}

static void frame(void)
{
    put("\033[2J");

    /* header bar */
    put("\033[1;1H\033[42;30m");
    char head[81];
    memset(head, ' ', 80);
    head[80] = 0;
    const char *name = " HARTWELL OS 1.0 ";
    memcpy(head + 2, name, strlen(name));
    char right[48];
    char dev[32];
    device_id(dev);
    snprintf(right, sizeof right, " device %s ", dev);
    memcpy(head + 80 - strlen(right), right, strlen(right));
    write(tty, head, 80);
    put("\033[0m");

    /* content panel: filled block, black on light grey */
    for (int r = 6; r <= 17; r++)
        at(r, 4, "\033[30;47m%-73s\033[0m", "");

    char body[2048] = "";
    read_file("/etc/hartwell/content.txt", body, sizeof body);

    int row = 8;
    char *save = NULL;
    for (char *l = strtok_r(body, "\n", &save); l && row < 17; l = strtok_r(NULL, "\n", &save)) {
        if (row == 8) {
            /* title: centred */
            int len = (int)strlen(l);
            int col = (4 + 76 - len) / 2;
            if (col < 6) col = 6;
            at(row, col, "\033[30;47m%s\033[0m", l);
            row += 2;
        } else {
            at(row, 8, "\033[0;30;47m%-60s\033[0m", l);
            row++;
        }
    }

    /* rule + footer */
    at(21, 2, "\033[0;90m%.76s\033[0m",
       "--------------------------------------------------------------------------");
    status_line();
    at(25, 2, "\033[0;90mno shell - no package manager - no remote login\033[0m");
}

/* unlicensed screen: nothing else runs */
static void license_screen(void)
{
    char dev[32];
    device_id(dev);
    put("\033[2J");
    put("\033[1;1H\033[41;97m");
    char head[81];
    memset(head, ' ', 80);
    head[80] = 0;
    const char *t = " HARTWELL OS 1.0 - NOT LICENSED ";
    memcpy(head + 2, t, strlen(t));
    write(tty, head, 80);
    put("\033[0m");

    at(5, 4, "\033[1;37mThis machine has no valid licence.\033[0m");
    at(7, 4, "Device id:  \033[1;33m%s\033[0m", dev);
    at(8, 4, "Send this id to business@hartwell-labs.pl");
    at(9, 4, "You will receive a key in the form HWO-YYYY-XXXX");
    at(11, 4, "\033[0;90mInstall it as /etc/hartwell/license and reboot:\033[0m");
    at(12, 6, "key=HWO-2026-ABCD");
    at(13, 6, "device=%s", dev);

    at(17, 4, "\033[0;90mNo shell, no network tools, no way past this screen.\033[0m");
    at(23, 4, "\033[0;32m149 EUR per device, perpetual\033[0m");
    at(24, 4, "\033[0;90mhartwell-labs.pl\033[0m");
    fflush(NULL);
}

static void ui_loop(void)
{
    if (fbui_run() == 0) return;      /* framebuffer UI ran and exited */
    tty = open("/dev/tty0", O_WRONLY);
    if (tty < 0) tty = open("/dev/console", O_WRONLY);
    if (tty < 0) {
        fprintf(stderr, "hwo: no console device\n");
        for (;;) sleep(60);
    }
    put("\033[?25l");   /* hide cursor */
    for (;;) {
        if (licensed(NULL, 0)) frame();
        else                   license_screen();
        sleep(2);
    }
}

/* ---------- exported for fbui ---------- */

void hwo_device_id(char out[32]) { device_id(out); }

int hwo_licensed(char *key_out, unsigned long key_n) { return licensed(key_out, key_n); }

int hwo_read(const char *path, char *buf, unsigned long n) { return read_file(path, buf, n); }

int hwo_key_valid(const char *key)
{
    char dev[32];
    device_id(dev);
    return key_ok(key, dev);
}

int hwo_save_key(const char *key)
{
    char dev[32], tmp[160];
    device_id(dev);
    mkdir("/etc/hartwell", 0755);
    int fd = open("/etc/hartwell/license", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int n = snprintf(tmp, sizeof tmp, "key=%s\ndevice=%s\n", key, dev);
    ssize_t w = write(fd, tmp, (size_t)n);
    fsync(fd);
    close(fd);
    if (w != n) return -1;

    /* persist on the state partition, if the machine has one */
    if (hwo_state_present()) {
        int sd = open(STATE_DIR "/license", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sd >= 0) {
            ssize_t sw = write(sd, tmp, (size_t)n);
            fsync(sd);
            close(sd);
            if (sw != n) return -1;
        }
        sync();
    }
    return 0;
}

/* 1 = the licence lives on a state partition and survives reboot */
int hwo_state_persistent(void)
{
    return hwo_state_present() && access(STATE_DIR "/license", R_OK) == 0;
}

int hwo_state_present(void)
{
    struct stat a, b;
    if (stat(STATE_DIR, &a) != 0 || stat("/", &b) != 0) return 0;
    return a.st_dev != b.st_dev;   /* a real mount, not just a directory */
}

/* ---------- vcsa dump (proof of what is on screen) ---------- */

static int dump_vcsa(const char *path)
{
    static const char *paths[] = { "/dev/vcsa", "/dev/vcsa0", "/dev/vcsa1", "/dev/vcsa2" };
    int fd = -1;
    for (unsigned i = 0; i < sizeof paths / sizeof *paths && fd < 0; i++)
        fd = open(paths[i], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "hwo: no text console (vcsa) found\n");
        return 1;
    }
    unsigned char hdr[4];
    if (read(fd, hdr, 4) != 4) { close(fd); return 1; }
    int rows = hdr[0], cols = hdr[1];
    size_t n = (size_t)rows * cols * 2;
    unsigned char *cells = malloc(n);
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, cells + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);

    FILE *f = fopen(path, "w");
    if (!f) { free(cells); return 1; }
    fprintf(f, "%d %d\n", rows, cols);
    for (size_t i = 0; i < got; i += 2) {
        fprintf(f, "%02x%02x", cells[i + 1], cells[i]);  /* attr, char */
        if (((i / 2) + 1) % cols == 0) fputc('\n', f);
    }
    fclose(f);
    free(cells);
    return 0;
}

int hwo_update(const char *bundle, int force);   /* src/update.c */
int hwo_boot_confirm(void);                      /* src/update.c */
int hwo_boot_rollback(void);                     /* src/update.c */

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: hwo id|license|ui|errs|err CODE message|fbprobe|vcsa FILE"
                        "|update BUNDLE [--force]|bootconfirm|rollback\n");
        return 2;
    }
    if (!strcmp(argv[1], "errs")) {
        hwo_errs_print(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "err") && argc > 3) {
        char msg[160] = "";
        for (int i = 3; i < argc; i++) {
            size_t l = strlen(msg);
            snprintf(msg + l, sizeof msg - l, "%s%s", l ? " " : "", argv[i]);
        }
        char sev = 'E';
        if (!hwo_errs_known(argv[2])) sev = 'E';
        else if (argv[2][0] == 'F') sev = 'F';
        else if (argv[2][0] == 'W') sev = 'W';
        hwo_note(sev, argv[2], "%s", msg);
        return 0;
    }
    if (!strcmp(argv[1], "update")) {
        if (argc < 3) {
            fprintf(stderr, "usage: hwo update BUNDLE [--force]\n");
            return 2;
        }
        int force = (argc > 3 && !strcmp(argv[3], "--force"));
        return hwo_update(argv[2], force);
    }
    if (!strcmp(argv[1], "bootconfirm")) return hwo_boot_confirm();
    if (!strcmp(argv[1], "rollback"))    return hwo_boot_rollback();
    if (!strcmp(argv[1], "id")) {
        char d[32];
        device_id(d);
        puts(d);
        return 0;
    }
    if (!strcmp(argv[1], "license")) {
        char key[64];
        if (licensed(key, sizeof key)) { printf("licensed %s\n", key); return 0; }
        puts("unlicensed");
        return 1;
    }
    if (!strcmp(argv[1], "ui")) {
        ui_loop();
        return 0;
    }
    if (!strcmp(argv[1], "fbprobe")) {
        int fd = open("/dev/fb0", O_RDWR);
        if (fd < 0) { puts("no /dev/fb0"); return 1; }
        struct fb_var_screeninfo vi;
        struct fb_fix_screeninfo fi;
        ioctl(fd, FBIOGET_VSCREENINFO, &vi);
        ioctl(fd, FBIOGET_FSCREENINFO, &fi);
        printf("fb: %ux%u virtual %ux%u offset %u,%u bpp %u line %u accel %u\n",
               vi.xres, vi.yres, vi.xres_virtual, vi.yres_virtual,
               vi.xoffset, vi.yoffset, vi.bits_per_pixel, fi.line_length, fi.accel);
        int t = open("/dev/tty0", O_RDWR | O_NONBLOCK);
        printf("tty0: %d\n", t);
        if (t >= 0) {
            printf("KDSETMODE(GRAPHICS)=%d KDGETMODE=", ioctl(t, KDSETMODE, KD_GRAPHICS));
            int m = -1;
            ioctl(t, KDGETMODE, &m);
            printf("%d\n", m);
        }
        vi.yoffset = 0; vi.xoffset = 0; vi.activate = FB_ACTIVATE_NOW;
        printf("set yoffset=0 -> %d\n", ioctl(fd, FBIOPUT_VSCREENINFO, &vi));
        return 0;
    }
    if (!strcmp(argv[1], "vcsa") && argc > 2) {
        return dump_vcsa(argv[2]);
    }
    fprintf(stderr, "hwo: unknown mode\n");
    return 2;
}
