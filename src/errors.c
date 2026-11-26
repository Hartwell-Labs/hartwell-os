/* Error codes and the on-unit error log.
 *
 * The appliance has no shell, so a failure has to say what happened in one line
 * that an operator can read off a serial console and quote to support. Every
 * failure gets a stable code; docs/errors.md is the book that explains them.
 *
 * Log format (one line per event, append-only, RAM only):
 *
 *     0012s E1001 E  no /dev/fb0 after modprobe
 *
 * Severity: F fatal (no interface), E error (degraded), W warning (works).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "errors.h"

#define ERR_LOG "/run/hwo.err"
#define ERR_DOC "docs/errors.md#"

typedef struct { const char *code; char sev; const char *title; const char *fix; } ErrInfo;

static const ErrInfo ERRORS[] = {
    { "F1001", 'F', "no framebuffer",              "check the DRM module for the GPU; boot with hwo.debug=1" },
    { "F1002", 'F', "framebuffer unsupported",     "resolution must be >= 800x600 and 16/24/32 bpp" },
    { "F1003", 'E', "interface exited",            "an exit is a crash; 5 in one boot forces a reboot" },
    { "F1004", 'F', "crash loop, rebooting",       "boot the image with hwo.rescue=1 and read /run/hwo.err" },
    { "E2001", 'E', "licence file unreadable",     "the file exists but cannot be parsed; re-apply the licence" },
    { "E2002", 'E', "licence not valid here",      "device id or signature does not match this unit" },
    { "E2003", 'E', "licence key rejected",        "wrong format, wrong checksum, or issued for another unit" },
    { "W3001", 'W', "content file missing",        "the Overview falls back to built-in rows" },
    { "W3002", 'W', "content has no rows",         "content.txt parsed to zero rows; check the 'Label | value' format" },
    { "W4001", 'W', "no state partition",          "no disk labelled HWO_CFG; licence and content live in RAM" },
    { "E4002", 'E', "state partition mount failed","the label matches but the filesystem will not mount read-write" },
    { "W6001", 'W', "media directory unreadable",  "frames are there but the directory cannot be listed" },
    { "W6002", 'W', "media frame decode failed",   "one PNG is corrupt; playback skips it" },
    { "W7001", 'W', "clock not set",               "no RTC and no network time; the header clock is wrong" },
    { "W9001", 'W', "update not confirmed",        "the new slot never came up cleanly; the previous image is booted" },
    { "W9002", 'W', "no boot partition",            "no HWBOOT partition was found, so updates are disabled on this unit" },
    { "E9001", 'E', "update signature invalid",    "the bundle was not signed by the vendor key" },
    { "E9002", 'E', "update older than running",   "downgrade refused; pass force only with a signed bundle" },
    { "E9003", 'E', "no writable boot partition",  "the unit booted from read-only media; use a disk install or a USB stick with HWBOOT" },
    { "E9004", 'E', "update bundle unreadable",    "the manifest is missing or the write failed; re-copy the bundle" },
    { "E9005", 'E', "fallback request invalid",    "\"hwo fallback\" takes the slot the loader booted (a or b)" },
    { NULL, 0, NULL, NULL }
};

static const ErrInfo *lookup(const char *code)
{
    if (!code) return NULL;
    for (const ErrInfo *e = ERRORS; e->code; e++) {
        if (strcmp(e->code, code) == 0) return e;
    }
    return NULL;
}

int hwo_errs_known(const char *code) { return lookup(code) != NULL; }

const char *hwo_err_title(const char *code)
{
    const ErrInfo *e = lookup(code);
    return e ? e->title : "unknown code";
}

const char *hwo_err_fix(const char *code)
{
    const ErrInfo *e = lookup(code);
    return e ? e->fix : "see docs/errors.md";
}

static long uptime_s(void)
{
    char buf[64];
    int fd = open("/proc/uptime", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    long s = 0;
    for (char *p = buf; *p >= '0' && *p <= '9'; p++) s = s * 10 + (*p - '0');
    return s;
}

void hwo_note(char sev, const char *code, const char *fmt, ...)
{
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    long up = uptime_s();
    char line[256];
    int n = snprintf(line, sizeof line, "%04lds %s %c  %s\n", up < 0 ? 0 : up, code, sev, msg);
    if (n < 0) return;

    int fd = open(ERR_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        ssize_t w = write(fd, line, (size_t)n);
        (void)w;
        close(fd);
    }
    /* the serial console is the only place a human sees this before the screen */
    ssize_t w = write(2, line, (size_t)n);
    (void)w;
}

