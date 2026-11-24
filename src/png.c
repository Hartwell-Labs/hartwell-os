/* Minimal PNG decoder (8-bit, non-interlaced) with a self-contained inflate.
 * Used by the media layer; no zlib or libpng on the appliance. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"

/* ---------- inflate (RFC 1951) ---------- */

typedef struct {
    const unsigned char *src;
    size_t len, pos;
    unsigned bitbuf;
    int bitcnt;
} In;

static int getbits(In *s, int need)
{
    while (s->bitcnt < need) {
        if (s->pos >= s->len) return -1;
        s->bitbuf |= (unsigned)s->src[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

typedef struct {
    unsigned short count[16];
    unsigned short sym[320];
} Huff;

static int huff_build(Huff *h, const unsigned char *lens, int n)
{
    memset(h, 0, sizeof *h);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;

    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;              /* over-subscribed */
    }
    unsigned short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (unsigned short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (unsigned short)i;
    return 0;
}

static int huff_decode(In *s, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = getbits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

typedef struct {
    unsigned char *p;
    size_t n, cap;
} Out;

static int out_put(Out *o, unsigned char b)
{
    if (o->n == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 65536;
        unsigned char *p = realloc(o->p, cap);
        if (!p) return -1;
        o->p = p;
        o->cap = cap;
    }
    o->p[o->n++] = b;
    return 0;
}

static const unsigned short len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                             35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                             3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                              193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                              4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inflate_codes(In *s, const Huff *lit, const Huff *dist, Out *o)
{
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (out_put(o, (unsigned char)sym)) return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = len_base[sym] + getbits(s, len_extra[sym]);
            int dsym = huff_decode(s, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            size_t d = (size_t)dist_base[dsym] + (size_t)getbits(s, dist_extra[dsym]);
            if (d == 0 || d > o->n) return -1;
            for (int i = 0; i < len; i++)
                if (out_put(o, o->p[o->n - d])) return -1;
        }
    }
}

static int inflate_block(In *s, Out *o)
{
    int final = getbits(s, 1);
    int type = getbits(s, 2);
    if (final < 0 || type < 0) return -1;

    if (type == 0) {
        s->bitbuf = 0;
        s->bitcnt = 0;
        if (s->pos + 4 > s->len) return -1;
        unsigned len = (unsigned)s->src[s->pos] | ((unsigned)s->src[s->pos + 1] << 8);
        s->pos += 4;
        if (s->pos + len > s->len) return -1;
        for (unsigned i = 0; i < len; i++)
            if (out_put(o, s->src[s->pos + i])) return -1;
        s->pos += len;
    } else if (type == 1) {
        unsigned char lens[288];
        int i = 0;
        for (; i < 144; i++) lens[i] = 8;
        for (; i < 256; i++) lens[i] = 9;
        for (; i < 280; i++) lens[i] = 7;
        for (; i < 288; i++) lens[i] = 8;
        Huff lit, dist;
        if (huff_build(&lit, lens, 288)) return -1;
        for (i = 0; i < 30; i++) lens[i] = 5;
        if (huff_build(&dist, lens, 30)) return -1;
        if (inflate_codes(s, &lit, &dist, o)) return -1;
    } else if (type == 2) {
        int hlit = getbits(s, 5) + 257;
        int hdist = getbits(s, 5) + 1;
        int hclen = getbits(s, 4) + 4;
        if (hlit > 286 || hdist > 30) return -1;
        static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        unsigned char clen[19];
        memset(clen, 0, sizeof clen);
        for (int i = 0; i < hclen; i++) {
            int v = getbits(s, 3);
            if (v < 0) return -1;
            clen[order[i]] = (unsigned char)v;
        }
        Huff cl;
        if (huff_build(&cl, clen, 19)) return -1;

        unsigned char lens[320];
        int n = 0;
        while (n < hlit + hdist) {
            int sym = huff_decode(s, &cl);
            if (sym < 0) return -1;
            if (sym < 16) {
                lens[n++] = (unsigned char)sym;
            } else {
                int rep, val = 0;
                if (sym == 16) {
                    if (n == 0) return -1;
                    val = lens[n - 1];
                    rep = 3 + getbits(s, 2);
                } else if (sym == 17) {
                    rep = 3 + getbits(s, 3);
                } else {
                    rep = 11 + getbits(s, 7);
                }
                if (rep < 0 || n + rep > hlit + hdist) return -1;
                while (rep--) lens[n++] = (unsigned char)val;
            }
        }
        Huff lit, dist;
        if (huff_build(&lit, lens, hlit)) return -1;
        if (huff_build(&dist, lens + hlit, hdist)) return -1;
        if (inflate_codes(s, &lit, &dist, o)) return -1;
    } else {
        return -1;
    }
    return final;
}

/* raw zlib stream (without the 2-byte header) -> bytes */
static int zlib_inflate(const unsigned char *src, size_t len, Out *o)
{
    In s = { .src = src, .len = len, .pos = 0, .bitbuf = 0, .bitcnt = 0 };
    for (;;) {
        int r = inflate_block(&s, o);
        if (r < 0) return -1;
        if (r == 1) return 0;
    }
}

/* ---------- PNG ---------- */

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static unsigned char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *b = malloc((size_t)sz);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

int png_load(const char *path, Png *img)
{
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    size_t flen = 0;
    unsigned char *file = read_file(path, &flen);
    if (!file) return -1;
    if (flen < 8 || memcmp(file, sig, 8) != 0) { free(file); return -1; }

    unsigned w = 0, h = 0;
    int depth = 0, color = -1, interlace = 0;
    unsigned char palette[256][3];
    unsigned char alpha[256];
    int npal = 0, has_trns = 0;
    Out idat = { 0 };

    for (size_t p = 8; p + 12 <= flen; ) {
        unsigned clen = be32(file + p);
        const unsigned char *type = file + p + 4;
        const unsigned char *data = file + p + 8;
        if ((size_t)clen > flen - p - 12) break;

        if (!memcmp(type, "IHDR", 4) && clen >= 13) {
            w = be32(data);
            h = be32(data + 4);
            depth = data[8];
            color = data[9];
            interlace = data[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = (int)(clen / 3);
            if (npal > 256) npal = 256;
            for (int i = 0; i < npal; i++) {
                palette[i][0] = data[i * 3];
                palette[i][1] = data[i * 3 + 1];
                palette[i][2] = data[i * 3 + 2];
            }
        } else if (!memcmp(type, "tRNS", 4) && color == 3) {
            has_trns = 1;
            for (unsigned i = 0; i < clen && i < 256; i++) alpha[i] = data[i];
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat.n + clen > 128u * 1024 * 1024) { free(idat.p); free(file); return -1; }
            unsigned char *q = realloc(idat.p, idat.n + clen);
            if (!q) { free(idat.p); free(file); return -1; }
            idat.p = q;
            memcpy(idat.p + idat.n, data, clen);
            idat.n += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        p += 12u + clen;
    }

    if (!w || !h || w > 8192 || h > 8192 || depth != 8 || interlace != 0 || color < 0) {
        free(idat.p);
        free(file);
        return -1;
    }

    int ch = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    if (!ch || idat.n < 3) { free(idat.p); free(file); return -1; }

    Out raw = { 0 };
    int rc = zlib_inflate(idat.p + 2, idat.n - 2, &raw);
    free(idat.p);
    free(file);
    if (rc != 0) { free(raw.p); return -1; }

    size_t stride = (size_t)w * (size_t)ch;
    if (raw.n < (stride + 1) * h) { free(raw.p); return -1; }

    /* unfilter in place, row by row */
    for (unsigned y = 0; y < h; y++) {
        unsigned char *row = raw.p + y * (stride + 1);
        int ft = row[0];
        unsigned char *cur = row + 1;
        unsigned char *prev = y ? raw.p + (y - 1) * (stride + 1) + 1 : NULL;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)ch ? cur[i - ch] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= (size_t)ch) ? prev[i - ch] : 0;
            switch (ft) {
            case 0: break;
            case 1: cur[i] = (unsigned char)(cur[i] + a); break;
            case 2: cur[i] = (unsigned char)(cur[i] + b); break;
            case 3: cur[i] = (unsigned char)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (unsigned char)(cur[i] + paeth(a, b, c)); break;
            default: free(raw.p); return -1;
            }
        }
    }

    unsigned char *out = malloc((size_t)w * h * 4);
    if (!out) { free(raw.p); return -1; }

    for (unsigned y = 0; y < h; y++) {
        const unsigned char *row = raw.p + y * (stride + 1) + 1;
        for (unsigned x = 0; x < w; x++) {
            unsigned char *d = out + ((size_t)y * w + x) * 4;
            const unsigned char *s = row + (size_t)x * ch;
            switch (color) {
            case 0: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
            case 2: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
            case 3: {
                int i = s[0];
                if (i >= npal) { d[0] = d[1] = d[2] = 0; d[3] = 255; break; }
                d[0] = palette[i][0]; d[1] = palette[i][1]; d[2] = palette[i][2];
                d[3] = (has_trns && i < 256) ? alpha[i] : 255;
                break;
            }
            case 4: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
            default: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; break;
            }
        }
    }
    free(raw.p);

    img->w = (int)w;
    img->h = (int)h;
    img->rgba = out;
    return 0;
}

void png_free(Png *img)
{
    if (!img) return;
    free(img->rgba);
    img->rgba = NULL;
    img->w = img->h = 0;
}
