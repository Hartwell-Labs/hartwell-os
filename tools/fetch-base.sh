#!/bin/sh
# Fetch the upstream artefacts build.sh needs into buildroot/ (x86_64) or
# buildroot-arm64/ (aarch64).
#
# A clean checkout cannot build: buildroot/ is ignored (hundreds of MB of
# Alpine base and kernel), so without this step `./build.sh` dies on the first
# tar. Every file is pinned to an exact version and checked against a recorded
# SHA256, so the image a customer builds is the image that was tested.
#
#   ./tools/fetch-base.sh                 # x86_64
#   ./tools/fetch-base.sh --arch arm64    # aarch64
#   ./tools/fetch-base.sh --force         # re-download everything
set -e
cd "$(dirname "$0")/.."

ARCH=amd64
FORCE=0
for a in "$@"; do
    case "$a" in
        arm64|aarch64) ARCH=arm64 ;;
        --force) FORCE=1 ;;
    esac
done

ALPINE=v3.22

if [ "$ARCH" = arm64 ]; then
    OUT=buildroot-arm64
    MINIROOTFS=alpine-minirootfs-3.22.8-aarch64.tar.gz
    MINIROOTFS_SHA=36091efacd083c1cf5a8c837272530bff4fc72db1fbfdef559f52de0d890bca4
    KPKG=linux-lts-6.12.119-r0.apk
    KPKG_SHA=60374bc0d9b125bc710f952d4508fd3ad4c78523c75ab1be1ec54fc3267cd54f
    BOOTPKG=systemd-boot-257.6-r0.apk
    BOOTPKG_SHA=39671469560339495ffed0ad001a4cbbe1ef971ad87c60ca3b2eded89fd00c23
    REL=https://dl-cdn.alpinelinux.org/alpine/$ALPINE/releases/aarch64
    MAIN=https://dl-cdn.alpinelinux.org/alpine/$ALPINE/main/aarch64
else
    OUT=buildroot
    MINIROOTFS=alpine-minirootfs-3.22.8-x86_64.tar.gz
    MINIROOTFS_SHA=2d7f46af9b7e86ad64ebeba3fe9b75b62dd5b259e708afb2ab21268f627696e5
    KPKG=linux-lts-6.12.119-r0.apk
    KPKG_SHA=de8ea6b94d2ba64f58db983f5eb3c3dd3634be8f20f0bdca4e60cd1cd0a02464
    BOOTPKG=
    REL=https://dl-cdn.alpinelinux.org/alpine/$ALPINE/releases/x86_64
    MAIN=https://dl-cdn.alpinelinux.org/alpine/$ALPINE/main/x86_64
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

need_kernel=$FORCE
[ ! -f "$OUT/vmlinuz-lts" ] && need_kernel=1
[ ! -d "$OUT/modules/lib/modules" ] && need_kernel=1
[ -n "$BOOTPKG" ] && [ ! -f "$OUT/systemd-bootaa64.efi" ] && need_kernel=1

if [ "$need_kernel" = 1 ]; then
    echo "==> fetching $KPKG for $ARCH"
    curl -fL --retry 3 -o "$TMP/$KPKG" "$MAIN/$KPKG"
    echo "$KPKG_SHA  $TMP/$KPKG" | sha256sum -c - > /dev/null || {
        echo "checksum mismatch for $KPKG" >&2; exit 1; }
    rm -rf "$OUT/modules"
    tar xzf "$TMP/$KPKG" -C "$TMP" 2>/dev/null
    cp "$TMP/boot/vmlinuz-lts" "$OUT/vmlinuz-lts"
    # stage_modules.py walks <base>/modules/lib/modules/<version>
    mkdir -p "$OUT/modules/lib"
    cp -a "$TMP/lib/modules" "$OUT/modules/lib/"
    rm -rf "$TMP/lib" "$TMP/boot" "$TMP/usr"

    if [ -n "$BOOTPKG" ]; then
        echo "==> fetching $BOOTPKG"
        curl -fL --retry 3 -o "$TMP/$BOOTPKG" "$MAIN/$BOOTPKG"
        echo "$BOOTPKG_SHA  $TMP/$BOOTPKG" | sha256sum -c - > /dev/null || {
            echo "checksum mismatch for $BOOTPKG" >&2; exit 1; }
        mkdir -p "$TMP/efi"
        tar xzf "$TMP/$BOOTPKG" -C "$TMP/efi" 2>/dev/null
        cp "$TMP/efi/usr/lib/systemd/boot/efi/systemd-bootaa64.efi" "$OUT/"
    fi
    echo "==> kernel $(ls "$OUT/modules/lib/modules")"
fi

if [ ! -f "$OUT/alpine-minirootfs.tar.gz" ] || [ "$FORCE" = 1 ]; then
    echo "==> fetching $MINIROOTFS"
    curl -fL --retry 3 -o "$TMP/rootfs.tar.gz" "$REL/$MINIROOTFS"
    echo "$MINIROOTFS_SHA  $TMP/rootfs.tar.gz" | sha256sum -c - > /dev/null || {
        echo "checksum mismatch for $MINIROOTFS" >&2; exit 1; }
    cp "$TMP/rootfs.tar.gz" "$OUT/alpine-minirootfs.tar.gz"
fi

echo "==> $OUT ready"
