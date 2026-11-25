/* Hartwell OS — framebuffer appliance UI.
 *
 * Draws directly into /dev/fb0 (bochs DRM in QEMU, any DRM/VESA framebuffer on
 * hardware), reads the keyboard from the active VT and never exits: the
 * supervisor in init restarts it if it does.
 *
 * Keys: 1 Overview  2 System  3 Licence  4 Media  F1..F4 the same  R refresh.
 * Pointer: evdev mouse/tablet moves the cursor and clicks the tabs, the key
 * field and the Activate button. On an unlicensed unit the licence page takes
 * the key from the keyboard.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

#include "font.h"
#include "hwo.h"
#include "errors.h"
#include "png.h"

/* ---------- palette ---------- */

#define BG        0x0B0F14
#define BG2       0x0D131A
#define PANEL     0x131A23
#define PANEL2    0x101720
#define LINE      0x202D3B
#define TEXT      0xE4ECF5
#define DIM       0x8CA0B6
#define FAINT     0x5B6F85
#define GREEN     0x3DDC84
#define RED       0xF2555F
#define AMBER     0xF5B83D
#define CYAN      0x4CC5E0

/* ---------- framebuffer ---------- */

static unsigned char *fb;       /* VRAM, slow MMIO */
static unsigned char *cv;       /* shadow buffer in RAM, all drawing happens here */
static int W, H, stride, bytespp;
static int fb_fd = -1, tty_fd = -1, in_fd = -1;

static inline void blend(int x, int y, unsigned c, unsigned a)
{
    if (x < 0 || y < 0 || x >= W || y >= H || a == 0) return;
    unsigned char *p = cv + (size_t)y * stride + (size_t)x * bytespp;
    unsigned sr = (c >> 16) & 255, sg = (c >> 8) & 255, sb = c & 255;

    if (a >= 255) {
        if (bytespp == 4) { unsigned *q = (unsigned *)p; *q = 0xFF000000u | (sr << 16) | (sg << 8) | sb; return; }
        if (bytespp == 3) { p[0] = (unsigned char)sb; p[1] = (unsigned char)sg; p[2] = (unsigned char)sr; return; }
        if (bytespp == 2) { unsigned v = (unsigned)(((sr >> 3) << 11) | ((sg >> 2) << 5) | (sb >> 3)); *(unsigned short *)p = (unsigned short)v; return; }
    }
    if (bytespp == 4) {
        unsigned *q = (unsigned *)p;
        unsigned o = *q;
        unsigned orr = (o >> 16) & 255, og = (o >> 8) & 255, ob = o & 255;
        unsigned nr = (sr * a + orr * (255 - a)) / 255;
        unsigned ng = (sg * a + og * (255 - a)) / 255;
        unsigned nb = (sb * a + ob * (255 - a)) / 255;
        *q = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
    } else if (bytespp == 3) {
        p[0] = (unsigned char)((sb * a + p[0] * (255 - a)) / 255);
        p[1] = (unsigned char)((sg * a + p[1] * (255 - a)) / 255);
        p[2] = (unsigned char)((sr * a + p[2] * (255 - a)) / 255);
    } else if (bytespp == 2) {
        unsigned short o = *(unsigned short *)p;
        unsigned orr = ((o >> 11) & 31) * 255 / 31, og = ((o >> 5) & 63) * 255 / 63, ob = (o & 31) * 255 / 31;
        unsigned nr = (sr * a + orr * (255 - a)) / 255;
        unsigned ng = (sg * a + og * (255 - a)) / 255;
        unsigned nb = (sb * a + ob * (255 - a)) / 255;
        *(unsigned short *)p = (unsigned short)(((nr >> 3) << 11) | ((ng >> 2) << 5) | (nb >> 3));
    }
}

/* copy the changed rows of the shadow buffer to the real framebuffer */
static void present(void)
{
    for (int y = 0; y < H; y++) {
        unsigned char *src = cv + (size_t)y * stride;
        unsigned char *dst = fb + (size_t)y * stride;
        if (memcmp(src, dst, (size_t)stride) != 0)
            memcpy(dst, src, (size_t)stride);
    }
}

static void fill(int x, int y, int w, int h, unsigned c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    if (bytespp == 4) {
        unsigned v = 0xFF000000u | (c & 0xFFFFFF);
        for (int j = 0; j < h; j++) {
            unsigned *row = (unsigned *)(cv + (size_t)(y + j) * stride) + x;
            for (int i = 0; i < w; i++) row[i] = v;
        }
        return;
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            blend(x + i, y + j, c, 255);
}

static void vgrad(int x, int y, int w, int h, unsigned top, unsigned bot)
{
    for (int j = 0; j < h; j++) {
        int t = h > 1 ? j * 255 / (h - 1) : 0;
        unsigned r = (((top >> 16) & 255) * (255 - t) + ((bot >> 16) & 255) * t) / 255;
        unsigned g = (((top >> 8) & 255) * (255 - t) + ((bot >> 8) & 255) * t) / 255;
        unsigned b = ((top & 255) * (255 - t) + (bot & 255) * t) / 255;
        fill(x, y + j, w, 1, (r << 16) | (g << 8) | b);
    }
}

/* corner coverage for a rounded rectangle */
static void rrect(int x, int y, int w, int h, int r, unsigned c)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fill(x + r, y, w - 2 * r, h, c);
    fill(x, y + r, r, h - 2 * r, c);
    fill(x + w - r, y + r, r, h - 2 * r, c);

    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            /* supersampled quarter disc */
            int inside = 0;
            for (int sy = 0; sy < 2; sy++)
                for (int sx = 0; sx < 2; sx++) {
                    double dx = (double)(i * 2 + sx + 1) / 2.0 - r;
                    double dy = (double)(j * 2 + sy + 1) / 2.0 - r;
                    if (dx * dx + dy * dy <= (double)r * r) inside++;
                }
            unsigned a = (unsigned)inside * 255 / 4;
            blend(x + r - 1 - i, y + r - 1 - j, c, a);           /* top-left */
            blend(x + w - r + i, y + r - 1 - j, c, a);           /* top-right */
            blend(x + r - 1 - i, y + h - r + j, c, a);           /* bottom-left */
            blend(x + w - r + i, y + h - r + j, c, a);           /* bottom-right */
        }
    }
}

