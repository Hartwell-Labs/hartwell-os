#!/bin/sh
# Build a bootable USB/disk image: FAT boot partition with both BIOS (syslinux) and
# UEFI (/EFI/BOOT/BOOTX64.EFI) loaders + HWO_CFG state partition.
#
#   ./tools/mkdisk.sh                                  # unlicensed disk, activation on first boot
#   HWO_KEY=HWO-2026-V8NS HWO_DEVICE=HWO-3DD5-B123 ./tools/mkdisk.sh
#   MEDIA_DIR=/path/to/png-frames ./tools/mkdisk.sh    # ship a frame sequence
#
# The UEFI loader is the Secure Boot-signed one from build/signed when present;
# its certificate is copied to the boot partition as EFI/hartwell-db.cer.
#
# Write the result to a stick with:  sudo dd if=dist/hartwell-os-1.0-usb.img of=/dev/sdX bs=4M
set -e
cd "$(dirname "$0")/.."

[ -f build/vmlinuz ] && [ -f build/initrd ] || { echo "run ./build.sh first" >&2; exit 1; }

BOOTMB=${BOOTMB:-100}
STATEMB=${STATEMB:-24}
OUT=${OUT:-dist/hartwell-os-1.0-usb.img}

# sign the licensed device with the vendor key, so the disk carries an
# unforgeable Ed25519 licence (not just the demo serial)
HWO_LIC_SIG=""
if [ -n "$HWO_DEVICE" ] && [ -f keys/hwo-lic.key ]; then
    [ -x build/hwo-lic ] || gcc -O2 -o build/hwo-lic tools/hwo-lic.c src/monocypher.c
    HWO_LIC_SIG=$(./build/hwo-lic sign keys "$HWO_DEVICE" | sed -n 's/^licence=//p')
fi

DOCKER_ARGS=""
MEDIA=0
if [ -n "$MEDIA_DIR" ]; then
    DOCKER_ARGS="-v $MEDIA_DIR:/media:ro"
    MEDIA=1
fi

docker run --rm -i \
    -v "$PWD/build:/b:ro" -v "$PWD/dist:/d" -v "$PWD/src:/src:ro" $DOCKER_ARGS \
    -e BOOTMB="$BOOTMB" -e STATEMB="$STATEMB" -e OUT="/d/$(basename "$OUT")" \
    -e HWO_KEY="$HWO_KEY" -e HWO_DEVICE="$HWO_DEVICE" -e HWO_LIC_SIG="$HWO_LIC_SIG" -e MEDIA="$MEDIA" \
    alpine:latest sh -s <<'INNER'
set -e
apk add --no-cache dosfstools mtools syslinux sfdisk >/dev/null 2>&1
cd /tmp

# --- boot partition: FAT16 + kernel/initrd + syslinux ------------------------
# Files go in with mtools first and syslinux installs the boot sector last: syslinux
# -i writes ldlinux.sys directly, after which mtools refuses further writes.
dd if=/dev/zero of=boot.img bs=1M count="$BOOTMB" status=none
mkfs.vfat -F 16 -s 4 -n HWBOOT boot.img >/dev/null
cat > syslinux.cfg <<'CFG'
DEFAULT hartwell
PROMPT 0
TIMEOUT 20
LABEL hartwell
  KERNEL /boot/vmlinuz
  INITRD /boot/initrd
  APPEND console=tty0 quiet rdinit=/init video=1280x800
CFG
mcopy -i boot.img /usr/share/syslinux/ldlinux.c32 ::ldlinux.c32
mcopy -i boot.img syslinux.cfg ::syslinux.cfg
mmd -i boot.img ::boot
mcopy -i boot.img /b/vmlinuz ::boot/vmlinuz
mcopy -i boot.img /b/initrd  ::boot/initrd
syslinux -i boot.img

