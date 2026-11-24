# Hartwell OS — Complete Documentation (A to Z)

## Table of Contents
1. [Overview](#overview)
2. [Building](#building)
3. [Running](#running)
4. [Licensing](#licensing)
5. [State Partition](#state-partition)
6. [Media / PNG Playback](#media--png-playback)
7. [Secure Boot](#secure-boot)
8. [Input / Mouse](#input--mouse)
9. [Content](#content)
10. [Verification](#verification)
11. [Known Limits](#known-limits)
12. [File Reference](#file-reference)

---

## 1. Overview

Hartwell OS is a single-purpose appliance operating system for x86_64 machines that must
perform one fixed task and never drift. It boots straight into a 1280×800 framebuffer
interface, enforces a per-device offline licence, and has no shell, no package manager,
no remote login and no open port.

**Target use cases:** signage players, kiosks, station controllers, production boards,
test benches, event rental equipment.

**Key properties:**
- Boots in ~1 s (KVM) or ~12 s (pure TCG emulation)
- 1280×800 framebuffer, 32 bpp, drawn directly by the runtime (no X, no Wayland, no browser)
- Offline per-device licence (`HWO-YYYY-XXXX`, bound to DMI UUID, no activation server)
- Licence and screen content survive reboot on the `HWO_CFG` VFAT partition
- Two boot modes: BIOS (El Torito ISO) and UEFI (signed EFI loader)
- Optional disk image with persistent state (`tools/mkdisk.sh`)
- Full source included; everything built from scratch, no binary blobs

---

## 2. Building

### 2.1 Requirements
- `gcc` (static build, `-static` flag)
- `docker` (ISO step installs syslinux/xorriso in a throwaway container)
- `make` (not required but common)
- `python3` (for some tool scripts)

### 2.2 Build commands

```sh
# Unlicensed image (boots to activation screen)
./build.sh

# Licensed image (bakes key in from environment)
HWO_KEY=HWO-2026-V8NS HWO_DEVICE=HWO-3DD5-B123 ./build.sh
```

### 2.3 Output in `dist/`

| File | Description |
|---|---|
| `hartwell-os-1.0.iso` | Bootable ISO, BIOS + UEFI, 25 MB |
| `hartwell-os-1.0-proof.iso` | Same rootfs, serial console, screen dumps to COM1 |
| `initrd` | CPIO-gzip initramfs (5.4 MB) |
| `vmlinuz` | Linux 6.12.118 LTS kernel |
| `SHA256SUMS` | SHA256 checksums of the above |

### 2.4 Build process steps (internal)

1. `staging rootfs` — extracts Alpine minirootfs, creates directory structure
2. `building appliance runtime` — compiles `src/hwo.c`, `src/fbui.c`, `src/png.c` into
   one static binary `hwo` (1.2 MB, no libc on target)
3. `installing init, agent, content` — copies `init.sh`, `agent.sh`, `content.txt`
4. `embedding licence` — if `HWO_KEY`/`HWO_DEVICE` are set, writes `license` file
5. `packing initramfs` — `find | cpio --null -o -H newc | gzip -9`
6. `Secure Boot: keys and signed UEFI loader` — `tools/secureboot.sh init` →
   `tools/secureboot.sh sign` → `build/signed/BOOTX64.EFI`
7. `packing initramfs + kernel` — `buildroot/vmlinuz-lts` + initrd into `build/`

### 2.5 Module staging

`build.sh` passes a module list to `tools/stage_modules.py`, which places the following
into `build/rootfs/lib/modules/`:

```
bochs virtio_gpu evdev psmouse usbhid
fat vfat nls_cp437 nls_iso8859_1 nls_ascii nls_utf8
virtio_blk virtio_pci sd_mod sr_mod usb_storage uas xhci_hcd xhci_pci
ehci_hcd ehci_pci uhci_hcd ahci
```

- `uhci_hcd` was added in this development cycle (was missing, caused USB tablet not
  to appear in the guest)
- `usbhid` makes USB mice/tablets work via evdev
- `psmouse` enables PS/2 keyboard (i8042); USB-only if PS/2 not present

---

## 3. Running

### 3.1 Boot from ISO (BIOS or UEFI)

```sh
qemu-system-x86_64 -cdrom dist/hartwell-os-1.0.iso -m 256 -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f
```

### 3.2 Boot from disk image

```sh
# Build the disk image
./build.sh
./tools/mkdisk.sh                                   # unlicensed
HWO_KEY=HWO-2026-V8NS HWO_DEVICE=HWO-3DD5-B123 ./tools/mkdisk.sh  # licensed

# Write to USB or disk
sudo dd if=dist/hartwell-os-1.0-usb.img of=/dev/sdX bs=4M && sync
```

### 3.3 QEMU command-line options (debug)

```sh
# With serial console
qemu-system-x86_64 -cdrom dist/hartwell-os-1.0.iso -m 512 -serial stdio -uuid <uuid>

# With VNC (for screenshot/control)
qemu-system-x86_64 -cdrom dist/hartwell-os-1.0.iso -m 512 -display vnc=0:0 -uuid <uuid>

# With debug kernel params
qemu-system-x86_64 -cdrom dist/hartwell-os-1.0.iso -m 512 \
  -append "console=ttyS0 quiet rdinit=/init video=1280x800 hwo.debug=1" -uuid <uuid>
```

### 3.4 Screenshots from inside QEMU

```sh
# On the QEMU monitor (press Ctrl+A then S to save, or use -mon chardev)
screendump /path/to/output.ppm
```

---

## 4. Licensing

### 4.1 Device ID

The device id is derived from the DMI product UUID:

```
HWO-<fnv1a(uuid) high 16 bits>-<low 16 bits>
```

Example: UUID `9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f` → `HWO-3DD5-B123`

### 4.2 Licence crypto — Ed25519 signatures

The authoritative licence is an **EdDSA (curve25519 + BLAKE2b)** signature over the
device id. The seller holds the seed in `keys/hwo-lic.key` (chmod 600); the appliance
carries only the public key `keys/hwo-lic.pub`, embedded into the binary by `build.sh`
(`-DHWO_LIC_PUB_HEX=...`). Monocypher (`src/monocypher.c`) does the maths.

- A signature can be produced **only** with the seller's seed — it cannot be forged
  from the shipped binary.
- It is over *this* device id, so it cannot be copied to another unit.
- Verification is fully offline; nothing phones home.

One-time keypair creation:

```sh
gcc -O2 -o build/hwo-lic tools/hwo-lic.c src/monocypher.c
./build/hwo-lic gen keys        # writes keys/hwo-lic.key (0600) + hwo-lic.pub
```

The typed `HWO-YYYY-XXXX` serial remains as a convenience/demo path (4 base32 chars of
`fnv1a("<device>|secret")`); it is **not** cryptographic and is superseded whenever a
signed `licence=` line is present.

### 4.3 Issuing a licence

```sh
# signed licence file for a device (preferred)
./tools/keygen.py --uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f
# device=HWO-3DD5-B123
# licence=<128 hex chars>

# or directly
./build/hwo-lic sign keys HWO-3DD5-B123
# device=HWO-3DD5-B123
# licence=<128 hex chars>

# verify
./build/hwo-lic verify keys HWO-3DD5-B123 <hex>
# valid
```

The `/state/license` file then looks like:

```
key=HWO-2026-V8NS
device=HWO-3DD5-B123
licence=920ed14e…900f
```

`tools/mkdisk.sh` signs automatically when `HWO_DEVICE` is set and `keys/hwo-lic.key`
exists, so the shipped image carries a real signature.

### 4.4 Binding behaviour

- Signature is over the machine's device id — copying the file to another unit is
  **rejected** (`licence_sig_ok` fails).
- A licence typed/stored on the unit lives on `HWO_CFG`, so it survives reboot; the
  initramfs copy is only a cache.
- The Licence screen reports which is in effect (signed, serial, or initramfs cache).

### 4.5 Without a licence

The machine boots to the activation screen and stays there — no shell, no network tools,
no way past it. The only exit is a valid key.

---

## 5. State Partition

### 5.1 Partition layout

The disk image built by `tools/mkdisk.sh` has two partitions:

| Partition | Label | Filesystem | Purpose |
|---|---|---|---|
| 1 | `HWBOOT` | FAT16 | BIOS syslinux + kernel + initramfs; also `EFI/BOOT/BOOTX64.EFI` (UEFI) |
| 2 | `HWO_CFG` | FAT16 | `/state`: `license` and `content.txt` survive reboot |

### 5.2 Mounting at boot

`init` (PID 1) scans all block devices and runs `blkid` on each. It **parses the full
output** and matches `LABEL="HWO_CFG"` (busybox `blkid -t` does NOT filter correctly,
so the full line is parsed). When found, it mounts:

```sh
mount -t vfat -o rw,iocharset=utf8,umask=0022 "$d" /state
```

### 5.3 What survives on the partition

- `/state/license` — contains `key=HWO-YYYY-XXXX\ndevice=HWO-3DD5-B123`
- `/state/content.txt` — overview screen content (Label | value pairs)

Every write goes back to the partition, so a licence typed on the unit and content
pushed by the fleet agent both survive power loss.

### 5.4 Optional — can run without it

The unit still boots and runs entirely from RAM if no `HWO_CFG` partition is present.
The licence and content are then lost on power off (initramfs cache only).

---

## 6. Media / PNG Playback

### 6.1 How to access

Press `4` (or click the **Media** tab) to show the Media screen. PNG frames
`frameNNNN.png` are read from `/state/media` (symlinked to `/etc/hartwell/media`).

### 6.2 Decoder — own `inflate`, no libpng

The PNG decoder is implemented from scratch in `src/png.c` — **no zlib/libpng
dependency on the target device**. Supported features:

- PNG types: 0 (grayscale), 2 (RGB), 3 (palette), 4 (grayscale+alpha), 6 (RGBA)
- All filter types: 0–4
- `tRNS` chunk
- Interlacing not supported (PNG generated without it by `mkmedia.sh`)

### 6.3 FPS and sequence

- FPS range: 1–60, default 10
- If no frames remain in the sequence, `poster.png` still image is shown
- Damaged frames are skipped; the index advances to the next
- UI displays: `frame n of N · fps M`

### 6.4 Converting video → PNG sequence

Use `tools/mkmedia.sh`:

```sh
./tools/mkmedia.sh --clip /tmp/clip.mp4 --fps 10 --out /state/media
```

Options:
- `--start_number N` — first frame number (default 0)
- `--fps N` — frames per second (default 10)
- `--width N` — resize width (maintains aspect ratio; original preserved if omitted)
- `--out DIR` — output directory (default `/state/media`)

The script uses `ffmpeg` to produce `frame0000.png`, `frame0001.png`, … using the
own `inflate` decoder logic (the PNG files are written raw; the target device decodes
them with `src/png.c`).

### 6.5 Media screen layout

- Full 1280×800 display
- Centered/or-contained PNG frame with drop shadow
- Bottom navigation tabs: `1 Overview`, `2 System`, `3 Licence`, `4 Media`
- FPS counter in the footer

---

## 7. Secure Boot

### 7.1 Key generation (one-time setup)

```sh
./tools/secureboot.sh init
```

This creates a full PK/KEK/db key set in `keys/` (all files chmod 600):

| File | Purpose |
|---|---|
| `PK.key` / `PK.crt` / `PK.esl` | Primary Key — signs itself and KEK |
| `KEK.key` / `KEK.crt` / `KEK.esl` | Key Enrollment Key — signs db |
| `db.key` / `db.crt` / `db.esl` | Database — authorises signed OS loaders |
| `db.cer` | Client certificate — copy to `EFI/hartwell-db.cer` on the disk |
| `db.auth` | Authorisation file — copy to `EFI/hartwell-db.auth` on the disk |

### 7.2 Signing the UEFI loader

```sh
./tools/secureboot.sh sign build/unsigned/syslinux.efi build/signed/BOOTX64.EFI
```

The `syslinux.efi` from `build/unsigned/` is signed with the generated keys and output
as `BOOTX64.EFI` in `build/signed/`.

### 7.3 Building OVMF vars file

```sh
./tools/secureboot.sh vars build/OVMF_VARS.sb.fd
```

Uses `virt-fw-vars` (installed inside the `hartwell-sbtools:1` Debian bookworm image).
Flags used: `-i`, `-o`, `--sb`, `--set-pk GUID FILE`, `--add-kek`, `--add-db`.
`--secure-boot` and `--output` are **not** used (they are not valid `virt-fw-vars`
flags; they were mistakenly referenced in earlier notes).

### 7.4 Testing Secure Boot in QEMU

```sh
# Prerequisite: vars file built
./tools/secureboot.sh vars build/OVMF_VARS.sb.fd

# Run with Secure Boot
qemu-system-x86_64 -drive file=dist/hartwell-os-1.0-usb.img -m 512 \
  -bios build/OVMF_CODE_4M.secboot.fd \
  -drive file=build/OVMF_VARS.sb.fd,format=raw \
  -serial file:/dev/null -no-reboot -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f

# Verify
./tools/secureboot.sh verify
```

### 7.5 Key file locations and copy to disk

After signing, copy the client-facing files to the disk image's `EFI/` partition:

```sh
# After ./tools/mkdisk.sh has been run:
cp keys/db.cer dist/hartwell-db.cer
cp keys/db.auth dist/hartwell-db.auth
# The signed loader is already in dist/hartwell-os-1.0-usb.img (built into EFI/)
```

### 7.6 Without Secure Boot

Default state: the EFI loader is **unsigned**, so Secure Boot must be off (or the key
must be enrolled). The BIOS path always works regardless. The key set is optional —
the system runs perfectly well without it.

---

## 8. Input / Mouse

### 8.1 Keyboard

- PS/2 or USB keyboard via evdev
- Works with no X server and no libinput
- All navigation in the UI: tab switching, number keys 1/2/3/4, F1–F4, ENTER

### 8.2 Mouse / Touch

- **USB mouse/tablet via evdev** ( `usbhid` + `uhci_hcd` / `xhci_hcd` modules )
- PS/2 is **impossible** — the `i8042` module is not in `buildroot/modules`, and
  most modern motherboards lack the i8042 controller in the QEMU virt machine
- The UI supports **multiple input devices simultaneously** (up to 4: mouse, tablet,
  touchscreen, etc.)
- Cursor is drawn in a shadow buffer (13×17 arrow with transparency)
- Hit-testing for: tab buttons, Licence screen controls, Media tab, Activate button
- Coordinates (approximate, 1280×800 resolution):
  - Tab 2 (Unlicensed): x 668..781, y 750..794
  - Tab 3 (Licensed):   x 790..888, y 750..794
  - Activate button:    x 784..952, y 314..360, centre ~(868, 337)
  - Cursor start position: (640, 400)

### 8.3 Input in QEMU

```sh
# USB tablet (recommended)
qemu-system-x86_64 -device usb-tablet -uuid <uuid>

# USB mouse
qemu-system-x86_64 -device usb-mouse -uuid <uuid>

# Both (UI reads all attached devices)
qemu-system-x86_64 -device qemu-xhci -device usb-tablet -device usb-mouse -uuid <uuid>
```

### 8.4 Debugging input

If events are not reaching the UI:

1. Check `ls /dev/input/event*` — should show event0 (keyboard), event1 (mouse/tablet)
2. In the guest, `printf` from `mouse_read()` shows events as they arrive
3. Use `hwo.debug=1` kernel parameter to print every polled event to the serial console
4. Verify modules are loaded: `ls /lib/modules/$(ls /lib/modules)/kernel/drivers/usb/`

---

## 9. Content

### 9.1 `/etc/hartwell/content.txt`

The Overview screen is driven by this file, one `Label | value` pair per line, plain ASCII:

```
Station 04 - Line A | live | since boot | source local
Output | 1 240 units/h
Shift | A - 06:00-14:00
Operator | station 2
Maintenance | next window 18:00
Status | running - no faults
```

- First line = card title + small tags (`live`, `since boot`, `source local`)
- Remaining lines = label/value rows shown on the card
- The agent can replace this file every 30 s via `HWO_URL` HTTP poll

### 9.2 Agent content pull

At boot `init.sh` brings up networking: it loads `virtio_net`/`e1000`/`e1000e`/`r8169`,
runs `udhcpc` on every NIC, installs the default route and writes `/etc/resolv.conf`.
The endpoint is read from `/etc/hartwell/agent.conf` (`url=...`) or the kernel parameter
`hwo.url=...`, and exported as `HWO_URL`.

With an endpoint set, the fleet agent:

1. Fetches `$HWO_URL/content.txt` every 30 s
2. Replaces the local copy atomically (write to temp, `mv`)
3. POSTs `device=...&state=...` to `$HWO_URL/heartbeat`
4. Status lands in `/run/fleet.status` (`online`, `offline`, `local`); the URL is echoed
   to `/run/fleet.url` for the System screen

If no endpoint is set, the agent runs in local mode (no outbound connections, no
heartbeat, content stays as last written by the technician). Without a DHCP lease the
unit still boots; the agent simply reports `local`/`offline`.

---

## 10. Verification

### 10.1 Screenshots are real

All screenshots on the product page (`index.html`, `SALES.md`, `README.md`) are
`screendump` output from a running QEMU guest, taken from the real framebuffer.
They are **not mockups** and **not design renders**.

### 10.2 Verification commands

```sh
# From the host, inside the dist/ directory
docker run --rm -v "$PWD/dist:/d" alpine:latest sh -c \
  'apk add --no-cache qemu-system-x86_64; qemu-system-x86_64 -cdrom /d/hartwell-os-1.0.iso \
   -m 512 -vga std -display none -monitor stdio -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f'\n# then on the monitor: screendump /d/x.ppm   /   sendkey 2   /   quit
```

### 10.3 Text-console (proof image) path

```sh
./build.sh
docker run --rm -v "$PWD/dist:/d" alpine:latest sh -c \
  'apk add --no-cache qemu-system-x86_64; qemu-system-x86_64 -cdrom /d/hartwell-os-1.0-proof.iso \
   -m 256 -display none -serial stdio -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f'\n# ... --CONSOLE-- <hex dump> --END--\npython3 render_console.py dump.txt out.png
```

---

## 11. Known Limits, stated plainly

| Limit | Status | Note |
|---|---|---|
| **No browser or video layer** | Stated | Screen is a rendered interface (cards, text, licence state), not HTML/WebGL and not video playback |
| **Mouse supported** | Already delivered | USB only (evdev via `usbhid`/`uhci_hcd`/`xhci_hcd`); PS/2 not possible |
| **Secure Boot** | Optional | Can be enabled with `tools/secureboot.sh`; default is off (unsigned loader) |
| **Graphics validated in QEMU only** | Stated | Real hardware needs matching DRM driver in `modules.load`; module list generated by `tools/stage_modules.py` |
| **Fleet agent HTTP-only** | Implemented | DHCP + route + resolv.conf at boot, outbound HTTP every 30 s; not exercised here against a live fleet server |
| **No audio** | Stated | Media screen is video-only (PNG frames, no sound) |
| **No real video codec** | Stated | Playback is a PNG frame sequence, not H.264/VP9 |
| **Licence** | Ed25519 | Signed file verified offline with an embedded public key; serial key remains for demos |
| **Content persistence** | Works on `HWO_CFG` | VFAT partition; technician can edit from Windows/macOS |
| **State partition optional** | Works without it | Unit runs from RAM; licence/content lost on power off |

---

## 12. File Reference

| File / Path | Purpose |
|---|---|
| `src/hwo.c` | Appliance runtime (id / license / ui / fbprobe / vcsa) |
| `src/hwo.h` | Runtime <-> UI interface |
| `src/fbui.c` | The whole graphical interface: layout, input, present() |
| `src/font.h` | Generated bitmap atlases (393 KB) |
| `src/init.sh` | PID 1: mount, load DRM modules, supervise the UI |
| `src/agent.sh` | Fleet agent (outbound HTTP poll) |
| `src/content.txt` | Default overview content |
| `build.sh` | Rootfs + initramfs + ISO builder |
| `tools/keygen.py` | Licence generator |
| `tools/mkdisk.sh` | Bootable USB/disk image with `HWO_CFG` state partition |
| `tools/mkmedia.sh` | Clip → PNG sequence (ffmpeg + own inflate decoder) |
| `tools/mkfont.py` | TTF → `src/font.h` |
| `tools/stage_modules.py` | Kernel module dependency resolver |
| `tools/secureboot.sh` | Keys init / sign / verify / vars |
| `keys/` | PK/KEK/db key set (chmod 600 on all `.key` files) |
| `dist/` | Build output: ISO, initrd, vmlinuz, SHA256SUMS, disk image |
| `index.html` | Product page (public) |
| `README.md` | Developer / user documentation |
| `SALES.md` | Sales kit (price, licence terms, email templates) |
| `buildroot/` | Cached kernel + rootfs tarballs |
| `dist/` | Build output directory (ISO, disk image, checksums) |

---

## Appendices

### A. Build order summary

```
./build.sh                              # rootfs + initramfs + ISO + signed loader
./tools/mkdisk.sh                       # disk image with two partitions
./tools/secureboot.sh init              # generate key set
./tools/secureboot.sh sign              # sign UEFI loader
./tools/securebot.sh vars               # build OVMF vars
./tools/mkmedia.sh --clip VIDEO --fps N # video → PNG sequence
```

### B. Useful one-liners

```sh
# List input devices in the guest
ls /dev/input/event*

# Check kernel params
cat /proc/cmdline

# Read the licence
cat /etc/hartwell/license

# Read the content
cat /etc/hartwell/content.txt

# Check state partition
mount | grep state

# Rebuild modules list
./tools/stage_modules.py "$ROOT" <modules>
```

### C. Contact

Hartwell Labs  
business@hartwell-labs.pl  
hartwell-labs.pl

Generated with Codebuff 🤖