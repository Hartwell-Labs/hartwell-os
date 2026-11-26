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
 * The boot partition (LABEL=HWBOOT, mounted at /boot by init) carries two slots,
 * /boot/slot-a and /boot/slot-b, each holding an initrd, and /boot/state records
 * which slot is active, which one it replaced, and how many boot attempts the
 * new slot still has. An update is written into the *inactive* slot and only then
 * made active, so the running image is never overwritten. Until the new image
 * confirms itself (hwo bootconfirm, called by init once the UI has been up for a
 * while) the attempt counter runs down; when it reaches zero the next boot puts
 * the previous slot back. A live ISO has no boot partition and says so (E9003)
 * instead of pretending to update.
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

/* how many times a freshly installed slot may fail to confirm before the
 * bootloader is pointed back at the slot it replaced */
#define SLOT_TRIES 3

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

static const char *boot_root(void)
{
    const char *b = getenv("HWO_BOOT_DIR");
    return b ? b : BOOT_DIR;
}

/* boot slots: a, b, their state file, loader.conf default line */
struct bootstate {
    char active[8];
    char previous[8];
    int tries;      /* attempts left for active; -1 once confirmed */
};

static void state_path(char *out, size_t n)
{
    snprintf(out, n, "%s/state", boot_root());
}

static int read_state(struct bootstate *s)
{
    snprintf(s->active, sizeof s->active, "a");
    snprintf(s->previous, sizeof s->previous, "b");
    s->tries = -1;

    char path[512];
    state_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "active=", 7)) {
            char *v = line + 7; v[strcspn(v, "\r\n")] = 0;
            snprintf(s->active, sizeof s->active, "%.7s", v);
        } else if (!strncmp(line, "previous=", 9)) {
            char *v = line + 9; v[strcspn(v, "\r\n")] = 0;
            snprintf(s->previous, sizeof s->previous, "%.7s", v);
        } else if (!strncmp(line, "tries=", 6)) {
            s->tries = atoi(line + 6);
        }
    }
    fclose(f);
    return 0;
}

static int write_state(const struct bootstate *s)
{
    char path[512], tmp[520];
    state_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "active=%s\nprevious=%s\n", s->active, s->previous);
    if (s->tries >= 0) fprintf(f, "tries=%d\n", s->tries);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Point the boot menu at a slot.
 *
 * systemd-boot in these images does not honour `default`: it boots whichever
 * entry sorts last, which on an unattended unit means the menu decides the
 * image. So the one live entry, zz-hartwell.conf, is rewritten in place and the
 * slot it boots is therefore what /boot/state says. aa-rescue.conf sorts first
 * and is only reachable by hand from the menu.
 */
#define ENTRY_REL "loader/entries/zz-hartwell.conf"

static int slot_has_kernel(const char *slot)
{
    char p[600];
    snprintf(p, sizeof p, "%s/slot-%s/vmlinuz", boot_root(), slot);
    return access(p, R_OK) == 0;
}

static int point_loader_at(const char *slot, int with_kernel)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", boot_root(), ENTRY_REL);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;   /* not a UEFI install, nothing to point */
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0) return 0;
    buf[n] = 0;

    char out[1200];
    size_t o = 0;
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
        char one[600];
        if (len >= sizeof one) len = sizeof one - 1;
        memcpy(one, line, len);
        one[len] = 0;

        if (!strncmp(one, "initrd", 6)) {
            o += (size_t)snprintf(out + o, sizeof out - o, "initrd /slot-%s/initrd\n", slot);
        } else if (!strncmp(one, "linux", 5)) {
            char lp[64];
            if (with_kernel) snprintf(lp, sizeof lp, "/slot-%s/vmlinuz", slot);
            else             snprintf(lp, sizeof lp, "/vmlinuz");
            o += (size_t)snprintf(out + o, sizeof out - o, "linux %s\n", lp);
        } else if (!strncmp(one, "options", 7)) {
            char opts[600];
            snprintf(opts, sizeof opts, "%s", one + 7);
            char *p = strstr(opts, "hwo.slot=");
            if (p) p[9] = slot[0];
            else {
                char *e = opts + strcspn(opts, "\r\n");
                snprintf(e, sizeof opts - (size_t)(e - opts), " hwo.slot=%s", slot);
            }
            o += (size_t)snprintf(out + o, sizeof out - o, "options%s\n", opts);
        } else if (o + len < sizeof out) {
            memcpy(out + o, one, len);
            o += len;
        }
        if (!nl) break;
        line = nl + 1;
    }
    out[o] = 0;

    char tmp[520];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) return -1;
    ssize_t w = write(ofd, out, o);
    fsync(ofd);
    close(ofd);
    if (w != (ssize_t)o || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 1;
}

