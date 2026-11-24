#!/usr/bin/env python3
"""Issue a signed Hartwell OS licence for one device.

The unit prints its id on the unlicensed screen (`hwo id`). This tool signs that
id with the vendor EdDSA key in `keys/` (via the compiled `hwo-lic`) and prints a
`/state/license` file that the appliance verifies offline against the public key
embedded in the binary. The signature cannot be copied to another unit and cannot
be forged from the public binary.

    ./tools/keygen.py --uuid 9d2f7c1e-...        # derive the id from DMI uuid
    ./tools/keygen.py --device HWO-76DF-0AFB     # use an id shown on the unit

Requires `keys/hwo-lic.key` (seller seed). Create it once with:
    gcc -O2 -o build/hwo-lic tools/hwo-lic.c src/monocypher.c
    ./build/hwo-lic gen keys
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIC = os.path.join(ROOT, "build", "hwo-lic")
KEYS = os.path.join(ROOT, "keys")


def fnv1a(text: str) -> int:
    h = 2166136261
    for byte in text.encode():
        h ^= byte
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def device_id_from_uuid(uuid: str) -> str:
    h = fnv1a(uuid)
    return f"HWO-{(h >> 16) & 0xFFFF:04X}-{h & 0xFFFF:04X}"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--uuid", help="DMI product_uuid of the target machine")
    ap.add_argument("--device", help="device id printed by the unit")
    a = ap.parse_args()

    if a.device:
        device = a.device
    elif a.uuid:
        device = device_id_from_uuid(a.uuid.strip())
    else:
        ap.error("give --uuid or --device")

    if not os.path.exists(LIC):
        sys.exit(f"missing {LIC}; build it: gcc -O2 -o build/hwo-lic tools/hwo-lic.c src/monocypher.c")
    if not os.path.exists(os.path.join(KEYS, "hwo-lic.key")):
        sys.exit(f"missing {KEYS}/hwo-lic.key; create it: {LIC} gen keys")

    out = subprocess.run([LIC, "sign", KEYS, device], check=True,
                         capture_output=True, text=True).stdout
    print(out.strip())
    print()
    print("# /state/license (copy this file to the HWO_CFG partition)")


if __name__ == "__main__":
    main()
