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

`tools/ab-qemu-test.py` drives the real ARM64 image in QEMU through four boots:

```sh
./tools/ab-qemu-test.py --image dist/hartwell-os-1.0-arm64.img \
    --bundle build --version 1.1.0
```

It installs a bundle from rescue mode, checks that the inactive slot received
kernel and initramfs while the running slot was untouched, then boots the new
slot and stops the VM before the watchdog can confirm: the entry on the ESP has
to have been renamed by the loader itself, `zz-live+3-0.conf` to
`zz-live+2-1.conf`. The next boot runs long enough to confirm, and the entry must
come back as `+3-0` with no stale counted names left. The last boot renames the
live entry to `zz-live+0-3.conf`, i.e. a slot the loader has marked bad: the
fallback entry has to boot the previous slot, `/init` has to log W9001 and adopt
it, and both entries must be rewritten so the adopted slot is live again. It
needs docker (for `qemu-system-aarch64` and mtools) and a built `build/hwo-lic`;
it works on a copy, so the shipped image is never touched.

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

There are two attempts turned at once, because they catch different failures.

**The loader counts first.** The live entry is `loader/entries/zz-live+3-0.conf`
and the `+3-0` is read by `systemd-boot` as *three tries left, none used*. Every
boot attempt renames it: `+2-1`, `+1-2`, `+0-3`. At zero the loader marks the
entry bad, sorts it to the top of the menu, and boots the next one — the
uncounted `zz-fallback.conf`, which points at the slot that ran before the
update. This is the counter that survives a kernel panic before `/init`, a
missing initramfs, or a `/init` that hangs: none of those reach the initramfs
code that would otherwise count.

**The initramfs counts second.** `/boot/state` still carries `tries=3`, and
`/init` decrements it before the UI starts. That covers the case where the
kernel boots but `/init` dies, and it is what `hwo rollback` acts on.

**Confirmation resets both.** When the UI has been up for 25 seconds,
`hwo bootconfirm` deletes `tries` from `/boot/state` and rewrites the live entry
back to `zz-live+3-0.conf`, deleting the counted names the loader left behind.

**A fallback is adopted, not fought.** If `/init` comes up on a slot that is not
the active one, the loader gave up: it logs **W9001**, runs `hwo fallback <slot>`
to swap `active`/`previous` in `/boot/state`, rewrites `zz-live+3-0.conf` for the
slot that actually booted and `zz-fallback.conf` for the one that failed, and
carries on without a reboot. Rescue is exempt: it boots slot a on purpose.

```
loader/entries/zz-live+3-0.conf     live slot, counted by the loader
loader/entries/zz-fallback.conf     previous slot, uncounted, never picked while live is healthy
loader/entries/aa-rescue.conf       shell; sorts first, so it is never auto-booted
loader/loader.conf                  timeout only, no default
```

There is no `default` line: the `systemd-boot` in these images boots the entry
that sorts last, and a bad entry sorts first, so the fallback is exactly what
comes up when the live entry runs out of tries. `hwo update`, `hwo rollback`,
`hwo bootconfirm` and `hwo fallback` rewrite these files in place; they never add
entries.

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
zz-live+3-0.conf after a boot       -> renamed by the loader to zz-live+2-1.conf
hwo bootconfirm                     -> tries line removed, entry back to zz-live+3-0.conf,
                                       counted names deleted, slot b permanent
zz-live+0-3.conf (loader gave up)   -> loader boots zz-fallback.conf (slot a), /init logs W9001,
                                       adopts slot a, rewrites both entries
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
- **What boot counting does not cover.** A slot that boots the kernel, reaches
  `/init` and fails *after* the 25 s confirmation is kept. A unit that loses the
  ESP itself (bad flash, corruption) cannot boot either entry. Rescue always uses
  slot a, so on a unit that has been updated twice the rescue image is the
  original one, not the previous update.
- Confirmation is time-based: 25 seconds of a live UI process. An image that
  starts, looks fine for half a minute and then fails will be confirmed and kept.
- Delivery is manual (USB stick, or a script on the fleet server). There is no
  push channel inside the OS.
