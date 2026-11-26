#!/usr/bin/env python3
"""End-to-end A/B and boot-counting test on the real ARM64 image, in QEMU.

What it proves, in the booted appliance and not on a mock:

  1. hwo update installs into the inactive slot and the running slot is untouched
  2. the loader itself counts boot attempts in the entry file name
     (zz-live+3-0.conf -> zz-live+2-1.conf on the next boot), which is the only
     counter that survives a kernel panic before /init
  3. the unit boots the new slot, the UI runs, and the watchdog confirms it after
     25 s: the entry name goes back to +3-0 and /boot/state loses its `tries`
  4. a live entry the loader has marked bad (+0-3) is skipped: the fallback
     entry boots the previous slot, /init logs W9001, adopts it, and the two
     entries are rewritten so the adopted slot is the live one

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
LIVE = "loader/entries/zz-live+3-0.conf"
FALLBACK = "loader/entries/zz-fallback.conf"
LIVE_HEAD = "+3-0"      # a fresh live entry, as build.sh and hwo write it
# The shipped rescue entry keeps the shell on the framebuffer (tty0). A bench
# with a serial cable puts ttyAMA0 last instead, which is where /dev/console and
# therefore the shell's stdin end up; the test needs that to drive the unit.
RESCUE = """title Rescue (shell, no UI)
linux /slot-a/vmlinuz
initrd /slot-a/initrd
options console=tty0 console=ttyAMA0 rdinit=/init video=1280x800 hwo.rescue=1
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


def esp_write(work, name, local):
    """copy a file staged in the work dir onto the ESP"""
    return mtools(work, f'mcopy -o -i "$D" /t/{local} ::{name}')


def esp_rename(work, src, dst):
    return mtools(work, f'mren -i "$D" ::{src} ::{dst}')


def esp_exists(work, name):
    r = mtools(work, f'mdir -b -i "$D" ::{name}')
    return name in r.stdout


def to_serial(entry):
    """put ttyAMA0 last: with tty0 last the kernel console is the framebuffer and
    /init's log lines never reach the serial port the test is listening on"""
    return entry.replace("console=ttyAMA0 console=tty0", "console=tty0 console=ttyAMA0")