static void panel(int x, int y, int w, int h, unsigned bgc)
{
    (void)bgc; (void)h;
    /* a section is marked by one rule and the space above it, the way a spec
       sheet is: no box, no radius, no shadow, nothing to look at but text */
    fill(x, y, w, 1, LINE);
}


/* ---------- text ---------- */

static int text_w(const Face *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        w += f->g[ch - 32].adv;
    }
    return w;
}

static void text(const Face *f, int x, int y, unsigned c, const char *s)
{
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const Glyph *g = &f->g[ch - 32];
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                unsigned a = f->px[g->off + (unsigned)j * g->w + i];
                if (a) blend(x + i, y + j, c, a);
            }
        x += g->adv;
    }
}

static void text_wrap(const Face *f, int x, int y, int maxw, int lineh, unsigned c, const char *s)
{
    char line[256];
    size_t n = 0;
    line[0] = 0;
    while (*s) {
        while (*s == ' ') s++;
        const char *w = s;
        while (*s && *s != ' ') s++;
        size_t wl = (size_t)(s - w);
        char word[64];
        if (wl >= sizeof word) wl = sizeof word - 1;
        memcpy(word, w, wl);
        word[wl] = 0;

        char cand[256];
        if (n == 0) snprintf(cand, sizeof cand, "%s", word);
        else        snprintf(cand, sizeof cand, "%s %s", line, word);
        if (text_w(f, cand) > maxw && n > 0) {
            text(f, x, y, c, line);
            y += lineh;
            snprintf(line, sizeof line, "%s", word);
            n = strlen(line);
        } else {
            snprintf(line, sizeof line, "%s", cand);
            n = strlen(line);
        }
    }
    if (n) text(f, x, y, c, line);
}

/* draw as much of s as fits in maxw, with a quiet ellipsis */
static void text_clip(const Face *f, int x, int y, unsigned c, const char *s, int maxw)
{
    int w = 0;
    const char *p = s;
    for (; *p; p++) {
        char one[2] = { *p, 0 };
        int cw = text_w(f, one);
        if (w + cw > maxw - 12) {
            /* write the prefix that fits, then two quiet dots */
            int px = x;
            for (const char *q = s; q < p; q++) {
                char o[2] = { *q, 0 };
                text(f, px, y, c, o);
                px += text_w(f, o);
            }
            text(f, px, y, FAINT, "..");
            return;
        }
        w += cw;
    }
    text(f, x, y, c, s);
}

static void text_r(const Face *f, int right, int y, unsigned c, const char *s)
{
    text(f, right - text_w(f, s), y, c, s);
}
/* Captions are tracked a little: uppercase mono at normal spacing disappears
   into a dark UI, one extra pixel per glyph makes them read as labels. */
static void cap(const Face *f, int x, int y, unsigned c, const char *s, int track)
{
    for (; *s; s++) {
        char one[2] = { *s, 0 };
        text(f, x, y, c, one);
        x += text_w(f, one) + track;
    }
}


/* ---------- state ---------- */

typedef struct {
    char title[128];
    char label[12][64];
    char value[12][128];
    int n;
} Content;

static Content content;
static char dev_id[32];
static char lic_key[64];
static int is_licensed;
static int page;                    /* 0 overview, 1 system, 2 licence, 3 media */
static char typed[32];
static char msg[128];

/* clickable regions, filled in while drawing and hit-tested on click */
enum { ACT_NONE = 0, ACT_PAGE0, ACT_PAGE1, ACT_PAGE2, ACT_PAGE3, ACT_ACTIVATE };
typedef struct { int x, y, w, h, act; } Hit;
static Hit hits[16];
static int nhits;

static void hit_add(int x, int y, int w, int h, int act)
{
    if (nhits < (int)(sizeof hits / sizeof hits[0])) {
        hits[nhits].x = x; hits[nhits].y = y;
        hits[nhits].w = w; hits[nhits].h = h;
        hits[nhits].act = act;
        nhits++;
    }
}

/* pointer state: every mouse/tablet/touchscreen is read, so a unit with a
 * touch panel and a mouse does not lose one of them */
typedef struct {
    int fd;
    int abs;
    int x0, x1, y0, y1;
    char path[40];
} PtrDev;

#define MAX_PTR 4
static PtrDev ptrs[MAX_PTR];
static int nptr;
static int ptr_x, ptr_y, ptr_visible;
static int debug_on;                /* hwo.debug=1 on the kernel command line */

/* media layer: PNG still or a numbered PNG frame sequence on the state partition */
#define MEDIA_FPS_DEFAULT 10
static char media_dir[64];
static int media_frames, media_first, media_index = -1, media_fps = MEDIA_FPS_DEFAULT;
static Png frame, still;
static long long media_last_ms, media_scan_ms;