static int write_version(const char *slot, const char *ver)
{
    char path[520], tmp[530];
    if (slot) snprintf(path, sizeof path, "%s/ver-%s", boot_root(), slot);
    else      snprintf(path, sizeof path, "%s/version.txt", boot_root());
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    char line[64];
    int n = snprintf(line, sizeof line, "%s\n", ver);
    ssize_t w = write(fd, line, (size_t)n);
    fsync(fd);
    close(fd);
    if (w != n || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int installed_version(char *out, size_t n)
{
    struct bootstate st;
    char path[512];
    read_state(&st);
    snprintf(path, sizeof path, "%s/ver-%s", boot_root(), st.active);
    int fd = open(path, O_RDONLY);
    if (fd < 0) snprintf(path, sizeof path, "%s/version.txt", boot_root()), fd = open(path, O_RDONLY);
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

    char verbuf[32], hashbuf[96], sigbuf[160], ibuf[96], kbuf[96], klenbuf[32];
    const char *ver  = field(hdr, "version", verbuf, sizeof verbuf);
    const char *hash = field(hdr, "hash", hashbuf, sizeof hashbuf);
    const char *sig  = field(hdr, "sig", sigbuf, sizeof sigbuf);
    const char *ihash = field(hdr, "initrd", ibuf, sizeof ibuf);
    if (!ihash) ihash = hash;
    const char *khash = field(hdr, "kernel", kbuf, sizeof kbuf);
    const char *klens = field(hdr, "kernellen", klenbuf, sizeof klenbuf);
    long klen = klens ? strtol(klens, NULL, 10) : -1;
    /* v2 bundles carry the kernel too; v1 replaced the initramfs only and are
     * still accepted so an older signed bundle keeps working */
    int with_kernel = (klen > 0 && khash && strlen(khash) == 64);
    if (!ver || !ihash || strlen(ihash) != 64 || !sig || (klen > 0 && !with_kernel)) {
        close(fd);
        hwo_note('E', "E9004", "manifest missing version/hash/sig");
        return 1;
    }

    /* the signature covers the manifest lines, in the order they are produced */
    char msg[HDR_MAX + 64];
    if (with_kernel)
        snprintf(msg, sizeof msg,
                 "HWO-UPDATE 2\nversion=%s\nkernel=%s\nkernellen=%ld\ninitrd=%s\n",
                 ver, khash, klen, ihash);
    else
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

    struct bootstate st;
    read_state(&st);
    /* never touch the running slot: the new image goes to the other one */
    const char *target = !strcmp(st.active, "a") ? "b" : "a";
    char out[600], tmp[620], sdir[512], kout[600], ktmp[620];
    snprintf(sdir, sizeof sdir, "%s/slot-%s", boot_root(), target);
    if (mkdir(sdir, 0755) != 0 && errno != EEXIST) {
        close(fd);
        hwo_note('E', "E9003", "no writable boot partition at %s (%s)", sdir, strerror(errno));
        return 1;
    }
    snprintf(out, sizeof out, "%s/initrd", sdir);
    snprintf(tmp, sizeof tmp, "%s/initrd.new", sdir);
    snprintf(kout, sizeof kout, "%s/vmlinuz", sdir);
    snprintf(ktmp, sizeof ktmp, "%s/vmlinuz.new", sdir);

    int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) {
        close(fd);
        hwo_note('E', "E9003", "no writable boot partition at %s (%s)", sdir, strerror(errno));
        return 1;
    }

    char buf[65536];
    size_t total = 0;
    ssize_t r;

    /* v2: the payload starts with the kernel, exactly kernellen bytes long */
    unsigned char kdigest[32];
    char kgot[65] = "";
    if (with_kernel) {
        int kfd = open(ktmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (kfd < 0) {
            close(ofd); close(fd); unlink(tmp);
            hwo_note('E', "E9003", "cannot write %s (%s)", ktmp, strerror(errno));
            return 1;
        }
        crypto_blake2b_ctx kctx;
        crypto_blake2b_init(&kctx, 32);
        long left = klen;
        while (left > 0 && (r = read(fd, buf, (size_t)(left < (long)sizeof buf ? left : (long)sizeof buf))) > 0) {
            crypto_blake2b_update(&kctx, (const unsigned char *)buf, (size_t)r);
            if (write(kfd, buf, (size_t)r) != r) {
                close(kfd); close(ofd); close(fd); unlink(tmp); unlink(ktmp);
                hwo_note('E', "E9004", "write failed to %s: %s", ktmp, strerror(errno));
                return 1;
            }
            left -= r;
        }
        fsync(kfd);
        close(kfd);
        crypto_blake2b_final(&kctx, kdigest);
        to_hex(kdigest, 32, kgot);
        if (left != 0 || strcmp(kgot, khash) != 0) {
            close(ofd); close(fd); unlink(tmp); unlink(ktmp);
            hwo_note('E', "E9001", "update %s rejected: kernel hash", ver);
            return 1;
        }
    }

    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        crypto_blake2b_update(&ctx, (const unsigned char *)buf, (size_t)r);
        if (write(ofd, buf, (size_t)r) != r) {
            close(ofd);
            close(fd);
            unlink(tmp);
            if (with_kernel) unlink(ktmp);
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
    if (strcmp(got, ihash) != 0) {
        unlink(tmp);
        if (with_kernel) unlink(ktmp);
        hwo_note('E', "E9001", "update %s rejected: payload hash", ver);
        return 1;
    }

    if (rename(tmp, out) != 0) {
        unlink(tmp);
        if (with_kernel) unlink(ktmp);
        hwo_note('E', "E9004", "cannot swap %s into place: %s", out, strerror(errno));
        return 1;
    }
    if (with_kernel && rename(ktmp, kout) != 0) {
        unlink(ktmp);
        hwo_note('E', "E9004", "cannot swap %s into place: %s", kout, strerror(errno));
        return 1;
    }
    int slot_kernel = with_kernel;
    if (!with_kernel) {
        /* v1 bundle: the slot has no kernel of its own yet, so it borrows the
         * one the unit is running; the entry then boots the slot's copy */
        char src[600];
        snprintf(src, sizeof src, "%s/slot-%s/vmlinuz", boot_root(), st.active);
        int sfd = open(src, O_RDONLY);
        int dfd = sfd >= 0 ? open(kout, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
        if (dfd >= 0) {
            ssize_t n;
            while ((n = read(sfd, buf, sizeof buf)) > 0) {
                if (write(dfd, buf, (size_t)n) != n) break;
            }
            fsync(dfd);
            close(dfd);
        }
        if (sfd >= 0) close(sfd);
        slot_kernel = slot_has_kernel(target);
    }

    write_version(target, ver);
    write_version(NULL, ver);

    /* state is written last: it is what makes the new slot bootable, and what
     * makes the next boot fall back if the new image never confirms itself */
    char was[8];
    snprintf(was, sizeof was, "%s", st.active);
    snprintf(st.previous, sizeof st.previous, "%s", was);
    snprintf(st.active, sizeof st.active, "%s", target);
    st.tries = SLOT_TRIES;
    if (write_state(&st) != 0) {
        hwo_note('E', "E9003", "cannot record slot state on the boot partition");
        return 1;
    }
    point_loader_at(target, slot_kernel);

    sync();
    printf("update %s applied (%s + %zu byte initramfs) into slot %s; reboot to run it\n"
           "slot %s is kept as the fallback until the new image confirms\n",
           ver, with_kernel ? "kernel" : "initramfs only", total, target, was);
    return 0;
}

/* called by init once the UI has been running for a while: the slot works, so
 * stop counting attempts and keep it */
int hwo_boot_confirm(void)
{
    struct bootstate st;
    if (read_state(&st) != 0) {
        puts("no boot state, nothing to confirm");
        return 0;
    }
    if (st.tries < 0) {
        printf("slot %s already confirmed\n", st.active);
        return 0;
    }
    st.tries = -1;
    if (write_state(&st) != 0) {
        hwo_note('E', "E9003", "cannot confirm slot %s: boot partition read-only", st.active);
        return 1;
    }
    sync();
    printf("slot %s confirmed\n", st.active);
    return 0;
}

/* put the slot that was running before the last update back in charge */
int hwo_boot_rollback(void)
{
    struct bootstate st;
    if (read_state(&st) != 0) {
        puts("no boot state, nothing to roll back");
        return 0;
    }
    if (!strcmp(st.active, st.previous)) {
        printf("already running the fallback slot %s\n", st.active);
        return 0;
    }
    char back[8];
    snprintf(back, sizeof back, "%s", st.previous);
    snprintf(st.previous, sizeof st.previous, "%s", st.active);
    snprintf(st.active, sizeof st.active, "%s", back);
    st.tries = -1;
    if (write_state(&st) != 0) {
        hwo_note('E', "E9003", "cannot roll back: boot partition read-only");
        return 1;
    }
    point_loader_at(back, slot_has_kernel(back));
    char v[32];
    installed_version(v, sizeof v);
    sync();
    printf("rolled back to slot %s (%s); reboot to run it\n", back, v);
    return 0;
}
