#!/bin/sh
# Fetch the two upstream artefacts build.sh needs into buildroot/.
#
# A clean checkout cannot build: buildroot/ is ignored (284 MB of Alpine base
# and kernel), so without this step `./build.sh` dies on the first tar. Both
# files are pinned to an exact version and checked against a recorded SHA256,
# so the image a customer builds is the image that was tested.
#
#   ./tools/fetch-base.sh          # only fetches what is missing
#   ./tools/fetch-base.sh --force  # re-download everything
set -e
cd "$(dirname "$0")/.."

ALPINE=v3.22
MINIROOTFS=alpine-minirootfs-3.22.8-x86_64.tar.gz
MINIROOTFS_SHA=2d7f46af9b7e86ad64ebeba3fe9b75b62dd5b259e708afb2ab21268f627696e5
KPKG=linux-lts-6.12.119-r0.apk
KPKG_SHA=de8ea6b94d2ba64f58db983f5eb3c3dd3634be8f20f0bdca4e60cd1cd0a02464

REL=https://dl-cdn.alpinelinux.org/alpine/$ALPINE/releases/x86_64
MAIN=https://dl-cdn.alpinelinux.org/alpine/$ALPINE/main/x86_64
OUT=buildroot
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$OUT"

need_kernel=0
[ "$1" = "--force" ] && need_kernel=1
[ ! -f "$OUT/vmlinuz-lts" ] && need_kernel=1
[ ! -d "$OUT/modules/lib/modules" ] && need_kernel=1

if [ "$need_kernel" = 1 ]; then
    echo "==> fetching $KPKG (123 MB)"
    curl -fL --retry 3 -o "$TMP/$KPKG" "$MAIN/$KPKG"
    echo "$KPKG_SHA  $TMP/$KPKG" | sha256sum -c - > /dev/null || {
        echo "checksum mismatch for $KPKG" >&2; exit 1; }
    # an .apk is a tar of signature + control + data streams; tar reads them all
    rm -rf "$OUT/modules"
    tar xzf "$TMP/$KPKG" -C "$TMP" 2>/dev/null
    cp "$TMP/boot/vmlinuz-lts" "$OUT/vmlinuz-lts"
    # stage_modules.py walks buildroot/modules/lib/modules/<version>
    mkdir -p "$OUT/modules/lib"
    cp -a "$TMP/lib/modules" "$OUT/modules/lib/"
    echo "==> kernel $(ls "$OUT/modules/lib/modules")"
fi

if [ ! -f "$OUT/alpine-minirootfs.tar.gz" ] || [ "$1" = "--force" ]; then
    echo "==> fetching $MINIROOTFS"
    curl -fL --retry 3 -o "$TMP/rootfs.tar.gz" "$REL/$MINIROOTFS"
    echo "$MINIROOTFS_SHA  $TMP/rootfs.tar.gz" | sha256sum -c - > /dev/null || {
        echo "checksum mismatch for $MINIROOTFS" >&2; exit 1; }
    cp "$TMP/rootfs.tar.gz" "$OUT/alpine-minirootfs.tar.gz"
fi

echo "==> buildroot ready"