static void load_content(void)
{
    char raw[1024];
    content.n = 0;
    content.title[0] = 0;
    if (hwo_read("/etc/hartwell/content.txt", raw, sizeof raw) != 0) {
        hwo_note('W', "W3001", "/etc/hartwell/content.txt missing; using built-in rows");
        snprintf(content.title, sizeof content.title, "No content configured");
        return;
    }
    char *save = NULL;
    int line = 0;
    for (char *l = strtok_r(raw, "\n", &save); l && content.n < 12; l = strtok_r(NULL, "\n", &save)) {
        if (line++ == 0) {
            snprintf(content.title, sizeof content.title, "%s", l);
            continue;
        }
        char *bar = strchr(l, '|');
        if (bar) {
            *bar = 0;
            char *lab = l, *val = bar + 1;
            while (*val == ' ') val++;
            char *e = lab + strlen(lab);
            while (e > lab && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            if (!lab[0] && !val[0]) continue;
            snprintf(content.label[content.n], 64, "%s", lab);
            snprintf(content.value[content.n], 128, "%s", val);
        } else {
            content.label[content.n][0] = 0;
            snprintf(content.value[content.n], 128, "%s", l);
        }
        content.n++;
    }
}

static void human_uptime(char *out, size_t n)
{
    char buf[64];
    if (hwo_read("/proc/uptime", buf, sizeof buf) != 0) { snprintf(out, n, "-"); return; }
    long s = atol(buf);
    long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (d)      snprintf(out, n, "%ld d %ld h %ld m", d, h, m);
    else if (h) snprintf(out, n, "%ld h %ld m %ld s", h, m, sec);
    else        snprintf(out, n, "%ld m %ld s", m, sec);
}

static void mem_line(char *out, size_t n)
{
    char buf[2048];
    if (hwo_read("/proc/meminfo", buf, sizeof buf) != 0) { snprintf(out, n, "-"); return; }
    long total = 0, avail = 0;
    char *p = strstr(buf, "MemTotal:");
    if (p) total = atol(p + 9);
    p = strstr(buf, "MemAvailable:");
    if (p) avail = atol(p + 13);
    snprintf(out, n, "%ld MB free of %ld MB", avail / 1024, total / 1024);
}

/* ---------- pages ---------- */

#define RAIL 236

static void build_chrome_bg(void)
{
    vgrad(0, 0, W, H, BG, BG2);
    /* left rail: the whole navigation lives here, so the content keeps the
       full height of the screen instead of losing a strip to a tab bar */
    fill(0, 0, RAIL, H, PANEL2);
    fill(RAIL, 0, 1, H, LINE);
    /* one baseline across the whole sheet, level with the rail footer rule */
    fill(RAIL + 1, H - 132, W - RAIL - 1, 1, 0x1B242E);
    text(&fb_bold, 28, 28, TEXT, "Hartwell OS");
}

static unsigned char *chrome_bg;      /* static background, built once */

static void draw_chrome(void)
{
    nhits = 0;
    size_t fbsz = (size_t)stride * H;
    if (chrome_bg) {
        memcpy(cv, chrome_bg, fbsz);
    } else {
        build_chrome_bg();
        chrome_bg = malloc(fbsz);
        if (chrome_bg) memcpy(chrome_bg, cv, fbsz);
    }

    /* ---- navigation ---- */
    static const char *names[4] = { "Overview", "System", "Licence", "Media" };
    int ny = 118;
    for (int i = 0; i < 4; i++) {
        int active = page == i;
        text(active ? &fb_bold : &fb_body, 28, ny, active ? TEXT : DIM, names[i]);
        hit_add(16, ny - 12, RAIL - 32, 52, ACT_PAGE0 + i);
        ny += 62;
    }


    /* ---- rail footer: state, identity, clock ---- */
    int fy = H - 132;
    fill(28, fy, RAIL - 56, 1, LINE);

    unsigned col = is_licensed ? GREEN : RED;
    text(&fb_body, 28, fy + 22, col, is_licensed ? "Licensed" : "Unlicensed");
    text(&fb_mono, 28, fy + 52, FAINT, dev_id);

    char clock[16];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(clock, sizeof clock, "%H:%M UTC", &tm);
    text(&fb_mono, 28, fy + 76, DIM, clock);

    text_r(&fb_body, W - 40, 27, FAINT,
           is_licensed ? "1-4 to switch screen" : "type the key, then ENTER");
}

static void mem_stat(long *free_mb, long *total_mb)
{
    char buf[2048];
    *free_mb = *total_mb = 0;
    if (hwo_read("/proc/meminfo", buf, sizeof buf) != 0) return;
    char *p = strstr(buf, "MemTotal:");
    if (p) *total_mb = atol(p + 9) / 1024;
    p = strstr(buf, "MemAvailable:");
    if (p) *free_mb = atol(p + 13) / 1024;
}

/* One metric tile: caption on top, the number large, a quiet line under it.
   Numbers are the only large type on the screen, which is what makes a
   dashboard readable from across a room. */
static void tile(int x, int y, int w, int h, const char *caption,
                 const char *value, const char *sub, unsigned accent)
{
    (void)h;
    text(&fb_body, x, y + 20, FAINT, caption);
    unsigned vc = (accent == AMBER || accent == RED) ? accent : TEXT;
    text(&fb_h1, x, y + 48, vc, value);
    if (sub && sub[0]) text_clip(&fb_mono, x, y + h - 32, DIM, sub, w);
}

static void draw_overview(void)
{
    int cx = RAIL + 40, cw = W - RAIL - 80;
    int hy = 84, hh = 306;

    /* ---- status card ---- */
    panel(cx, hy, cw, hh, PANEL);

    text(&fb_h1, cx, hy + 30, TEXT, content.title);

    char src[32] = "local";
    hwo_read("/run/fleet.status", src, sizeof src);
    char meta[160];
    snprintf(meta, sizeof meta, "%s, source %s",
             is_licensed ? "live" : "locked", strcmp(src, "local") ? "fleet" : "local");
    text(&fb_body, cx, hy + 86, DIM, meta);

    fill(cx, hy + 128, cw, 1, LINE);

    /* content rows: label in the left column, value aligned in the right */
    int y = hy + 152;
    int shown = content.n > 6 ? 6 : content.n;
    int half = (cw) / 2;
    for (int i = 0; i < shown; i++) {
        int col = i % 2, row = i / 2;
        int bx = cx + col * half;
        int by = y + row * 44;
        if (content.label[i][0]) {
            text(&fb_body, bx, by, FAINT, content.label[i]);
            text(&fb_body, bx + 136, by, TEXT, content.value[i]);
        } else {
            text(&fb_body, bx, by, TEXT, content.value[i]);
        }
    }

    /* ---- metric row ---- */
    int gy = hy + hh + 24, gh = 168;
    int gap = 20, gw = (cw - 2 * gap) / 3;
    fill(cx, gy - 4, cw, 1, LINE);

    char up[64], st[64] = "local";
    long fm = 0, tm = 0;
    human_uptime(up, sizeof up);
    mem_stat(&fm, &tm);
    hwo_read("/run/fleet.status", st, sizeof st);

    tile(cx, gy, gw, gh, "UPTIME", up, is_licensed ? "service running" : "awaiting licence", CYAN);

    char memv[32], mems[64];
    snprintf(memv, sizeof memv, "%ld MB", fm);
    snprintf(mems, sizeof mems, "free of %ld MB", tm);
    tile(cx + gw + gap, gy, gw, gh, "MEMORY", memv, mems, GREEN);

    unsigned fcol = !strcmp(st, "online") ? GREEN : !strcmp(st, "offline") ? AMBER : DIM;
    tile(cx + 2 * (gw + gap), gy, gw, gh, "FLEET", st, "outbound poll / 30 s", fcol);

    /* ---- facts strip: sits on the baseline, not in the middle of nowhere ---- */
    text(&fb_body, cx, H - 112, FAINT,
         "No shell. No listening sockets. Five consecutive UI crashes reboot the unit.");
}

static void draw_system(void)
{
    int cx = RAIL + 40, cy = 84, cw = W - RAIL - 80, ch = 366;
    panel(cx, cy, cw, ch, PANEL);
    text(&fb_h1, cx, cy + 30, TEXT, "System");

    /* Degraded state is said on the screen, not only in the log: a plain
       count, right-aligned, in the colour of its severity. */
    int errs = 0, warns = 0;
    hwo_errs_split(&errs, &warns);
    if (errs || warns) {
        char lbl[24];
        if (errs)
            snprintf(lbl, sizeof lbl, "%d error%s", errs, errs == 1 ? "" : "s");
        else
            snprintf(lbl, sizeof lbl, "%d warning%s", warns, warns == 1 ? "" : "s");
        text_r(&fb_body, cx + cw, cy + 38, errs ? RED : AMBER, lbl);
    }

    struct utsname u;
    uname(&u);
    char up[64], mem[64], fbline[64];
    human_uptime(up, sizeof up);
    mem_line(mem, sizeof mem);
    snprintf(fbline, sizeof fbline, "%d x %d @ %d bpp", W, H, bytespp * 8);

    const char *labels[] = { "Kernel", "Machine", "Uptime", "Memory", "Display", "Runtime",
                             "State", "Licence" };
    char vals[8][128];
    snprintf(vals[0], 128, "%s", u.release);
    snprintf(vals[1], 128, "%s", u.machine);
    snprintf(vals[2], 128, "%s", up);
    snprintf(vals[3], 128, "%s", mem);
    snprintf(vals[4], 128, "%s", fbline);
    snprintf(vals[5], 128, "hwo 1.0 / PID 1");
    snprintf(vals[7], 128, "%s", is_licensed ? lic_key : "none");
    if (hwo_state_present()) {
        snprintf(vals[6], 128, "HWO_CFG mounted at /state");
    } else {
        char st[512];
        if (hwo_read("/run/state.log", st, sizeof st) != 0) st[0] = 0;
        for (char *c = st; *c; c++) if (*c == '\n') *c = ' ';
        size_t l = strlen(st);
        const char *tail = l > 110 ? st + l - 110 : st;
        if (strstr(st, "failed")) snprintf(vals[6], 128, "%.110s", tail);
        else                    snprintf(vals[6], 128, "RAM only (no HWO_CFG)");
    }

    fill(cx, cy + 128, cw, 1, LINE);
    int half = (cw) / 2;
    for (int i = 0; i < 8; i++) {
        int col = i % 2, row = i / 2;
        int bx = cx + col * half;
        int by = cy + 152 + row * 44;
        /* values sit in one column per half, so the eye can scan the list
           instead of hunting for where each label ends */
        text(&fb_body, bx, by, FAINT, labels[i]);
        text_clip(&fb_mono, bx + 148, by - 2, i == 7 && !is_licensed ? RED : TEXT,
                  vals[i], half - 148 - 16);
    }

    int gy = cy + ch + 24, gh = 150, gw = (cw - 24) / 2;
    panel(cx, gy, gw, gh, PANEL);
    panel(cx + gw + 24, gy, gw, gh, PANEL);

    cap(&fb_bold, cx, gy + 22, FAINT, "SESSION", 2);
    text(&fb_mono, cx, gy + 58, TEXT, "no shell / no login");
    text(&fb_mono, cx, gy + 88, DIM, "no listening sockets");
    text(&fb_mono, cx, gy + 118, DIM, "5 UI exits -> reboot");

    cap(&fb_bold, cx + gw + 52, gy + 22, FAINT, "AGENT", 2);
    char st[64] = "local";
    hwo_read("/run/fleet.status", st, sizeof st);
    text(&fb_mono, cx + gw + 52, gy + 58, TEXT, st);
    char url[96] = "no endpoint set";
    if (hwo_read("/run/fleet.url", url, sizeof url) != 0 || !url[0])
        snprintf(url, sizeof url, "endpoint not configured");
    text(&fb_mono, cx + gw + 52, gy + 88, DIM, url);
    text(&fb_mono, cx + gw + 52, gy + 118, DIM, "poll 30 s / outbound only");
}

static void draw_license(void)
{
    int cx = RAIL + 40, cy = 84, cw = W - RAIL - 80, ch = is_licensed ? 360 : 420;
    panel(cx, cy, cw, ch, PANEL);
    text(&fb_h1, cx, cy + 30, TEXT, is_licensed ? "Licence" : "Activate this device");

    fill(cx, cy + 126, cw, 1, LINE);

    if (is_licensed) {
        text(&fb_body, cx, cy + 150, DIM, "Device id");
        text(&fb_mono, cx + 300, cy + 150, TEXT, dev_id);
        text(&fb_body, cx, cy + 190, DIM, "Key");
        text(&fb_mono, cx + 300, cy + 190, GREEN, lic_key);
        text(&fb_body, cx, cy + 230, DIM, "Bound to");
        text(&fb_mono, cx + 300, cy + 230, TEXT, "this machine's DMI UUID");
        text(&fb_body, cx, cy + 270, DIM, "Validated");
        text(&fb_mono, cx + 300, cy + 270, TEXT, "offline / no activation server");
        text(&fb_body, cx, cy + 310, DIM, "Stored on");
        text(&fb_mono, cx + 300, cy + 310,
             hwo_state_persistent() ? GREEN : AMBER,
             hwo_state_persistent() ? "HWO_CFG partition, survives reboot"
                                    : "initramfs only, lost on reboot");

        text(&fb_body, cx, H - 112, FAINT,
             "Perpetual, one machine. Updates for 12 months. Support: business@hartwell-labs.pl");
        return;
    }

    char line[160];
    snprintf(line, sizeof line, "Send this device id to business@hartwell-labs.pl and type the key you get back.");
    text(&fb_body, cx, cy + 146, DIM, line);

    text(&fb_body, cx, cy + 176, DIM, "Device id");
    text(&fb_mono, cx + 300, cy + 176, AMBER, dev_id);

    text(&fb_body, cx, cy + 226, DIM, "Key");

    int bx = cx + 300, by = cy + 214, bw = 420, bh = 46;
    rrect(bx, by, bw, bh, 8, typed[0] ? GREEN : LINE);
    rrect(bx + 1, by + 1, bw - 2, bh - 2, 7, PANEL2);
    text(&fb_mono, bx + 16, by + 13, typed[0] ? TEXT : FAINT,
         typed[0] ? typed : "HWO-YYYY-XXXX");

    time_t now = time(NULL);
    if (typed[0] && (now % 2) == 0) {
        int w = text_w(&fb_mono, typed);
        fill(bx + 17 + w, by + 12, 2, 22, GREEN);
    }
    hit_add(bx, by, bw, bh, ACT_NONE);

    /* the same action ENTER performs, reachable with the pointer */
    int abx = bx + bw + 24, abw = 168;
    rrect(abx, by, abw, bh, 8, GREEN);
    text(&fb_bold, abx + 56, by + 13, 0x08150D, "Activate");
    hit_add(abx, by, abw, bh, ACT_ACTIVATE);

    char hint[128] = "Type the key, then ENTER to activate";
    if (msg[0]) snprintf(hint, sizeof hint, "%s", msg);
    text(&fb_body, cx, cy + 290, msg[0] ? (strstr(msg, "Invalid") ? RED : GREEN) : FAINT, hint);
    text(&fb_body, cx, cy + 330, FAINT, "Offline check: the key only works on this machine");

    text_wrap(&fb_body, cx, cy + ch + 24, cw, 26, FAINT,
              "The key is valid only on this unit. Once it checks out, the appliance starts.");
}

/* fit a decoded image into a box, whole image visible, centred */
static void draw_png_fit(const Png *p, int x0, int y0, int w0, int h0)
{
    if (!p || !p->rgba || p->w <= 0 || p->h <= 0 || w0 <= 0 || h0 <= 0) return;
    double sx = (double)p->w / (double)w0, sy = (double)p->h / (double)h0;
    double s = sx > sy ? sx : sy;
    int vw = (int)(p->w / s), vh = (int)(p->h / s);
    if (vw > w0) vw = w0;
    if (vh > h0) vh = h0;
    int ox = x0 + (w0 - vw) / 2, oy = y0 + (h0 - vh) / 2;
    for (int y = 0; y < vh; y++) {
        int syy = (int)((double)y * s);
        if (syy >= p->h) syy = p->h - 1;
        for (int x = 0; x < vw; x++) {
            int sxx = (int)((double)x * s);
            if (sxx >= p->w) sxx = p->w - 1;
            const unsigned char *q = p->rgba + ((size_t)syy * p->w + sxx) * 4;
            blend(ox + x, oy + y, ((unsigned)q[0] << 16) | ((unsigned)q[1] << 8) | q[2], q[3]);
        }
    }
}

static void media_scan(void)
{
    media_frames = 0;
    media_fps = MEDIA_FPS_DEFAULT;
    media_dir[0] = 0;

    const char *cand[2] = { "/state/media", "/etc/hartwell/media" };
    for (int c = 0; c < 2 && !media_frames; c++) {
        DIR *d = opendir(cand[c]);
        if (!d) continue;
        struct dirent *e;
        int max = -1, min = -1;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "frame", 5) != 0) continue;
            char *end = NULL;
            long n = strtol(e->d_name + 5, &end, 10);
            if (!end || strcmp(end, ".png") || n < 0 || n >= 100000) continue;
            if ((int)n > max) max = (int)n;
            if (min < 0 || (int)n < min) min = (int)n;
        }
        closedir(d);
        if (max >= 0) {
            snprintf(media_dir, sizeof media_dir, "%s", cand[c]);
            media_first = min;
            media_frames = max - min + 1;
            char buf[32], path[96];
            snprintf(path, sizeof path, "%s/fps", cand[c]);
            if (hwo_read(path, buf, sizeof buf) == 0) {
                int f = atoi(buf);
                if (f >= 1 && f <= 60) media_fps = f;
            }
            return;
        }
    }

    /* no sequence: a single image is still useful */
    const char *names[2] = { "poster.png", "image.png" };
    for (int c = 0; c < 2; c++) {
        char path[96];
        snprintf(path, sizeof path, "%s/%s", cand[c], names[c]);
        if (png_load(path, &still) == 0) {
            snprintf(media_dir, sizeof media_dir, "%s", cand[c]);
            break;
        }
    }
}