/* A warning that the log explains (no state partition, RAM only) is not the
   same as a failure: the UI counts them apart. */
void hwo_errs_split(int *errs, int *warns)
{
    if (errs) *errs = 0;
    if (warns) *warns = 0;
    FILE *f = fopen(ERR_LOG, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char code[8] = "", sev = 0;
        if (sscanf(line, "%*ds %7s %c", code, &sev) != 2) continue;
        if (sev == 'W') { if (warns) (*warns)++; }
        else { if (errs) (*errs)++; }
    }
    fclose(f);
}

int hwo_last_error(char *code_out, unsigned long n, char *msg_out, unsigned long mn)
{
    FILE *f = fopen(ERR_LOG, "r");
    if (!f) return -1;
    char line[256], last[256] = "";
    while (fgets(line, sizeof line, f)) {
        if (line[0]) snprintf(last, sizeof last, "%s", line);
    }
    fclose(f);
    if (!last[0]) return -1;
    char code[8] = "", sev = '?';
    if (sscanf(last, "%*ds %7s %c", code, &sev) != 2) return -1;
    char *msg = last;
    for (int i = 0; i < 3 && msg; i++) { msg = strchr(msg, ' '); if (msg) msg++; }
    if (!msg) msg = (char *)"";
    size_t l = strlen(msg);
    while (l && (msg[l - 1] == '\n' || msg[l - 1] == ' ')) msg[--l] = 0;
    if (code_out) snprintf(code_out, n, "%s", code);
    if (msg_out) snprintf(msg_out, mn, "%s", msg);
    return 0;
}

int hwo_errs_count(void)
{
    FILE *f = fopen(ERR_LOG, "r");
    if (!f) return 0;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

int hwo_errs_print(FILE *out)
{
    FILE *f = fopen(ERR_LOG, "r");
    if (!f) {
        fprintf(out, "no errors recorded this boot\n");
        return 0;
    }
    char line[256];
    int n = 0;
    char seen[24][8];
    int nseen = 0;
    while (fgets(line, sizeof line, f)) {
        char up[16] = "", code[8] = "";
        char sev = '?';
        if (sscanf(line, "%15s %7s %c", up, code, &sev) != 3) continue;
        /* skip the three leading fields to reach the message */
        char *msg = line;
        for (int i = 0; i < 3 && msg; i++) {
            msg = strchr(msg, ' ');
            if (msg) msg++;
        }
        if (!msg) msg = (char *)"";
        fprintf(out, "  %s  %-22s %s", code, hwo_err_title(code), msg);
        if (!strchr(line, '\n')) fputc('\n', out);
        int dup = 0;
        for (int i = 0; i < nseen; i++) if (strcmp(seen[i], code) == 0) dup = 1;
        if (!dup && nseen < 24) snprintf(seen[nseen++], sizeof seen[0], "%s", code);
        n++;
    }
    fclose(f);
    if (!n) {
        fprintf(out, "no errors recorded this boot\n");
        return 0;
    }
    fprintf(out, "\n%d event(s). Fixes:\n", n);
    for (int i = 0; i < nseen; i++)
        fprintf(out, "  %s  %s  (%s%s)\n", seen[i], hwo_err_fix(seen[i]), ERR_DOC, seen[i]);
    return n;
}