# --- same boot, for UEFI firmware: syslinux.efi as the removable-media path --
# UEFI finds /EFI/BOOT/BOOTX64.EFI on any FAT partition and reads syslinux.cfg from
# that directory, so the kernel and initrd live next to it.
mmd -i boot.img ::EFI ::EFI/BOOT
if [ -f /b/signed/BOOTX64.EFI ]; then
    LOADER=/b/signed/BOOTX64.EFI
else
    LOADER=/usr/share/syslinux/efi64/syslinux.efi
fi
mcopy -i boot.img "$LOADER" ::EFI/BOOT/BOOTX64.EFI
if [ -f /b/signed/db.cer ]; then
    mcopy -i boot.img /b/signed/db.cer ::EFI/hartwell-db.cer
    mcopy -i boot.img /b/signed/db.cer ::hartwell-db.cer
fi
mcopy -i boot.img /usr/share/syslinux/efi64/ldlinux.e64   ::EFI/BOOT/ldlinux.e64
mcopy -i boot.img /b/vmlinuz ::EFI/BOOT/vmlinuz
mcopy -i boot.img /b/initrd  ::EFI/BOOT/initrd
cat > efi.cfg <<'CFG'
DEFAULT hartwell
PROMPT 0
TIMEOUT 20
LABEL hartwell
  KERNEL vmlinuz
  INITRD initrd
  APPEND console=tty0 quiet rdinit=/init video=1280x800
CFG
mcopy -i boot.img efi.cfg ::EFI/BOOT/syslinux.cfg

# --- state partition: HWO_CFG, holds licence + content across reboots ---------
dd if=/dev/zero of=state.img bs=1M count="$STATEMB" status=none
mkfs.vfat -F 16 -n HWO_CFG state.img >/dev/null
mcopy -i state.img /src/content.txt ::content.txt
if [ "$MEDIA" = 1 ]; then
    mmd -i state.img ::media
    mcopy -i state.img /media/*.png ::media/
    [ -f /media/fps ] && mcopy -i state.img /media/fps ::media/fps
fi
if [ -n "$HWO_KEY" ] && [ -n "$HWO_DEVICE" ]; then
    {
        printf 'key=%s\ndevice=%s\n' "$HWO_KEY" "$HWO_DEVICE"
        [ -n "$HWO_LIC_SIG" ] && printf 'licence=%s\n' "$HWO_LIC_SIG"
    } > license
    mcopy -i state.img license ::license
fi

# --- assemble: MBR, partition table, both filesystems ------------------------
total=$((1 + BOOTMB + STATEMB + 1))
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$total" status=none
dd if=/usr/share/syslinux/mbr.bin of="$OUT" bs=440 count=1 conv=notrunc status=none
sfdisk "$OUT" >/dev/null <<EOF
label: dos
start=2048, size=$((BOOTMB * 2048)), type=c, bootable
start=$(((1 + BOOTMB) * 2048)), size=$((STATEMB * 2048)), type=c
EOF
dd if=boot.img  of="$OUT" bs=1M seek=1            conv=notrunc status=none
dd if=state.img of="$OUT" bs=1M seek=$((1 + BOOTMB)) conv=notrunc status=none
sync
echo "written: $OUT ($(($total)) MB)"
sfdisk -l "$OUT" 2>/dev/null | sed -n '2,8p'
INNER

# checksum next to the ISO sums
SUM=$(sha256sum "dist/$(basename "$OUT")" | cut -d' ' -f1)
if [ -f dist/SHA256SUMS ]; then
    grep -v 'hartwell-os-1.0-usb.img' dist/SHA256SUMS > dist/SHA256SUMS.new || true
    echo "$SUM  hartwell-os-1.0-usb.img" >> dist/SHA256SUMS.new
    mv dist/SHA256SUMS.new dist/SHA256SUMS
fi

# the container runs as root; hand the image back to the user
docker run --rm -v "$PWD/dist:/d" alpine:latest chown "$(id -u):$(id -g)" "/d/$(basename "$OUT")" 2>/dev/null || true

echo
echo "==> disk image ready: $OUT"
echo "    write it to a stick:  sudo dd if=$OUT of=/dev/sdX bs=4M && sync"