static void media_load(int idx)
{
    char path[96];
    snprintf(path, sizeof path, "%s/frame%04d.png", media_dir, media_first + idx);
    Png next;
    if (png_load(path, &next) != 0) {
        media_index = idx;          /* skip a broken frame, keep playing */
        return;
    }
    png_free(&frame);
    frame = next;
    media_index = idx;
}

static void draw_media(void)
{
    int cx = RAIL + 40, cy = 84, cw = W - RAIL - 80, ch = 430;
    panel(cx, cy, cw, ch, PANEL);
    text(&fb_h1, cx, cy + 34, TEXT, "Media");

    if (media_frames > 0) {
        if (media_index < 0) media_load(0);
        char meta[160];
        snprintf(meta, sizeof meta, "local frames   |   %d fps   |   frame %d of %d",
                 media_fps, media_index < 0 ? 0 : media_index + 1, media_frames);
        text(&fb_body, cx, cy + 90, FAINT, meta);
        fill(cx, cy + 126, cw, 1, LINE);
        draw_png_fit(&frame, cx + 20, cy + 140, cw - 40, ch - 162);
        return;
    }

    if (still.rgba) {
        char meta[160];
        snprintf(meta, sizeof meta, "local still   |   %d x %d   |   %s", still.w, still.h, media_dir);
        text(&fb_body, cx, cy + 90, FAINT, meta);
        fill(cx, cy + 126, cw, 1, LINE);
        draw_png_fit(&still, cx + 20, cy + 140, cw - 40, ch - 162);
        return;
    }

    text(&fb_body, cx, cy + 90, FAINT, "no media on this unit   |   local files only, no network");
    fill(cx, cy + 126, cw, 1, LINE);

    text(&fb_body, cx, cy + 152, TEXT, "Copy PNG frames to the state partition and this screen plays them:");
    text(&fb_mono, cx, cy + 190, GREEN, "/state/media/frame0000.png ... frameNNNN.png");
    text(&fb_mono, cx, cy + 220, GREEN, "/state/media/fps        (1-60, default 10)");
    text(&fb_mono, cx, cy + 250, GREEN, "/state/media/poster.png (single still)");

    text(&fb_body, cx, cy + 296, DIM, "Convert a clip on any workstation:");
    text(&fb_mono, cx, cy + 326, TEXT, "ffmpeg -i clip.mp4 -vf scale=640:-2 -r 10 /run/media/frame%04d.png");
    text(&fb_body, cx, cy + 366, FAINT, "PNG 8-bit (RGB, RGBA, grey or palette), non-interlaced. Frames loop.");
}

