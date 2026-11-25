# Error codes — the book

The appliance has no shell, so every failure has to be readable from a serial
console and quotable to support. Each one gets a stable code, one line of
context, and an entry here that goes from symptom to verified fix.

## Where the codes appear

| Place | How to read it |
|---|---|
| Serial console (115200 8N1) | every event is echoed the moment it happens |
| `/run/hwo.err` | one line per event: `0012s E1001 E  cannot open /dev/fb0` |
| The unit itself | the **System** screen: one amber `N WARN` or red `N ERR` chip next to the state |
| The unit itself | `hwo errs` in rescue mode, or `hwo errs > /dev/ttyS0` |
| Support | quote the code and the line; timestamps are seconds since kernel start |

The log is in RAM and resets on reboot. If a unit dies repeatedly, capture the
serial output before power-cycling it.

## Getting a shell — rescue mode only

The shipped image has no shell, no login and no open port. The single door is a
kernel command line flag, which means **physical access to the boot menu**:

1. Boot the ISO or the disk and press a key at the bootloader prompt.
2. Choose **Rescue** (or append `hwo.rescue=1` to the kernel line).
3. The unit prints the error log and drops to `/bin/sh` on the console.
4. `hwo errs` prints the log again plus the fix line for every code; `hwo id`
   prints the unit id; `cat /run/hwo.err` is the raw log.
5. Leaving: type `exit`, then power-cycle. Nothing is written unless you do it.

In rescue the binary is `/hwo`; `PATH` includes `/`, so `hwo errs` works as
written. If a unit ever drops you into a bare shell without it, run `/hwo errs`.

Rescue mode does not start the UI and does not start the fleet agent. It is
deliberately not reachable over the network.

## Codes

| Code | Severity | Meaning |
|---|---|---|
| F1001 | fatal | no framebuffer (`/dev/fb0` missing or no usable mode) |
| F1002 | fatal | framebuffer exists but the pixel format is unsupported |
| F1003 | error | the interface exited (each exit is a crash) |
| F1004 | fatal | five exits in one boot → the supervisor reboots |
| E2001 | error | licence file unreadable |
| E2002 | error | licence is not valid on this unit (device or signature) |
| E2003 | error | typed licence key rejected |
| W3001 | warning | content file missing, built-in rows are shown |
| W3002 | warning | content parsed to zero rows |
| W4001 | warning | no `HWO_CFG` state partition, everything is in RAM |
| E4002 | error | state partition found but will not mount read-write |
| W6001 | warning | media directory unreadable |
| W6002 | warning | one media frame failed to decode, playback skips it |
| W7001 | warning | clock is not set (no RTC, no network time) |
| E9001 | error | update bundle signature or payload hash invalid |
| E9002 | error | update is older than the running image (downgrade refused) |
| E9003 | error | no writable boot partition for the update |
| E9004 | error | update bundle unreadable, or the write failed |
| W9001 | warning | the new slot never confirmed itself, the previous image is booted |

---

## F1001 — no framebuffer

**Symptom.** Serial prints `hartwell-os: no framebuffer, falling back to text
console`, the screen stays black or shows a text console, and the UI never
appears.

**Cause.** The GPU driver did not bind. Usually the machine has a GPU whose DRM
module is not in `modules.load`, or the panel is wired to an output the kernel
did not initialise.

**Diagnose.** In rescue mode:

```sh
cat /run/modprobe.log              # which modules failed to load
ls /sys/class/drm/                 # what the kernel actually registered
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null
dmesg | grep -Ei 'drm|fb0|i915|amdgpu|simpledrm'
```

**Fix.** Add the missing driver to the module list and rebuild:

```sh
grep -ril "$(lspci -nn | grep -i vga)" /lib/modules/*/modules.dep >/dev/null
tools/stage_modules.py build/rootfs simpledrm i915 amdgpu nouveau virtio_gpu ...
```

For a one-off machine, booting with `nomodeset` gets a simplefb framebuffer and
the UI will run at whatever mode the firmware set.

**Verify.** `/dev/fb0` exists, `hwo fbprobe` draws a test pattern, then reboot
and confirm the error log has no F1001.

## F1002 — framebuffer unsupported

**Symptom.** UI never starts; serial shows the bpp value.

**Cause.** The mode is 8 bpp, or smaller than 800×600.

**Fix.** Set a supported mode in the bootloader (`vga=0x0318` for 1024×768×32 or
`video=1280x800@60` with KMS) and reboot.

**Verify.** `hwo fbprobe` exits 0 and paints the screen.

## F1003 — interface exited

**Symptom.** The screen blanks and comes back; serial counts exits.

**Cause.** A crash in the UI process. One exit is survivable, five trigger F1004.

**Diagnose.** Rescue mode, then read the log and check what changed last: the
content file, a licence edit, a media directory.

```sh
hwo errs
ls -l /etc/hartwell /state 2>/dev/null
```

**Fix.** Restore the last good content/licence, or boot a known-good image. If it
reproduces on a clean image, capture the serial output and report it — the exit
is a bug, not a configuration problem.

## F1004 — crash loop

**Symptom.** The unit reboots every few seconds.

**Cause.** Five UI exits in one boot: the supervisor reboots rather than sit on a
dead screen.