def esp_entries(work):
    r = mtools(work, 'mdir -b -i "$D" ::/loader/entries')
    return [l.strip().split("::/loader/entries/")[-1] for l in r.stdout.splitlines() if l.strip()]


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

    def shell(self, seconds=120):
        """wait until the rescue shell answers, not just the word 'rescue' in
        the cmdline echo or the banner"""
        end = time.time() + seconds
        while time.time() < end:
            out = self.run("echo HWO_READY", 4)
            if "HWO_READY" in out:
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

    r = sh(f"./build/hwo-lic make-update keys {a.version} {a.bundle} > {work}/UPDATE.BIN")
    if r.returncode != 0:
        sys.exit("make-update failed: " + r.stderr)
    r = mtools(work, 'mcopy -o -i "$D" /t/UPDATE.BIN ::UPDATE.BIN')
    if r.returncode != 0:
        sys.exit("cannot stage the bundle on the ESP: " + r.stderr)

    fails = []
    kernel_md5 = md5(os.path.join(a.bundle, "vmlinuz"))
    initrd_md5 = md5(os.path.join(a.bundle, "initrd"))

    # baseline: what the shipped image has on its ESP before any update
    base_a = esp_md5(work, "slot-a/initrd")
    base_ver = esp_read(work, "ver-a").strip()
    print(f"   image: slot a initrd {base_a[:12]}, version {base_ver}")
    installed = esp_entries(work)
    print(f"   entries: {', '.join(sorted(installed))}")
    if not esp_exists(work, LIVE) or not esp_exists(work, FALLBACK):
        sys.exit(f"image has no {LIVE} / {FALLBACK}: {installed}")

    # boot 1 hands the machine to the rescue entry and to nothing else: with the
    # live and fallback entries out of the way the rescue entry is the only one
    # the loader can pick, which is how a technician at the console forces it
    print("== boot 1: rescue, install the update ==")
    esp_rename(work, "loader/entries/zz-fallback.conf", "loader/entries/zz-fallback.off")
    esp_rename(work, "loader/entries/zz-live+3-0.conf", "loader/entries/zz-live.off")
    with open(os.path.join(work, "rescue.conf"), "w") as f:
        f.write(RESCUE)
    r = esp_write(work, "loader/entries/aa-rescue.conf", "rescue.conf")
    if r.returncode != 0:
        sys.exit("cannot stage the rescue entry: " + r.stderr)
    ser = start_vm(work)
    if not ser.wait_for("rescue console", 180) or not ser.shell():
        sys.exit("no rescue shell; serial:\n" + ser.read(2)[-1200:])
    out = ser.run("hwo update /boot/UPDATE.BIN", 30)
    print("   " + out.strip()[-300:].replace("\n", "\n   "))
    if "applied" not in out:
        fails.append("update was not applied")
    state = ser.run("cat /boot/state", 4)
    print("   /boot/state after update:\n" + "\n".join("     " + l for l in state.strip().splitlines()[-4:]))
    ser.close()
    stop_vm(work)

    entries = esp_entries(work)
    new_slot = "b" if esp_read(work, "ver-b").strip() else "a"
    old_slot = "a" if new_slot == "b" else "b"
    print(f"   new slot {new_slot}, old slot {old_slot}, entries: {', '.join(sorted(entries))}")

    got_kernel, got_initrd = esp_md5(work, f"slot-{new_slot}/vmlinuz"), esp_md5(work, f"slot-{new_slot}/initrd")
    if got_kernel != kernel_md5:
        fails.append("new slot kernel is not the one from the bundle")
    if got_initrd != initrd_md5:
        fails.append("new slot initramfs is not the one from the bundle")
    if esp_md5(work, f"slot-{old_slot}/initrd") != base_a:
        fails.append("running slot was modified by the update")
    if esp_read(work, f"ver-{new_slot}").strip() != a.version:
        fails.append(f"version marker is {esp_read(work, f'ver-{new_slot}').strip()!r}")
    if not esp_exists(work, LIVE):
        fails.append(f"update did not write a counted live entry {LIVE}")
    else:
        entry = esp_read(work, LIVE)
        if f"/slot-{new_slot}/vmlinuz" not in entry:
            fails.append(f"live entry does not point at slot {new_slot}")
        if f"hwo.slot={new_slot}" not in entry:
            fails.append("live entry cmdline does not carry the new slot")
    fb = esp_read(work, FALLBACK) if esp_exists(work, FALLBACK) else ""
    if f"/slot-{old_slot}/vmlinuz" not in fb:
        fails.append(f"fallback entry does not point at slot {old_slot}")
    if f"active={new_slot}" not in state:
        fails.append("state does not make the new slot active")
    if "tries=3" not in state:
        fails.append("state has no attempt counter for the new slot")

    # the same console swap for both remaining entries, so their /init logs land
    # on the serial port instead of only on the screen
    for name in (LIVE, FALLBACK):
        text = esp_read(work, name) if esp_exists(work, name) else ""
        if text and text != to_serial(text):
            with open(os.path.join(work, "entry.conf"), "w") as f:
                f.write(to_serial(text))
            esp_write(work, name, "entry.conf")

    # boot 2: start the UI and kill the VM before the watchdog can confirm, so
    # the only counter that ran is the loader's own one in the file name
    print("== boot 2: the loader counts the attempt down (no confirmation) ==")
    ser = start_vm(work)
    if not ser.wait_for(f"boot slot {new_slot}", 180):
        fails.append(f"slot {new_slot} did not boot")
    if not ser.wait_for("init at", 60):
        fails.append("no /init on the new slot")
    ser.close()
    stop_vm(work)
    counted = esp_entries(work)
    print(f"   entries after the boot: {', '.join(sorted(counted))}")
    if "zz-live+2-1.conf" not in counted:
        fails.append(f"the loader did not count the boot down: {sorted(counted)}")
    if "zz-live+3-0.conf" in counted:
        fails.append("the loader left the freshly counted name in place")

    # boot 3: let the watchdog confirm, which must reset the count to +3-0
    print("== boot 3: the UI runs long enough to confirm the slot ==")
    ser = start_vm(work)
    if not ser.wait_for(f"boot slot {new_slot}", 180):
        fails.append(f"slot {new_slot} did not boot on the second run")
    if not ser.wait_for("init at", 60):
        fails.append("no /init on the confirming boot")
    time.sleep(40)
    ser.close()
    stop_vm(work)

    state = esp_read(work, "state")
    print("   /boot/state after the confirming boot:\n" +
          "\n".join("     " + l for l in state.strip().splitlines()))
    if f"active={new_slot}" not in state:
        fails.append("new slot is not active after the confirming boot")
    if "tries" in state:
        fails.append("slot was not confirmed after 25 s of UI")
    entries = esp_entries(work)
    print(f"   entries: {', '.join(sorted(entries))}")
    if "zz-live+3-0.conf" not in entries:
        fails.append(f"confirming did not reset the count to +3-0: {sorted(entries)}")
    if len([e for e in entries if e.startswith("zz-live")]) != 1:
        fails.append(f"stale counted entries left behind: {sorted(entries)}")

    # boot 4: the loader has run out of tries and skips the live entry
    print("== boot 4: a bad live entry (+0-3) falls through to the fallback ==")
    esp_rename(work, "loader/entries/zz-live+3-0.conf", "loader/entries/zz-live+0-3.conf")
    ser = start_vm(work)
    if not ser.wait_for(f"boot slot {old_slot}", 180):
        fails.append(f"the fallback slot {old_slot} did not boot")
    if not ser.wait_for("fell back", 60):
        fails.append("init did not notice that it was running the fallback slot")
    ser.close()
    stop_vm(work)

    state = esp_read(work, "state")
    print("   /boot/state after the fallback:\n" +
          "\n".join("     " + l for l in state.strip().splitlines()))
    if f"active={old_slot}" not in state:
        fails.append("the fallback slot was not adopted")
    if f"previous={new_slot}" not in state:
        fails.append("the dropped slot is not remembered as the previous one")
    entries = esp_entries(work)
    print(f"   entries: {', '.join(sorted(entries))}")
    if "zz-live+3-0.conf" not in entries:
        fails.append(f"the adopted slot has no fresh counted entry: {sorted(entries)}")
    else:
        entry = esp_read(work, LIVE)
        if f"/slot-{old_slot}/vmlinuz" not in entry:
            fails.append("the live entry still points at the dropped slot")
    fb = esp_read(work, FALLBACK) if esp_exists(work, FALLBACK) else ""
    if f"/slot-{new_slot}/vmlinuz" not in fb:
        fails.append("the fallback entry does not point back at the dropped slot")
    if esp_read(work, f"ver-{new_slot}").strip() != a.version:
        fails.append("dropped slot lost its version marker")

    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)

    print()
    if fails:
        print("FAIL: " + "; ".join(fails))
        return 1
    print(f"PASS: update into slot {new_slot} ({a.version}) with kernel and "
          f"initramfs verified; the loader counted the attempt in the entry name "
          f"and the watchdog reset it; a bad entry fell through to slot "
          f"{old_slot}, which init adopted and made live again")
    return 0


if __name__ == "__main__":
    sys.exit(main())