static void draw_footer(void)
{
    /* nothing to draw: navigation and state live in the rail */
}

/* ---------- pointer ---------- */

static void draw_pointer(void)
{
    if (!ptr_visible) return;
    static const char *shape[] = {
        "#............", "##...........", "#.#..........", "#..#.........",
        "#...#........", "#....#.......", "#.....#......", "#......#.....",
        "#.......#....", "#........#...", "#.........#..", "#......#####.",
        "#...#..#.....", "#..#...#.....", "##..#..#.....", "#....#.#.....",
        "#.....##.....",
    };
    for (int pass = 0; pass < 2; pass++) {
        int ox = pass == 0 ? 1 : 0, oy = pass == 0 ? 1 : 0;
        unsigned col = pass == 0 ? 0x000000 : 0xFFFFFF;
        unsigned a = pass == 0 ? 190 : 255;
        for (int j = 0; j < (int)(sizeof shape / sizeof shape[0]); j++)
            for (int i = 0; shape[j][i]; i++)
                if (shape[j][i] == '#')
                    blend(ptr_x + i + ox, ptr_y + j + oy, col, a);
    }
}

static void redraw(void)
{
    draw_chrome();
    if (page == 0) draw_overview();
    else if (page == 1) draw_system();
    else if (page == 2) draw_license();
    else draw_media();
    draw_footer();
    draw_pointer();
    present();
}

