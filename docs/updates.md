# Updating a unit

A unit is updated with a **signed bundle**: a small text manifest followed by the
new initramfs. Nothing about the transport is trusted — the signature is checked
on the appliance against the vendor public key compiled into `hwo`, so a bundle
can arrive on a USB stick, over email, or from a fleet server.

## Make a bundle (vendor side)

```sh
./build.sh                                   # produces build/initrd
./build/hwo-lic make-update keys 1.1.0 build/initrd > hwo-1.1.0.bundle
```

The manifest the tool writes, and the target rebuilds byte for byte:

```
HWO-UPDATE 1
version=1.1.0
hash=<BLAKE2b-256 of the payload, hex>
sig=<Ed25519 over the three lines above, hex>

<payload: gzip initramfs>
```

The signing key is `keys/hwo-lic.key` — the same vendor key that signs licences.
It never leaves the vendor machine; `keys/` is git-ignored.

## Apply it on the unit

```sh
hwo update /media/usb/hwo-1.1.0.bundle
# update 1.1.0 applied, 6575166 bytes; reboot to run it
```

The unit writes the payload to the boot partition (`LABEL=HWBOOT`, mounted at
`/boot` by `/init`) as `initrd.new`, `fsync`s, renames it over `initrd`, and only
then updates `/boot/version.txt`. A half-applied update therefore never claims to
be the new version.

Reach a shell on a running appliance through the boot menu entry **Rescue**
(kernel flag `hwo.rescue=1`), or run the update at boot from a USB stick.

## What is refused, and why

| Code | Meaning | Result |
|---|---|---|
| **E9001** | signature does not verify, or the payload hash does not match | nothing written, no partial file left |
| **E9002** | the bundle version is not strictly newer than `/etc/hartwell/version.txt` | refused, so an old vulnerable image cannot be replayed |
| **E9003** | no writable boot partition (live ISO, read-only media) | refused; the unit keeps running and says so |
| **E9004** | manifest missing, unreadable, or the write failed | nothing applied |

`--force` bypasses the version check (E9002) but never the signature: a bundle
signed with another key, or a payload that does not match its hash, is always
rejected.

## Tested

```
valid 1.1.0 over installed 1.0.0   -> applied, /boot/initrd byte-identical, marker 1.1.0
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
- There is no A/B slot yet, and no automatic rollback: a bundle that boots but
  misbehaves is reverted by applying the older signed bundle with `--force`.
- Delivery is manual (USB stick, or a script on the fleet server). There is no
  push channel inside the OS.
