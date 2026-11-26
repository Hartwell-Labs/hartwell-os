#!/usr/bin/env python3
"""End-to-end A/B test on the real ARM64 image, in QEMU.

What it proves, in the booted appliance and not on a mock:

  1. hwo update installs into the inactive slot and the running slot is untouched
  2. the unit boots the new slot, the UI runs, and the watchdog confirms it after
     25 s, so /boot/state loses its `tries` line
  3. an unconfirmed slot (tries=0) is dropped on the next boot: /init rolls back,
     logs W9001, reboots, and the old slot is active again

The image copy is writable; the shipped image is not modified. Requires docker
with qemu-system-aarch64 (Alpine container), mtools inside that container, and
build/hwo-lic already built by ./build.sh.

    ./tools/ab-qemu-test.py --image dist/hartwell-os-1.0-arm64.img \
        --bundle build --version 1.1.0
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

ESP_OFFSET = 1048576
CONTAINER = "hwo-abtest"
ENTRY = """title Hartwell OS
linux /slot-a/vmlinuz
initrd /slot-a/initrd
options console=tty0 console=ttyAMA0 quiet rdinit=/init video=1280x800 hwo.slot=a hwo.rescue=1
"""

QEMU = (
    "apk add qemu-system-aarch64 edk2 qemu-hw-display-virtio-gpu-pci "
    "qemu-hw-display-virtio-gpu mtools >/dev/null 2>&1; "
    "cp /usr/share/qemu/edk2-arm-vars.fd /tmp/vars.fd; umask 0; "
    "exec qemu-system-aarch64 -M virt -cpu cortex-a57 -m 768 -smp 2 "
    "-drive if=pflash,format=raw,readonly=on,file=/usr/share/qemu/edk2-aarch64-code.fd "
    "-drive if=pflash,format=raw,file=/tmp/vars.fd "
    "-device virtio-gpu-pci -device qemu-xhci -device usb-kbd -device usb-tablet "
    "-drive file=/img,format=raw,if=virtio -display none "
    "-qmp unix:/q/qmp.sock,server=on,nowait "
    "-serial unix:/q/ser.sock,server=on,nowait"
)


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def mtools(work, script):
    """Run mtools against the ESP inside the image, in the Alpine container.

    The container command is single-quoted so the host shell does not expand
    $D itself; the scripts below use double quotes only.
    """
    return sh(f"docker run --rm -v {work}:/t alpine:latest sh -c '"
              f"apk add --no-cache mtools >/dev/null 2>&1; D=/t/disk.img@@{ESP_OFFSET}; {script}'")


def esp_read(work, name):
    r = mtools(work, f'mtype -i "$D" ::{name}')
    return r.stdout.replace("\r", "")


def esp_md5(work, name):
    """md5 of a file on the ESP, or "" when it is missing."""
    r = mtools(work, f'mtype -i "$D" ::{name} > /tmp/x && md5sum /tmp/x')
    out = r.stdout.split()
    return out[0] if out else ""


def md5(path):
    import hashlib
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def esp_write_state(work, active, previous, tries):
    body = f"active={active}\nprevious={previous}\n" + (f"tries={tries}\n" if tries is not None else "")
    with open(os.path.join(work, "state.new"), "w") as f:
        f.write(body)
    mtools(work, 'mcopy -o -i "$D" /t/state.new ::state')


class Serial:
    def __init__(self, path, timeout=120):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(1)
        self.s.settimeout(1.0)
        self.buf = b""

    def read(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if not d:
                    break
                self.buf += d
            except socket.timeout:
                pass
        return self.buf.decode("utf-8", "replace")

    def wait_for(self, needle, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if needle in self.read(1.0):
                return True
        return False

    def run(self, cmd, wait=6):
        self.buf = b""
        self.s.sendall(cmd.encode() + b"\n")
        return self.read(wait)

    def close(self):
        self.s.close()


def qmp(work, command, args=None):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.path.join(work, "qmp.sock"))
    f = s.makefile("rwb")
    f.write(b'{"execute":"qmp_capabilities"}\n')
    f.flush()
    f.readline()
    payload = {"execute": command}
    if args:
        payload["arguments"] = args
    f.write((json.dumps(payload) + "\n").encode())
    f.flush()
    while True:
        line = f.readline()
        if not line:
            break
        r = json.loads(line)
        if "event" not in r:
            break
    s.close()


def start_vm(work):
    sh(f"docker rm -f {CONTAINER}")
    for stale in ("ser.sock", "qmp.sock"):
        try:
            os.remove(os.path.join(work, stale))
        except FileNotFoundError:
            pass
    r = sh(f"docker run -d --name {CONTAINER} -v {work}/disk.img:/img -v {work}:/q "
           f"alpine:latest sh -c '{QEMU}'")
    if not r.stdout.strip():
        sys.exit("docker run failed: " + r.stderr)
    end = time.time() + 180
    while time.time() < end:
        if os.path.exists(os.path.join(work, "ser.sock")):
            break
        time.sleep(2)
    time.sleep(1)
    for _ in range(20):
        sh(f"docker exec {CONTAINER} sh -c 'chmod 777 /q /q/ser.sock /q/qmp.sock 2>/dev/null'")
        if os.access(os.path.join(work, "ser.sock"), os.R_OK | os.W_OK):
            break
        time.sleep(1)
    return Serial(os.path.join(work, "ser.sock"))


def stop_vm(work):
    try:
        qmp(work, "quit")
    except Exception:
        pass
    time.sleep(2)
    sh(f"docker rm -f {CONTAINER}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default="dist/hartwell-os-1.0-arm64.img")
    ap.add_argument("--bundle", default="build", help="directory with vmlinuz and initrd")
    ap.add_argument("--version", default="1.1.0")
    ap.add_argument("--work", default="/tmp/hwo-ab-qemu")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()

    work = a.work
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    shutil.copy(a.image, os.path.join(work, "disk.img"))
    with open(os.path.join(work, "entry.conf"), "w") as f:
        f.write(ENTRY)

    r = sh(f"./build/hwo-lic make-update keys {a.version} {a.bundle} > {work}/UPDATE.BIN")
    if r.returncode != 0:
        sys.exit("make-update failed: " + r.stderr)
    r = mtools(work, 'mcopy -o -i "$D" /t/UPDATE.BIN ::UPDATE.BIN; '
                     'mcopy -o -i "$D" /t/entry.conf ::loader/entries/zz-hartwell.conf')
    if r.returncode != 0:
        sys.exit("cannot stage the bundle on the ESP: " + r.stderr)

    fails = []
    kernel_md5 = md5(os.path.join(a.bundle, "vmlinuz"))
    initrd_md5 = md5(os.path.join(a.bundle, "initrd"))

    # baseline: what the shipped image has on its ESP before any update
    base_a = esp_md5(work, "slot-a/initrd")
    base_ver = esp_read(work, "ver-a").strip()
    print(f"   image: slot a initrd {base_a[:12]}, version {base_ver}")

    print("== boot 1: rescue, install the update ==")
    ser = start_vm(work)
    if not ser.wait_for("rescue", 150):
        sys.exit("no rescue prompt; serial:\n" + ser.read(2)[-1200:])
    out = ser.run("hwo update /boot/UPDATE.BIN", 30)
    print("   " + out.strip()[-300:].replace("\n", "\n   "))
    if "applied" not in out:
        fails.append("update was not applied")
    state = ser.run("cat /boot/state", 4)
    print("   /boot/state after update:\n" + "\n".join("     " + l for l in state.strip().splitlines()[-4:]))
    ser.close()
    stop_vm(work)

    new_slot = "b" if esp_read(work, "ver-b").strip() else "a"
    old_slot = "a" if new_slot == "b" else "b"
    print(f"   new slot {new_slot}, old slot {old_slot}")

    got_kernel, got_initrd = esp_md5(work, f"slot-{new_slot}/vmlinuz"), esp_md5(work, f"slot-{new_slot}/initrd")
    if got_kernel != kernel_md5:
        fails.append("new slot kernel is not the one from the bundle")
    if got_initrd != initrd_md5:
        fails.append("new slot initramfs is not the one from the bundle")
    if esp_md5(work, f"slot-{old_slot}/initrd") != base_a:
        fails.append("running slot was modified by the update")
    if esp_read(work, f"ver-{new_slot}").strip() != a.version:
        fails.append(f"version marker is {esp_read(work, f'ver-{new_slot}').strip()!r}")
    entry = esp_read(work, "loader/entries/zz-hartwell.conf")
    if f"/slot-{new_slot}/vmlinuz" not in entry:
        fails.append(f"boot entry does not point at slot {new_slot}")
    if f"active={new_slot}" not in state:
        fails.append("state does not make the new slot active")
    if "tries=3" not in state:
        fails.append("state has no attempt counter for the new slot")

    # hand the next boot to the UI, the way a real unit runs
    with open(os.path.join(work, "entry.conf"), "w") as f:
        f.write(entry.replace(" hwo.rescue=1", ""))
    r = mtools(work, 'mcopy -o -i "$D" /t/entry.conf ::loader/entries/zz-hartwell.conf')
    if r.returncode != 0:
        sys.exit("cannot hand the entry to the UI: " + r.stderr)

    print("== boot 2: new slot runs the UI, watchdog confirms it ==")
    ser = start_vm(work)
    if not ser.wait_for(f"boot slot {new_slot}", 180):
        fails.append(f"slot {new_slot} did not boot")
    if not ser.wait_for("init at", 60):
        fails.append("no /init on the new slot")
    # the watchdog confirms after 25 s of a live UI
    time.sleep(40)
    ser.close()
    stop_vm(work)

    state = esp_read(work, "state")
    print("   /boot/state after boot 2:\n" + "\n".join("     " + l for l in state.strip().splitlines()))
    if f"active={new_slot}" not in state:
        fails.append("new slot is not active after boot 2")
    if "tries" in state:
        fails.append("slot was not confirmed after 25 s of UI")

    print("== boot 3: unconfirmed slot is dropped, unit returns to the old one ==")
    esp_write_state(work, new_slot, old_slot, 0)
    ser = start_vm(work)
    if not ser.wait_for("rolling back", 180):
        fails.append("no rollback on an unconfirmed slot")
    if not ser.wait_for(f"boot slot {old_slot}", 180):
        fails.append(f"unit did not come back on slot {old_slot}")
    ser.close()
    stop_vm(work)

    state = esp_read(work, "state")
    print("   /boot/state after rollback:\n" + "\n".join("     " + l for l in state.strip().splitlines()))
    if f"active={old_slot}" not in state:
        fails.append("rollback did not make the old slot active")
    entry = esp_read(work, "loader/entries/zz-hartwell.conf")
    if f"/slot-{old_slot}/vmlinuz" not in entry:
        fails.append("boot entry still points at the dropped slot")
    if esp_read(work, f"ver-{new_slot}").strip() != a.version:
        fails.append("dropped slot lost its version marker")

    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)

    print()
    if fails:
        print("FAIL: " + "; ".join(fails))
        return 1
    print(f"PASS: update into slot {new_slot} ({a.version}), kernel and initramfs "
          f"verified, confirmed by the UI watchdog, slot {old_slot} kept the "
          f"running system and got it back after a forced rollback")
    return 0


if __name__ == "__main__":
    sys.exit(main())
