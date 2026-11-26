# Updating a unit

A unit is updated with a **signed bundle**: a small text manifest followed by the
new initramfs. Nothing about the transport is trusted — the signature is checked
on the appliance against the vendor public key compiled into `hwo`, so a bundle
can arrive on a USB stick, over email, or from a fleet server.

## Make a bundle (vendor side)

```sh
./build.sh                                   # produces build/vmlinuz, build/initrd
./build/hwo-lic make-update keys 1.1.0 build > hwo-1.1.0.bundle
```

`<image>` is a directory holding `vmlinuz` and `initrd`: a hole A/B payload.
Passing a single initramfs file still produces a v1 bundle, which replaces only
the initramfs and borrows the kernel the unit is running.

The manifest the tool writes, and the target rebuilds byte for byte:

```
HWO-UPDATE 2
version=1.1.0
kernel=<BLAKE2b-256 of vmlinuz, hex>
kernellen=<bytes of vmlinuz in the payload>
initrd=<BLAKE2b-256 of initrd, hex>
sig=<Ed25519 over the lines above, hex>

<payload: vmlinuz bytes, then gzip initramfs>
```

The signing key is `keys/hwo-lic.key` — the same vendor key that signs licences.
It never leaves the vendor machine; `keys/` is git-ignored.

## Tested end to end

`tools/ab-qemu-test.py` drives the real ARM64 image in QEMU through three boots:

```sh
./tools/ab-qemu-test.py --image dist/hartwell-os-1.0-arm64.img \
    --bundle build --version 1.1.0
```

It installs a bundle from rescue mode, checks that the inactive slot received
kernel and initramfs while the running slot was untouched, boots the new slot
until the UI watchdog confirms it, then forces `tries=0` on the next boot and
expects the unit to log W9001, rewrite the loader entry and come back on the old
slot. It needs docker (for `qemu-system-aarch64` and mtools) and a built
`build/hwo-lic`; it works on a copy, so the shipped image is never touched.

## Apply it on the unit

```sh
hwo update /media/usb/hwo-1.1.0.bundle
# update 1.1.0 applied, 6575166 bytes; reboot to run it
```

The unit writes the payload into the **inactive slot** of the boot partition
(`LABEL=HWBOOT`, mounted at `/boot` by `/init`): `/boot/slot-a` or `/boot/slot-b`,
whichever is not running. It writes `initrd.new` in that slot, `fsync`s, renames
it over the slot's `initrd`, records `/boot/ver-<slot>`, and only then edits
`/boot/state` — the file that makes the new slot the one the bootloader loads.
The running slot is never opened for writing, so a failed update cannot damage
the image the unit is using.

```
/boot/state          active=b
                     previous=a
                     tries=3
/boot/slot-a/initrd  the image that has been running
/boot/slot-b/initrd  the image just installed
/boot/ver-a          1.0.0
/boot/ver-b          1.1.0
```

## Rollback

`tries` is the number of boots the new slot gets to prove itself. Each boot of
that slot decrements it in `/init`, before the UI starts, so a crash loop cannot
run past it. When the UI has been up for 25 seconds, `hwo bootconfirm` deletes
the counter and the slot is permanent. A boot that finds `tries=0` points the
boot entry back at the previous slot, reboots, and logs **W9001**.

The boot menu is one live entry, `loader/entries/zz-hartwell.conf`, plus
`aa-rescue.conf`. `hwo update` and `hwo rollback` rewrite the live entry's
`initrd` and `hwo.slot=` rather than editing `default` in `loader.conf`: the
`systemd-boot` shipped in these images ignores `default` and boots the entry
that sorts last, so a second slot entry would win the menu on its own. One
entry, rewritten in place, keeps the menu and `/boot/state` in agreement.

Manual control, for a technician who sees a bad image on screen:

```sh
hwo rollback      # point the loader back at the previous slot, then reboot
hwo bootconfirm   # keep the running slot for good, without waiting
```

The **Rescue** boot entry always uses slot a — the image installed with the
unit — so rescue stays available even after slot b has been updated twice.

Reach a shell on a running appliance through the boot menu entry **Rescue**
(kernel flag `hwo.rescue=1`), or run the update at boot from a USB stick.

## What is refused, and why

| Code | Meaning | Result |
|---|---|---|
| **E9001** | signature does not verify, or the payload hash does not match | nothing written, no partial file left |
| **E9002** | the bundle version is not strictly newer than `/etc/hartwell/version.txt` | refused, so an old vulnerable image cannot be replayed |
| **E9003** | no writable boot partition (live ISO, read-only media) | refused; the unit keeps running and says so |
| **E9004** | manifest missing, unreadable, or the write failed | nothing applied |
| **W9001** | the new slot never confirmed within its three boots | the previous slot is booted again; nothing to do at the unit |

`--force` bypasses the version check (E9002) but never the signature: a bundle
signed with another key, or a payload that does not match its hash, is always
rejected.

## Tested

```
valid 1.1.0 over installed 1.0.0   -> applied into slot b, payload byte-identical, ver-b=1.1.0
                                     state active=b previous=a tries=3,
                                     entry now initrd /slot-b/initrd + hwo.slot=b,
                                     slot a untouched
hwo bootconfirm                     -> tries line removed, slot b permanent
hwo rollback                        -> state active=a, entry back to slot a, version reports 1.0.0
same bundle again                  -> E9002
payload byte flipped               -> E9001, no file left behind
bundle signed with another key     -> E9001
no /boot (read-only image)         -> E9003
```

## Limits, stated plainly

- The bundle carries the **initramfs only**. A kernel change still needs a new
  image (and, on ARM, a new `vmlinuz` on the ESP). This is deliberate: the kernel
  is what boots, and swapping it without a second boot slot to fall back to is
  how appliances get bricked.
- **Two slots, kernel included.** Each slot carries its own `vmlinuz` and
  `initrd`, so a kernel change is a normal update and the running slot is never
  overwritten. What is still outside the slots is `systemd-boot` itself.
- **A kernel that panics before `/init` runs is not counted.** The attempts are
  decremented by the initramfs, so a kernel that never reaches `/init` reboots
  into the same bad slot; the unit then needs the Rescue entry or a technician.
  Firmware-level counting (systemd-boot boot counting) would close this and is a
  separate change.
- Confirmation is time-based: 25 seconds of a live UI process. An image that
  starts, looks fine for half a minute and then fails will be confirmed and kept.
- Delivery is manual (USB stick, or a script on the fleet server). There is no
  push channel inside the OS.
