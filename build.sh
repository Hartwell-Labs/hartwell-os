#!/bin/sh
# Build Hartwell OS 1.0: initramfs + bootable ISO (BIOS + UEFI) + proof ISO.
# Requires: gcc (static), docker (for syslinux/xorriso), the Alpine base in buildroot/.
set -e
cd "$(dirname "$0")"

ROOT=build/rootfs
DIST=dist
VMLINUZ=${VMLINUZ:-buildroot/vmlinuz-lts}

# the Alpine base is not committed: a clean checkout fetches it, pinned
if [ ! -f buildroot/alpine-minirootfs.tar.gz ] || [ ! -f buildroot/vmlinuz-lts ]; then
  sh tools/fetch-base.sh
fi

echo "==> staging rootfs"
rm -rf build "$DIST"
mkdir -p "$ROOT" "$DIST"
tar xzf buildroot/alpine-minirootfs.tar.gz -C "$ROOT"
mkdir -p "$ROOT"/{proc,sys,dev,run,tmp,etc/hartwell}
rm -rf "$ROOT"/var/cache "$ROOT"/usr/share/man "$ROOT"/usr/share/doc "$ROOT"/opt "$ROOT"/media "$ROOT"/mnt

echo "==> building appliance runtime"
# vendor EdDSA licence key: the public half is embedded in the binary, the seed
# stays in keys/ for the seller. Generated once and then reused.
if [ ! -f keys/hwo-lic.pub ]; then
  echo "==> generating vendor licence key"
  gcc -O2 -o build/hwo-lic tools/hwo-lic.c src/monocypher.c
  ./build/hwo-lic gen keys
fi
gcc -O2 -o build/hwo-lic tools/hwo-lic.c src/monocypher.c
LIC_PUB=$(cat keys/hwo-lic.pub)
gcc -static -O2 -Wall -Wextra -DHWO_LIC_PUB_HEX='"'"$LIC_PUB"'"' \
    -o "$ROOT/hwo" src/hwo.c src/fbui.c src/png.c src/errors.c src/monocypher.c
# display (DRM), input (evdev keyboard + USB pointer) and storage (VFAT state
# partition, USB/SATA/virtio disks)
python3 tools/stage_modules.py "$ROOT" \
    bochs virtio_gpu evdev psmouse usbhid \
    virtio_net e1000 e1000e r8169 \
    fat vfat nls_cp437 nls_iso8859_1 nls_ascii nls_utf8 \
    virtio_blk virtio_pci sd_mod sr_mod usb_storage uas xhci_hcd xhci_pci ehci_hcd ehci_pci uhci_hcd \
    ahci ata_piix ata_generic pata_acpi

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

cat > "$ROOT/etc/hartwell/version" <<EOF
Hartwell OS 1.0
build $(date -u +%Y-%m-%dT%H:%M:%SZ)
kernel $(basename "$VMLINUZ")
EOF

echo "==> packing initramfs"
( cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 ) > build/initrd
cp "$VMLINUZ" build/vmlinuz

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
( cd "$DIST" && sha256sum hartwell-os-1.0.iso hartwell-os-1.0-proof.iso > SHA256SUMS )

echo
echo "==> done"
ls -lh "$DIST"
cat "$DIST/SHA256SUMS"
