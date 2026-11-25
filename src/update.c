/* Signed updates.
 *
 * A bundle is a small text manifest followed by the new initramfs:
 *
 *     HWO-UPDATE 1
 *     version=1.1.0
 *     hash=<blake2b-256 of the payload, hex>
 *     sig=<ed25519 over the three lines above, hex>
 *
 *     <payload: gzip initramfs>
 *
 * The signature is checked with the vendor public key embedded in this binary
 * (the same key that signs licences), so an update can come from a USB stick,
 * an email or a fleet server without trusting the transport. A version that is
 * not strictly newer is refused, so a stolen old bundle cannot roll a unit back
 * onto a known-vulnerable image.
 *
 * Applying writes the payload to the boot partition (LABEL=HWBOOT, mounted at
 * /boot by init) as initrd.new, fsyncs, renames over initrd and only then
 * updates /boot/version.txt. A live ISO has no boot partition, and says so
 * (E9003) instead of pretending to update.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "errors.h"
#include "monocypher.h"

#ifndef HWO_LIC_PUB_HEX
#define HWO_LIC_PUB_HEX ""
#endif

#define VER_FILE "/etc/hartwell/version.txt"
#define BOOT_DIR "/boot"
#define HDR_MAX  2048

static int hex_nib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int from_hex(const char *hex, unsigned char *out, size_t n)
{
    if (!hex || strlen(hex) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nib(hex[2 * i]), lo = hex_nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static void to_hex(const unsigned char *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 15];
    }
    out[2 * n] = 0;
}

/* "1.2.3" -> 1002003, so comparison is arithmetic and not a string sort */
static int ver_num(const char *v)
{
    int a = 0, b = 0, c = 0;
    if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 1) return -1;
    return a * 1000000 + b * 1000 + c;
}

static int installed_version(char *out, size_t n)
{
    const char *boot = getenv("HWO_BOOT_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/version.txt", boot ? boot : BOOT_DIR);
    int fd = open(path, O_RDONLY);
    if (fd < 0) snprintf(path, sizeof path, "%s", VER_FILE), fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(out, n, "0.0.0"); return 0; }
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) { snprintf(out, n, "0.0.0"); return 0; }
    out[r] = 0;
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    return 0;
}

/* copies the value into out: the manifest is read three times and a static
 * buffer would hand back the last value for every field */
static const char *field(const char *hdr, const char *key, char *out, size_t on)
{
    size_t kl = strlen(key);
    const char *p = hdr;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *s = p + kl + 1, *e = strchr(s, '\n');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n >= on) n = on - 1;
            memcpy(out, s, n);
            out[n] = 0;
            return out;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

static long payload_offset(int fd)
{
    char buf[HDR_MAX + 1];
    ssize_t n = read(fd, buf, HDR_MAX);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *sep = strstr(buf, "\n\n");
    if (!sep) return -1;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    return (long)(sep - buf) + 2;
}

/* verify and apply; returns 0 on success and writes one error code otherwise */
int hwo_update(const char *bundle, int force)
{
    int fd = open(bundle, O_RDONLY);
    if (fd < 0) {
        hwo_note('E', "E9004", "cannot open update bundle %s", bundle);
        return 1;
    }

    char hdr[HDR_MAX + 1];
    long off = payload_offset(fd);
    if (off < 0) {
        close(fd);
        hwo_note('E', "E9004", "bundle has no manifest: %s", bundle);
        return 1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, hdr, (size_t)off) != off) {
        close(fd);
        hwo_note('E', "E9004", "short read on %s", bundle);
        return 1;
    }
    hdr[off] = 0;

    char verbuf[32], hashbuf[96], sigbuf[160];
    const char *ver  = field(hdr, "version", verbuf, sizeof verbuf);
    const char *hash = field(hdr, "hash", hashbuf, sizeof hashbuf);
    const char *sig  = field(hdr, "sig", sigbuf, sizeof sigbuf);
    if (!ver || !hash || !sig) {
        close(fd);
        hwo_note('E', "E9004", "manifest missing version/hash/sig");
        return 1;
    }

    /* the signature covers the manifest lines, in the order they are produced */
    char msg[HDR_MAX + 64];
    snprintf(msg, sizeof msg, "HWO-UPDATE 1\nversion=%s\nhash=%s\n", ver, hash);

    unsigned char pk[32], sg[64];
    static const char *pubhex = HWO_LIC_PUB_HEX;
    int ok = pubhex[0] && strlen(pubhex) == 64 &&
             from_hex(pubhex, pk, 32) == 0 && from_hex(sig, sg, 64) == 0 &&
             crypto_eddsa_check(sg, pk, (const unsigned char *)msg, strlen(msg)) == 0;
    if (!ok) {
        close(fd);
        hwo_note('E', "E9001", "update %s rejected: signature", ver);
        return 1;
    }

    if (lseek(fd, off, SEEK_SET) < 0) {
        close(fd);
        hwo_note('E', "E9004", "seek failed on %s", bundle);
        return 1;
    }

    char running[32];
    installed_version(running, sizeof running);
    if (!force && ver_num(ver) <= ver_num(running)) {
        close(fd);
        hwo_note('E', "E9002", "update %s is not newer than %s", ver, running);
        return 1;
    }

    const char *boot = getenv("HWO_BOOT_DIR");
    if (!boot) boot = BOOT_DIR;
    char out[512], tmp[512];
    snprintf(out, sizeof out, "%s/initrd", boot);
    snprintf(tmp, sizeof tmp, "%s/initrd.new", boot);

    int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) {
        close(fd);
        hwo_note('E', "E9003", "no writable boot partition at %s (%s)", boot, strerror(errno));
        return 1;
    }

    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);
    char buf[65536];
    size_t total = 0;
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        crypto_blake2b_update(&ctx, (const unsigned char *)buf, (size_t)r);
        if (write(ofd, buf, (size_t)r) != r) {
            close(ofd);
            close(fd);
            unlink(tmp);
            hwo_note('E', "E9004", "write failed to %s: %s", tmp, strerror(errno));
            return 1;
        }
        total += (size_t)r;
    }
    close(fd);
    fsync(ofd);
    close(ofd);

    unsigned char digest[32];
    crypto_blake2b_final(&ctx, digest);
    char got[65];
    to_hex(digest, 32, got);
    if (strcmp(got, hash) != 0) {
        unlink(tmp);
        hwo_note('E', "E9001", "update %s rejected: payload hash", ver);
        return 1;
    }

    if (rename(tmp, out) != 0) {
        unlink(tmp);
        hwo_note('E', "E9004", "cannot swap %s into place: %s", out, strerror(errno));
        return 1;
    }

    /* the version marker is written last, so a half-applied update never claims
     * to be the new version */
    snprintf(tmp, sizeof tmp, "%s/version.txt.new", boot);
    int vf = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (vf >= 0) {
        char line[64];
        int n = snprintf(line, sizeof line, "%s\n", ver);
        if (write(vf, line, (size_t)n) != n) { /* best effort, hash already matched */ }
        fsync(vf);
        close(vf);
        snprintf(out, sizeof out, "%s/version.txt", boot);
        rename(tmp, out);
    }

    sync();
    printf("update %s applied, %zu bytes; reboot to run it\n", ver, total);
    return 0;
}
