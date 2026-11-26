#!/bin/sh
# Build Hartwell OS 1.0: initramfs + bootable ISO (BIOS + UEFI) + proof ISO.
# Requires: gcc (static), docker (for syslinux/xorriso), the Alpine base in buildroot/.
set -e
cd "$(dirname "$0")"

ROOT=build/rootfs
DIST=dist

# x86_64 (default) or arm64: same rootfs and UI, a different kernel, module
# tree and boot chain. The ARM build has no BIOS and no ISO: the deliverable is
# a GPT disk image with one EFI system partition holding systemd-boot.
ARCH=${ARCH:-amd64}
VERSION=1.0.0
case "$ARCH" in
  amd64) BASE=buildroot ;;
  arm64|aarch64) ARCH=arm64; BASE=buildroot-arm64 ;;
  *) echo "unknown ARCH '$ARCH' (use amd64 or arm64)" >&2; exit 1 ;;
esac
VMLINUZ=${VMLINUZ:-$BASE/vmlinuz-lts}
export HWO_BUILDROOT=$BASE

# the Alpine base is not committed: a clean checkout fetches it, pinned
if [ ! -f "$BASE/alpine-minirootfs.tar.gz" ] || [ ! -f "$BASE/vmlinuz-lts" ]; then
  if [ "$ARCH" = arm64 ]; then sh tools/fetch-base.sh --arch arm64
  else                          sh tools/fetch-base.sh; fi
fi

echo "==> staging rootfs"
rm -rf build
mkdir -p "$ROOT" "$DIST"
tar xzf "$BASE/alpine-minirootfs.tar.gz" -C "$ROOT"
mkdir -p "$ROOT"/{proc,sys,dev,run,tmp,etc/hartwell}
rm -rf "$ROOT"/var/cache "$ROOT"/usr/share/man "$ROOT"/usr/share/doc "$ROOT"/opt "$ROOT"/media "$ROOT"/mnt

echo "==> building appliance runtime"
# vendor EdDSA licence key: the public half is embedded in the binary, the seed
# stays in keys/ for the seller. Generated once and then reused.
mkdir -p keys            # ignored by git: a clean checkout starts without it
if [ ! -f keys/hwo-lic.pub ]; then
  echo "==> generating vendor licence key"
  gcc -O2 -o build/hwo-lic tools/hwo-lic.c src/monocypher.c
  ./build/hwo-lic gen keys
fi
gcc -O2 -o build/hwo-lic tools/hwo-lic.c src/monocypher.c
LIC_PUB=$(cat keys/hwo-lic.pub)
if [ "$ARCH" = arm64 ]; then
  # the runtime must be an aarch64 binary; build it in an emulated arm64 image
  # so the build host needs no cross toolchain
  docker run --rm --platform linux/arm64 \
      -v "$PWD/src:/src:ro" -v "$PWD/build:/out" -e LIC_PUB="$LIC_PUB" alpine:latest sh -c '
    set -e
    apk add --no-cache gcc musl-dev linux-headers >/dev/null 2>&1
    gcc -static -O2 -Wall -Wextra -DHWO_LIC_PUB_HEX="\"$LIC_PUB\"" -o /out/hwo \
        /src/hwo.c /src/fbui.c /src/png.c /src/errors.c /src/update.c /src/monocypher.c
  '
  cp build/hwo "$ROOT/hwo"
else
gcc -static -O2 -Wall -Wextra -DHWO_LIC_PUB_HEX='"'"$LIC_PUB"'"' \
    -o "$ROOT/hwo" src/hwo.c src/fbui.c src/png.c src/errors.c src/update.c src/monocypher.c
fi
# display (DRM), input (evdev keyboard + USB pointer) and storage (VFAT state
# partition, USB/SATA/virtio disks)
if [ "$ARCH" = arm64 ]; then
  # QEMU virt and boards with a simple framebuffer: no bochs, no PS/2, no PATA
  HWO_ALLOW_MISSING=1 python3 tools/stage_modules.py "$ROOT" \
      virtio_gpu simpledrm evdev usbhid xhci_hcd xhci_pci \
      virtio_net virtio_blk virtio_pci virtio_mmio \
      fat vfat nls_cp437 nls_iso8859_1 nls_ascii nls_utf8 \
      sd_mod sr_mod usb_storage uas