/* ---------- input ---------- */

/* PS/2 or USB keyboard via evdev: no VT session rules, works in graphics mode */
static int evdev_char(int code, int shift, char *out)
{
    static const char *row1 = "1234567890";
    static const char *row2 = "qwertyuiop";
    static const char *row3 = "asdfghjkl";
    static const char *row4 = "zxcvbnm";

    if (code >= KEY_1 && code <= KEY_0) { *out = row1[code - KEY_1]; return 1; }
    if (code >= KEY_Q && code <= KEY_P) { *out = row2[code - KEY_Q]; return 1; }
    if (code >= KEY_A && code <= KEY_L) { *out = row3[code - KEY_A]; return 1; }
    if (code >= KEY_Z && code <= KEY_M) { *out = row4[code - KEY_Z]; return 1; }
    switch (code) {
    case KEY_MINUS:     *out = '-'; return 1;
    case KEY_SPACE:     *out = ' '; return 1;
    case KEY_DOT:       *out = '.'; return 1;
    case KEY_SLASH:     *out = '/'; return 1;
    case KEY_ENTER:
    case KEY_KPENTER:   *out = '\r'; return 1;
    case KEY_BACKSPACE:
    case KEY_DELETE:    *out = 0x7f; return 1;
    case KEY_ESC:       *out = 0x1b; return 1;
    case KEY_F1:        *out = 0x01; return 1;
    case KEY_F2:        *out = 0x02; return 1;
    case KEY_F3:        *out = 0x03; return 1;
    case KEY_F4:        *out = 0x04; return 1;
    }
    (void)shift;
    return 0;
}