**Fix.** Boot with `hwo.rescue=1`, run `hwo errs`, fix the cause of the F1003
entries, power-cycle. If the image itself is broken, reflash the update bundle.

## E2001 — licence file unreadable

**Symptom.** The unit boots to the activation screen although a licence was
applied.

**Cause.** `/etc/hartwell/license` or `/state/license` exists but is not the
expected `key=` / `device=` / `licence=` text (truncated copy, wrong encoding,
disk full when it was written).

**Diagnose.** `cat /state/license` — it must be plain ASCII, one field per line.

**Fix.** Re-apply the licence: type the key on the activation screen, or copy a
known-good file onto the `HWO_CFG` partition from another machine.

## E2002 — licence not valid here

**Symptom.** `HWO-…` key is on the unit, still unlicensed; log says which device
the licence belongs to.

**Cause.** The licence was issued for a different DMI UUID (board swapped, image
cloned from another unit) or the signature does not verify (file edited).

**Diagnose.** `hwo id` and compare with the `device=` line in the licence file.

**Fix.** Request a licence for the current id from `business@hartwell-labs.pl`.
Cloning a disk image between machines will always produce this code; that is the
point of binding to the board.

## E2003 — licence key rejected

**Symptom.** Typing the key on the activation screen leaves the unit unlicensed.

**Cause.** Wrong format (`HWO-YYYY-XXXX`), wrong checksum, or a key issued for a
different id.

**Fix.** Re-type carefully (the screen uppercases), or use the signed licence
file, which takes precedence and cannot be mistyped.

## W3001 / W3002 — content

**Symptom.** Overview shows the built-in rows instead of the customer's.

**Cause.** `/etc/hartwell/content.txt` (and `/state/content.txt`) missing, or it
parsed to nothing.

**Diagnose.** `head -5 /state/content.txt` — first line is the title, then
`Label | value` rows.

**Fix.** Copy a valid file to the `HWO_CFG` partition and reboot; the file is
copied into the image at boot.

## W4001 — no state partition

**Symptom.** Licence and content disappear after a reboot.

**Cause.** No partition labelled `HWO_CFG`. The ISO always runs this way; a disk
install without `tools/mkdisk.sh` does too.

**Fix.** Write the disk image (`tools/mkdisk.sh`) instead of booting the raw ISO,
so the state partition exists.

## E4002 — state partition mount failed

**Symptom.** `HWO_CFG` exists (`blkid` sees the label) but `/state` is empty.

**Cause.** Filesystem damaged, or the partition is locked by a previous unclean
unmount.

**Diagnose.** `blkid`, then `fsck.vfat -n /dev/sdX1`.

**Fix.** `fsck.vfat -a` from another machine, or reformat and re-apply licence
and content. Data on that partition is licence + content only; both are
reproducible.

## W6001 / W6002 — media

**Symptom.** Media screen says no media, or playback stutters/skips.

**Cause.** Directory unreadable (W6001) or one PNG corrupt (W6002).

**Fix.** Regenerate the sequence with `tools/mkmedia.sh`; frames must be 8-bit
PNG, non-interlaced, named `frameNNNN.png`.

## W7001 — clock not set

**Symptom.** Header clock shows a date in 1970 or a wrong hour.

**Cause.** No RTC battery and no network lease, so nothing set the clock.

**Fix.** Give the unit network access (it takes a DHCP lease at boot) or set the
clock in rescue mode with `date -s`. The UI reports UTC.

## E9001 / E9002 / E9003 / E9004 — updates

**Symptom.** `hwo update BUNDLE` prints one of these and the unit keeps running
the old image (the UI is untouched: a refused update never affects the screen).

| Code | Cause | Fix |
|---|---|---|
| E9001 | the manifest signature does not verify against the vendor key compiled into `hwo`, or the payload's BLAKE2b does not match the `hash=` line | use a bundle built by `tools/hwo-lic make-update` with the vendor key; a re-signed or truncated file is refused |
| E9002 | `version=` is not strictly newer than `/etc/hartwell/version.txt` | build a newer bundle, or pass `--force` when deliberately reverting to a known-good signed image |
| E9003 | no writable boot partition at `/boot` (live ISO, read-only media) | update a disk install, or boot the unit from media with an `HWBOOT` partition |
| E9004 | the file has no manifest, or the copy to `/boot` failed | re-copy the bundle and check free space on the boot partition |

**Verify.** After a successful update: `/boot/state` names the new slot and its
tries, `/boot/slot-<x>/initrd` is the new payload, `/boot/ver-<x>` the new
version, and the serial log on the next boot prints `hartwell-os: boot slot x`
followed by `hartwell-os: boot ...`. See `docs/updates.md` for the bundle format
and the vendor-side commands.

## W9001 — update not confirmed

**Symptom.** The serial log on boot says `slot b never confirmed, rolling back
to a`, then the unit reboots into the previous image and the UI comes up
normally.

**Cause.** The update was installed into the inactive slot, but that image never
reached a live UI long enough to call `hwo bootconfirm` within its three boot
attempts. Usual reasons: a GPU driver the new image loads differently, a crashed
UI, or a broken licence file on `/state`.

**Fix.** Nothing on the unit: the rollout is already undone. On the bench, boot
the image with `hwo.debug=1` to see the module log, or run `hwo rollback` by hand
to force the fallback before it expires. Details in `docs/updates.md`,
"Rollback".
