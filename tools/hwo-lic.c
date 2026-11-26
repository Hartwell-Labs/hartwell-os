/* hwo-lic — vendor-side licence tool for Hartwell OS.
 *
 * Signs a device id with EdDSA (curve25519 + BLAKE2b, Monocypher) so the unit can
 * verify the licence offline with an embedded public key. The private seed never
 * leaves the vendor; the appliance only carries the public key.
 *
 *   hwo-lic gen    <dir>                 create <dir>/hwo-lic.key (seed) + .pub
 *   hwo-lic sign   <dir> <device>        print device=/licence= for a unit
 *   hwo-lic verify <dir> <device> <hex>  check a signature
 *   hwo-lic make-update <dir> <ver> <image>    signed update bundle (kernel+initrd
 *                                              for a directory, initramfs only for a file)
 *
 * Build (host):  cc -O2 -o tools/hwo-lic tools/hwo-lic.c src/monocypher.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "../src/monocypher.h"

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int from_hex(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void to_hex(const uint8_t *in, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int read_file(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t got = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got] = 0;
    while (got && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) buf[--got] = 0;
    return 0;
}

static int keypair_from_dir(const char *dir, uint8_t sk[64], uint8_t pk[32]) {
    char path[512], hex[256];
    snprintf(path, sizeof path, "%s/hwo-lic.key", dir);
    if (read_file(path, hex, sizeof hex) != 0) {
        fprintf(stderr, "cannot read %s (run: hwo-lic gen %s)\n", path, dir);
        return -1;
    }
    uint8_t seed[32];
    if (from_hex(hex, seed, 32) != 0) { fprintf(stderr, "bad seed in %s\n", path); return -1; }
    crypto_eddsa_key_pair(sk, pk, seed);
    crypto_wipe(seed, sizeof seed);
    return 0;
}

static int cmd_gen(const char *dir) {
    uint8_t seed[32], sk[64], pk[32];
    FILE *r = fopen("/dev/urandom", "rb");
    if (!r || fread(seed, 1, 32, r) != 32) { fprintf(stderr, "no entropy\n"); if (r) fclose(r); return 1; }
    fclose(r);
    uint8_t seed_copy[32];
    memcpy(seed_copy, seed, 32);
    crypto_eddsa_key_pair(sk, pk, seed);

    char path[512], hex[256];
    snprintf(path, sizeof path, "%s/hwo-lic.key", dir);
    FILE *k = fopen(path, "w");
    if (!k) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    to_hex(seed_copy, 32, hex); fprintf(k, "%s\n", hex); fclose(k);
    chmod(path, 0600);

    snprintf(path, sizeof path, "%s/hwo-lic.pub", dir);
    FILE *p = fopen(path, "w");
    if (!p) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    to_hex(pk, 32, hex); fprintf(p, "%s\n", hex); fclose(p);

    crypto_wipe(seed, sizeof seed);
    crypto_wipe(sk, sizeof sk);
    printf("wrote %s/hwo-lic.key (0600) and %s/hwo-lic.pub\n", dir, dir);
    return 0;
}

static int cmd_sign(const char *dir, const char *device) {
    uint8_t sk[64], pk[32], sig[64];
    if (keypair_from_dir(dir, sk, pk) != 0) return 1;
    crypto_eddsa_sign(sig, sk, (const uint8_t *)device, strlen(device));
    char hex[160];
    to_hex(sig, 64, hex);
    printf("device=%s\nlicence=%s\n", device, hex);
    crypto_wipe(sk, sizeof sk);
    return 0;
}

static int cmd_verify(const char *dir, const char *device, const char *sighex) {
    char path[512];
    uint8_t pk[32], sig[64];
    snprintf(path, sizeof path, "%s/hwo-lic.pub", dir);
    char hex[256];
    if (read_file(path, hex, sizeof hex) != 0 || from_hex(hex, pk, 32) != 0) {
        fprintf(stderr, "cannot read public key %s\n", path);
        return 1;
    }
    if (strlen(sighex) != 128 || from_hex(sighex, sig, 64) != 0) {
        fprintf(stderr, "signature must be 128 hex chars\n");
        return 2;
    }
    int r = crypto_eddsa_check(sig, pk, (const uint8_t *)device, strlen(device));
    printf("%s\n", r == 0 ? "valid" : "INVALID");
    return r == 0 ? 0 : 3;
}


static int hash_path(const char *path, char *hex, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);
    unsigned char buf[65536];
    size_t n;
    long total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        crypto_blake2b_update(&ctx, buf, n);
        total += (long)n;
    }
    fclose(f);
    uint8_t digest[32];
    crypto_blake2b_final(&ctx, digest);
    to_hex(digest, 32, hex);
    if (len) *len = total;
    return 0;
}

static int append_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        fwrite(buf, 1, n, stdout);
    fclose(f);
    return 0;
}

/* Make a signed update bundle: manifest + payload on stdout.
 *
 *   hwo-lic make-update <dir> <version> <image> > hwo-1.1.0.bundle
 *
 * <image> is either a directory holding vmlinuz and initrd (a full A/B slot
 * payload: kernel and initramfs), or a single initramfs file (the old, v1
 * bundle that leaves the kernel alone). Hashes are BLAKE2b-256 and the
 * signature covers the manifest lines exactly as the target rebuilds them, so
 * verification needs no parsing ambushes on the receiving side. */
static int cmd_make_update(const char *dir, const char *ver, const char *what) {
    uint8_t sk[64], pk[32];
    if (keypair_from_dir(dir, sk, pk) != 0) return 1;

    struct stat st;
    int two = (stat(what, &st) == 0 && S_ISDIR(st.st_mode));
    char kpath[512], ipath[512];
    snprintf(kpath, sizeof kpath, "%s/vmlinuz", what);
    snprintf(ipath, sizeof ipath, "%s/initrd", what);
    if (!two) snprintf(ipath, sizeof ipath, "%s", what);

    char ihash[65], khash[65], sig[129], msg[512];
    long klen = 0;
    if (hash_path(ipath, ihash, NULL) != 0) return 1;
    if (two) {
        if (hash_path(kpath, khash, &klen) != 0) return 1;
        snprintf(msg, sizeof msg,
                 "HWO-UPDATE 2\nversion=%s\nkernel=%s\nkernellen=%ld\ninitrd=%s\n",
                 ver, khash, klen, ihash);
    } else {
        snprintf(msg, sizeof msg, "HWO-UPDATE 1\nversion=%s\nhash=%s\n", ver, ihash);
    }

    uint8_t sigbin[64];
    crypto_eddsa_sign(sigbin, sk, (const uint8_t *)msg, strlen(msg));
    to_hex(sigbin, 64, sig);

    printf("%ssig=%s\n\n", msg, sig);
    if (two && append_file(kpath) != 0) return 1;
    return append_file(ipath);
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "gen") == 0) return cmd_gen(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "sign") == 0) return cmd_sign(argv[2], argv[3]);
    if (argc >= 5 && strcmp(argv[1], "verify") == 0) return cmd_verify(argv[2], argv[3], argv[4]);
    if (argc >= 5 && strcmp(argv[1], "make-update") == 0)
        return cmd_make_update(argv[2], argv[3], argv[4]);
    fprintf(stderr,
            "usage:\n"
            "  hwo-lic gen <dir>\n"
            "  hwo-lic sign <dir> <device>\n"
            "  hwo-lic verify <dir> <device> <hex-signature>\n"
            "  hwo-lic make-update <dir> <version> <image-dir|initrd> > BUNDLE\n");
    return 64;
}