static void input_init(void)
{
    for (int i = 0; i < 8 && in_fd < 0; i++) {
        char path[40];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (strstr(name, "eyboard") || strstr(name, "QEMU") || strstr(name, "KBD")) in_fd = fd;
        else close(fd);
    }
}

static int shift_down;

/* returns number of chars produced */
static int read_evdev(char *out, int max)
{
    struct input_event ev;
    int n = 0;
    while (read(in_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type != EV_KEY) continue;
        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            shift_down = ev.value != 0;
            continue;
        }
        if (ev.value != 1) continue;           /* presses only; ignore autorepeat */
        char c;
        if (!evdev_char(ev.code, shift_down, &c)) continue;
        if (n < max) {
            if (c >= 'a' && c <= 'z' && shift_down) c = (char)(c - 32);
            out[n++] = c;
        }
    }
    return n;
}

static long long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the one place that turns a typed key into a licensed unit */
static void activate(void)
{
    if (is_licensed || !typed[0]) return;
    if (!hwo_key_valid(typed)) {
        snprintf(msg, sizeof msg, "Invalid key for %s", dev_id);
        return;
    }
    if (hwo_save_key(typed) != 0) {
        snprintf(msg, sizeof msg, "Key is valid but could not be saved (read-only media)");
        return;
    }
    snprintf(lic_key, sizeof lic_key, "%s", typed);
    is_licensed = 1;
    msg[0] = 0;
    typed[0] = 0;
    page = 0;
}

static void hit_click(void)
{
    for (int i = 0; i < nhits; i++) {
        const Hit *h = &hits[i];
        if (ptr_x < h->x || ptr_y < h->y || ptr_x >= h->x + h->w || ptr_y >= h->y + h->h) continue;
        if (h->act >= ACT_PAGE0 && h->act <= ACT_PAGE3) page = h->act - ACT_PAGE0;
        else if (h->act == ACT_ACTIVATE) activate();
        return;
    }
}

static int bit_set(const unsigned long *a, int b)
{
    return (int)((a[b / (8 * sizeof(unsigned long))] >> (b % (8 * sizeof(unsigned long)))) & 1UL);
}

static void mouse_init(void)
{
    unsigned long evbit[8], keybit[8];
    for (int i = 0; i < 16 && nptr < MAX_PTR; i++) {
        char path[40];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        memset(evbit, 0, sizeof evbit);
        memset(keybit, 0, sizeof keybit);
        if (ioctl(fd, EVIOCGBIT(0, sizeof evbit), evbit) < 0 ||
            !bit_set(evbit, EV_KEY) ||
            (!bit_set(evbit, EV_REL) && !bit_set(evbit, EV_ABS))) {
            close(fd);
            continue;
        }
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybit), keybit);
        if (!bit_set(keybit, BTN_LEFT) && !bit_set(keybit, BTN_TOUCH)) { close(fd); continue; }

        int seen = 0;
        for (int k = 0; k < nptr; k++)
            if (strcmp(ptrs[k].path, path) == 0) seen = 1;
        if (seen) { close(fd); continue; }

        PtrDev *p = &ptrs[nptr];
        memset(p, 0, sizeof *p);
        p->fd = fd;
        snprintf(p->path, sizeof p->path, "%s", path);
        if (bit_set(evbit, EV_ABS)) {
            struct input_absinfo ai;
            p->abs = 1;
            if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum > ai.minimum) {
                p->x0 = ai.minimum; p->x1 = ai.maximum;
            }
            if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum > ai.minimum) {
                p->y0 = ai.minimum; p->y1 = ai.maximum;
            }
        }
        nptr++;

        if (debug_on) {
            char name[128] = "";
            ioctl(fd, EVIOCGNAME(sizeof name), name);
            printf("hartwell-os: pointer on %s (%s) name=%s range=%d..%d x %d..%d\n",
                   path, p->abs ? "absolute" : "relative", name,
                   p->x0, p->x1, p->y0, p->y1);
            fflush(stdout);
        }
    }
    if (!ptr_visible) {
        ptr_x = W / 2;
        ptr_y = H / 2;
    }
}

static void mouse_read(int idx)
{
    PtrDev *p = &ptrs[idx];
    struct input_event ev;
    int moved = 0, clicked = 0;
    while (read(p->fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (debug_on) {
            printf("pointer ev %d type=%d code=%d value=%d\n", idx, ev.type, ev.code, ev.value);
            fflush(stdout);
        }
        if (ev.type == EV_REL) {
            if (ev.code == REL_X) { ptr_x += ev.value; moved = 1; }
            else if (ev.code == REL_Y) { ptr_y += ev.value; moved = 1; }
        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) {
                ptr_x = p->x1 > p->x0 ? (ev.value - p->x0) * (W - 1) / (p->x1 - p->x0) : ev.value;
                moved = 1;
            } else if (ev.code == ABS_Y) {
                ptr_y = p->y1 > p->y0 ? (ev.value - p->y0) * (H - 1) / (p->y1 - p->y0) : ev.value;
                moved = 1;
            }
        } else if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == BTN_TOUCH) && ev.value == 1) {
            clicked = 1;
        }
    }
    if (ptr_x < 0) ptr_x = 0;
    if (ptr_y < 0) ptr_y = 0;
    if (ptr_x >= W) ptr_x = W - 1;
    if (ptr_y >= H) ptr_y = H - 1;
    if (!moved && !clicked) return;
    ptr_visible = 1;
    if (clicked) hit_click();
    redraw();
}