else
  python3 tools/stage_modules.py "$ROOT" \
      bochs virtio_gpu evdev psmouse usbhid \
      virtio_net e1000 e1000e r8169 \
      fat vfat nls_cp437 nls_iso8859_1 nls_ascii nls_utf8 \
      virtio_blk virtio_pci sd_mod sr_mod usb_storage uas xhci_hcd xhci_pci ehci_hcd ehci_pci uhci_hcd \
      ahci ata_piix ata_generic pata_acpi
fi

# a syntax error in /init reaches the unit as a kernel panic, so it is caught here
sh -n src/init.sh || { echo "src/init.sh does not parse"; exit 1; }

echo "==> installing init, agent, content"
cp src/init.sh  "$ROOT/init";      chmod 0755 "$ROOT/init"
cp src/agent.sh "$ROOT/agent.sh";  chmod 0755 "$ROOT/agent.sh"
cp src/rescue.sh "$ROOT/rescue.sh"; chmod 0755 "$ROOT/rescue.sh"
cp src/content.txt "$ROOT/etc/hartwell/content.txt"

# media: a PNG sequence embedded in the initramfs so the Media screen works on
# the ISO too (the disk image ships it under /state/media instead).
if [ -n "$MEDIA_DIR" ] && [ -d "$MEDIA_DIR" ]; then
  echo "==> embedding media from $MEDIA_DIR"
  mkdir -p "$ROOT/etc/hartwell/media"
  cp "$MEDIA_DIR"/*.png "$ROOT/etc/hartwell/media/" 2>/dev/null || true
  [ -f "$MEDIA_DIR/fps" ] && cp "$MEDIA_DIR/fps" "$ROOT/etc/hartwell/media/"
fi

if [ -n "$HWO_KEY" ] && [ -n "$HWO_DEVICE" ]; then
  echo "==> embedding licence for $HWO_DEVICE"
  printf 'key=%s\ndevice=%s\n' "$HWO_KEY" "$HWO_DEVICE" > "$ROOT/etc/hartwell/license"
else
  echo "==> no licence embedded (device will boot unlicensed)"
fi

printf '%s\n' "$VERSION" > "$ROOT/etc/hartwell/version.txt"

cat > "$ROOT/etc/hartwell/version" <<EOF
Hartwell OS 1.0
build $(date -u +%Y-%m-%dT%H:%M:%SZ)
kernel $(basename "$VMLINUZ")
EOF

echo "==> packing initramfs"
( cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 ) > build/initrd
cp "$VMLINUZ" build/vmlinuz

if [ "$ARCH" = arm64 ]; then
  echo "==> packing bootable ARM64 disk image"
  printf '%s\n' "$VERSION" > build/ver-a
  docker run --rm -v "$PWD/build:/b" -v "$PWD/$BASE:/base:ro" -v "$PWD/dist:/out" alpine:latest sh -c '
    set -e
    apk add --no-cache mtools dosfstools parted >/dev/null 2>&1
    IMG=/tmp/hwo-arm64.img
    dd if=/dev/zero of=$IMG bs=1M count=96 status=none
    parted -s $IMG mklabel gpt mkpart ESP fat16 1MiB 80MiB set 1 esp on
    OFF=$((1024*1024))
    mkfs.vfat -F 16 -n HWOBOOT --offset 2048 $IMG $((79*1024*2)) >/dev/null
    D="$IMG@@$OFF"
    mmd -i "$D" ::EFI ::EFI/BOOT ::loader ::loader/entries ::slot-a ::slot-b
    mcopy -i "$D" /base/systemd-bootaa64.efi ::EFI/BOOT/BOOTAA64.EFI
    # a slot is a whole boot payload: its own kernel and its own initramfs, so
    # an update can change the kernel without ever touching the running slot
    mcopy -i "$D" /b/vmlinuz ::slot-a/vmlinuz
    mcopy -i "$D" /b/vmlinuz ::slot-b/vmlinuz
    mcopy -i "$D" /b/initrd ::slot-a/initrd
    mcopy -i "$D" /b/initrd ::slot-b/initrd
    # one live entry plus rescue. This systemd-boot ignores `default` and boots
    # the entry that sorts last, so the live entry is zz-hartwell.conf: whichever
    # way the menu resolves, an unattended unit gets the slot stated in
    # /boot/state (hwo update rewrites this file, it does not add entries).
    printf "default zz-hartwell\ntimeout 5\n" > /tmp/loader.conf
    printf "title Hartwell OS\nlinux /slot-a/vmlinuz\ninitrd /slot-a/initrd\noptions console=ttyAMA0 console=tty0 quiet rdinit=/init video=1280x800 hwo.slot=a\n" > /tmp/hwo.conf
    # rescue always boots the original slot-a image: the image that has been
    # running since the unit was installed, never a half-tested update
    printf "title Rescue (shell, no UI)\nlinux /slot-a/vmlinuz\ninitrd /slot-a/initrd\noptions console=ttyAMA0 console=tty0 rdinit=/init video=1280x800 hwo.rescue=1\n" > /tmp/rescue.conf
    printf "active=a\nprevious=b\n" > /tmp/state
    mcopy -i "$D" /tmp/loader.conf ::loader/loader.conf
    mcopy -i "$D" /tmp/hwo.conf ::loader/entries/zz-hartwell.conf
    mcopy -i "$D" /tmp/rescue.conf ::loader/entries/aa-rescue.conf
    mcopy -i "$D" /tmp/state ::state
    mcopy -i "$D" /b/ver-a ::ver-a
    cp $IMG /out/hartwell-os-1.0-arm64.img
  '
  # arch-suffixed: the x86 build writes dist/initrd and dist/vmlinuz, and a
  # build of the other arch must not silently replace them
  cp build/initrd "$DIST/initrd-arm64"
  cp build/vmlinuz "$DIST/vmlinuz-arm64"
  ( cd "$DIST" && ls | grep -v SHA256SUMS | xargs sha256sum > SHA256SUMS )
  echo
  echo "==> done (arm64)"
  ls -lh "$DIST"
  cat "$DIST/SHA256SUMS"
  exit 0
fi

echo "==> Secure Boot: keys and signed UEFI loader for the disk image"
./tools/secureboot.sh init >/dev/null
mkdir -p build/unsigned build/signed
if [ -f build/unsigned/syslinux.efi ]; then :; else
    docker run --rm -v "$PWD/build:/b" alpine:latest sh -c '
      set -e
      apk add --no-cache syslinux >/dev/null 2>&1
      cp /usr/share/syslinux/efi64/syslinux.efi /b/unsigned/syslinux.efi
      cp /usr/share/syslinux/efi64/ldlinux.e64 /b/unsigned/ldlinux.e64
    '
fi
./tools/secureboot.sh sign build/unsigned/syslinux.efi build/signed/BOOTX64.EFI
cp keys/db.cer build/signed/db.cer
echo "==> signed: build/signed/BOOTX64.EFI"

echo "==> building ISO"
mkdir -p build/iso/isolinux build/iso/boot
cp build/vmlinuz  build/iso/boot/vmlinuz
cp build/initrd   build/iso/boot/initrd
cat > build/iso/isolinux/isolinux.cfg <<'CFG'
DEFAULT hartwell
# Ten seconds of quiet boot. Any key during that window still opens the prompt,
# which is how a technician reaches "hartwell rescue"; without a key the unit
# always boots on its own, so an unattended machine can never sit at "boot:".
TIMEOUT 100
PROMPT 0

LABEL hartwell
  KERNEL /boot/vmlinuz
  INITRD /boot/initrd
  APPEND console=tty0 quiet rdinit=/init video=1280x800

# Physical access only: drops to /bin/sh, prints the error log, starts no UI
# and no agent. See docs/errors.md, "Getting a shell".
LABEL rescue
  KERNEL /boot/vmlinuz
  INITRD /boot/initrd
  APPEND console=tty0 rdinit=/init video=1280x800 hwo.rescue=1
CFG

cat > build/iso/isolinux/proof.cfg <<'CFG'
DEFAULT hartwell-proof
TIMEOUT 10
PROMPT 0

LABEL hartwell-proof
  KERNEL /boot/vmlinuz
  INITRD /boot/initrd
  APPEND console=ttyS0 quiet rdinit=/init video=1280x800 hwo.proof=1
CFG

docker run --rm -v "$PWD/build/iso:/iso" -v "$PWD/dist:/out" -v "$PWD/keys:/keys:ro" \
    -v "$PWD/build/signed:/signed:ro" alpine:latest sh -c '
  set -e
  apk add --no-cache syslinux xorriso mtools dosfstools grub grub-efi sbsigntool >/dev/null 2>&1
  cp /usr/share/syslinux/isolinux.bin /usr/share/syslinux/ldlinux.c32 /iso/isolinux/

  # UEFI: El Torito FAT image holding a GRUB EFI binary. GRUB reads the ISO9660
  # filesystem, so the kernel and initrd stay only once, at /boot in the ISO.
  cat > /tmp/grub.cfg <<CFG
set timeout=10
search --no-floppy --set=root --file /boot/initrd
menuentry "Hartwell OS" {
  linux /boot/vmlinuz console=tty0 quiet rdinit=/init video=1280x800
  initrd /boot/initrd
}
menuentry "Rescue (shell, no UI)" {
  linux /boot/vmlinuz console=tty0 rdinit=/init video=1280x800 hwo.rescue=1
  initrd /boot/initrd
}
CFG
  grub-mkstandalone -O x86_64-efi -o /tmp/grub.efi \
    --modules="part_gpt part_msdos fat iso9660 normal linux echo search search_fs_file" \
    --locales="" --themes="" --fonts="" "boot/grub/grub.cfg=/tmp/grub.cfg"
  # Secure Boot: the loader is signed with the Hartwell db key; the certificate
  # ships next to it so a customer can enroll it in firmware.
  sbsign --key /keys/db.key --cert /keys/db.crt --output /tmp/BOOTX64.EFI /tmp/grub.efi
  cp /keys/db.cer /iso/hartwell-db.cer
  mkdir -p /iso/EFI/BOOT
  dd if=/dev/zero of=/iso/EFI/efiboot.img bs=1M count=6 status=none
  mkfs.vfat -F 16 -s 1 -n HWOEFI /iso/EFI/efiboot.img >/dev/null
  mmd -i /iso/EFI/efiboot.img ::EFI ::EFI/BOOT
  mcopy -i /iso/EFI/efiboot.img /tmp/BOOTX64.EFI ::EFI/BOOT/BOOTX64.EFI
  mcopy -i /iso/EFI/efiboot.img /keys/db.cer ::EFI/hartwell-db.cer

  xorriso -as mkisofs -quiet -o /out/hartwell-os-1.0.iso \
    -b isolinux/isolinux.bin -c isolinux/boot.cat \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -eltorito-alt-boot -e EFI/efiboot.img -no-emul-boot \
    -isohybrid-gpt-basdat \
    -V HARTWELL_OS /iso
  rm -rf /iso/EFI
  cp /iso/isolinux/proof.cfg /iso/isolinux/isolinux.cfg
  xorriso -as mkisofs -quiet -o /out/hartwell-os-1.0-proof.iso \
    -b isolinux/isolinux.bin -c isolinux/boot.cat \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -V HARTWELL_PROOF /iso
' >/dev/null

cp build/initrd "$DIST/initrd"
cp build/vmlinuz "$DIST/vmlinuz"
( cd "$DIST" && ls | grep -v SHA256SUMS | xargs sha256sum > SHA256SUMS )

echo
echo "==> done"
ls -lh "$DIST"
cat "$DIST/SHA256SUMS"
