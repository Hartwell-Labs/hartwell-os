#!/usr/bin/env python3
"""Copy a module and its dependencies into an initramfs rootfs and emit a load order.

The kernel's own .modinfo `depends=` field is the dependency graph; no depmod needed,
so this works for a kernel tree that was never installed on the build host.

usage: stage_modules.py ROOTFS MODULE [MODULE ...]
"""
import gzip
import os
import re
import shutil
import sys

BASE = os.environ.get("HWO_BUILDROOT", "buildroot")
SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), BASE, "modules")

MODDIR = None
for cand in sorted(os.listdir(os.path.join(SRC, "lib", "modules"))):
    MODDIR = os.path.join(SRC, "lib", "modules", cand)
    break
if not MODDIR or not os.path.isdir(MODDIR):
    raise SystemExit(f"no module tree under {SRC}")

INDEX = {}
for root, dirs, files in os.walk(MODDIR):
    for f in files:
        if f.endswith(".ko.gz"):
            INDEX[f[: -len(".ko.gz")].replace("-", "_")] = os.path.join(root, f)

VER = os.path.basename(MODDIR)


def deps_of(name):
    path = INDEX.get(name)
    if not path:
        if os.environ.get("HWO_ALLOW_MISSING"):
            print(f"    skip {name}: not in this kernel tree", file=sys.stderr)
            return []
        raise SystemExit(f"module not found: {name}\nhave: {', '.join(sorted(INDEX)[:20])} ...")
    with gzip.open(path, "rb") as fh:
        blob = fh.read()
    m = re.search(rb"depends=([^\x00]*)", blob)
    if not m:
        return []
    return [d.replace("-", "_") for d in m.group(1).decode().split(",") if d]


def resolve(name, seen, order):
    if name in seen:
        return
    seen.add(name)
    for d in deps_of(name):
        resolve(d, seen, order)
    order.append(name)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    rootfs, wanted = sys.argv[1], sys.argv[2:]
    seen, order = set(), []
    for w in wanted:
        resolve(w.replace("-", "_"), seen, order)

    dest = os.path.join(rootfs, "lib", "modules", VER)
    lines = []
    for name in order:
        src = INDEX[name]
        sub = os.path.relpath(os.path.dirname(src), MODDIR)
        odir = os.path.join(dest, sub)
        os.makedirs(odir, exist_ok=True)
        ko = odir + "/" + os.path.basename(src)[: -len(".gz")]
        with gzip.open(src, "rb") as fin, open(ko, "wb") as fout:
            shutil.copyfileobj(fin, fout)
        os.chmod(ko, 0o644)
        lines.append(f"{sub}/{os.path.basename(ko)}")
        print(f"  + {name}")
    with open(os.path.join(rootfs, "etc", "hartwell", "modules.load"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"{len(order)} modules staged into lib/modules/{VER}")


main()