static void key_press(const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];

        if (c == 0x1b) {
            /* escape sequence: F1..F3 = ESC O P/Q/R */
            if (i + 2 < n && (buf[i + 1] == 'O' || buf[i + 1] == '[')) {
                char f = buf[i + 2];
                if (f == 'P') page = 0;
                if (f == 'Q') page = 1;
                if (f == 'R') page = 2;
                i += 2;
                continue;
            }
            typed[0] = 0;
            msg[0] = 0;
            continue;
        }
        /* F1..F4 always navigate, even while the key field has focus */
        if (c >= 0x01 && c <= 0x04) { page = c - 1; continue; }

        int typing = (page == 2 && !is_licensed);

        if (!typing) {
            if (c == '1') { page = 0; continue; }
            if (c == '2') { page = 1; continue; }
            if (c == '3') { page = 2; continue; }
            if (c == '4') { page = 3; continue; }
            if (c == 'r' || c == 'R') { load_content(); continue; }
        }

        if (!typing) continue;

        if (c == '\r' || c == '\n') {
            activate();
            continue;
        }
        if (c == 0x7f || c == 0x08) {
            size_t l = strlen(typed);
            if (l) typed[l - 1] = 0;
            msg[0] = 0;
            continue;
        }
        if (c >= 32 && c <= 126) {
            if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);   /* keys are uppercase */
            size_t l = strlen(typed);
            if (l < sizeof typed - 1) {
                typed[l] = (char)c;
                typed[l + 1] = 0;
            }
            msg[0] = 0;
        }
    }
}

int fbui_run(void)
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) { hwo_note('F', "F1001", "cannot open /dev/fb0"); return -1; }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vi) != 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fi) != 0) {
        hwo_note('F', "F1001", "framebuffer reports no usable mode");
        close(fb_fd);
        return -1;
    }
    W = (int)vi.xres;
    H = (int)vi.yres;
    stride = (int)fi.line_length;
    bytespp = (int)(vi.bits_per_pixel / 8);
    if (bytespp < 2) {
        hwo_note('F', "F1002", "%d bpp is not supported", vi.bits_per_pixel);
        close(fb_fd);
        return -1;
    }

    fb = mmap(NULL, (size_t)stride * H, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb == MAP_FAILED) { close(fb_fd); return -1; }
    cv = calloc(1, (size_t)stride * H);
    if (!cv) { munmap(fb, (size_t)stride * H); close(fb_fd); return -1; }

    tty_fd = open("/dev/tty0", O_RDWR | O_NONBLOCK);
    if (tty_fd < 0) tty_fd = open("/dev/console", O_RDWR | O_NONBLOCK);
    if (tty_fd >= 0) {
        write(tty_fd, "\033[?25l\033[2J", 9);       /* cursor off, clear text console */
        ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);      /* stop the console drawing over us */
    }

    {
        char cmdline[512];
        if (hwo_read("/proc/cmdline", cmdline, sizeof cmdline) == 0 && strstr(cmdline, "hwo.debug=1"))
            debug_on = 1;
    }

    hwo_device_id(dev_id);
    is_licensed = hwo_licensed(lic_key, sizeof lic_key);
    load_content();
    page = is_licensed ? 0 : 2;

    input_init();
    mouse_init();
    media_scan();
    media_scan_ms = ms_now();
    media_last_ms = media_scan_ms;

    int last_sec = -1;
    for (;;) {
        int timeout = 250;
        if (page == 3) {
            long long now = ms_now();
            if (now - media_scan_ms > 5000) {
                media_scan_ms = now;
                media_scan();
                if (media_frames && media_index >= media_frames) media_index = 0;
            }
            if (media_frames > 0) {
                timeout = 1000 / media_fps;
                if (timeout < 10) timeout = 10;
            }
        }
        struct pollfd pfds[2 + MAX_PTR] = {
            { .fd = in_fd,  .events = POLLIN },
            { .fd = tty_fd, .events = POLLIN },
        };
        for (int i = 0; i < nptr; i++) {
            pfds[2 + i].fd = ptrs[i].fd;
            pfds[2 + i].events = POLLIN;
        }
        int r = poll(pfds, 2 + nptr, timeout);
        /* devices plugged in after boot (USB mouse, touch panel) */
        if (nptr < MAX_PTR) mouse_init();
        if (r > 0 && in_fd >= 0 && (pfds[0].revents & POLLIN)) {
            char kbuf[64];
            int kn = read_evdev(kbuf, (int)sizeof kbuf);
            if (kn > 0) {
                key_press(kbuf, kn);
                redraw();
            }
        }
        if (r > 0 && tty_fd >= 0 && (pfds[1].revents & POLLIN)) {
            char buf[64];
            ssize_t n = read(tty_fd, buf, sizeof buf);
            if (n > 0) {
                key_press(buf, (int)n);
                redraw();
            }
        }
        if (r > 0)
            for (int i = 0; i < nptr; i++)
                if (pfds[2 + i].revents & POLLIN) mouse_read(i);

        if (page == 3 && media_frames > 0) {
            long long now = ms_now();
            if (now - media_last_ms >= 1000 / media_fps) {
                media_last_ms = now;
                media_load((media_index + 1) % media_frames);
                redraw();
            }
        }

        time_t now = time(NULL);
        if ((int)now != last_sec) {
            last_sec = (int)now;
            if (!is_licensed) is_licensed = hwo_licensed(lic_key, sizeof lic_key);
            redraw();
        }
    }
    return 0;
}
