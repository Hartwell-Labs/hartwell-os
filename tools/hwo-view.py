#!/usr/bin/env python3
"""hwo-view - watch and drive a Hartwell OS machine from a browser.

The image runs on hardware with no X, no Wayland and often no VNC; this tool
turns a QEMU framebuffer into a page you can click on. It is built for
operation, not for a demo:

  * capture is change-driven - an idle screen costs no encode and no traffic
  * one JPEG per change, quality 70 by default (~40 KB for a UI screen)
  * every connection is served the newest frame; slow clients drop frames
    instead of building a queue, so latency does not grow over time
  * keyboard and mouse go straight to QMP (input-send-event), no batching
  * each target can be an ISO, a kernel+initramfs, or an already running VM
    attached by QMP socket - so the same tool works for a lab box and a VM

Usage:
  tools/hwo-view.py                                   # dist ISO, port 8899
  tools/hwo-view.py --target rescue=kernel:dist/vmlinuz,dist/initrd,hwo.rescue=1
  tools/hwo-view.py --attach live=/run/qemu-vm.qmp --port 8900
  tools/hwo-view.py --target os=iso:dist/hartwell-os-1.0.iso --quality 60 --fps 30
"""
import argparse
import io
import re
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

try:
    from PIL import Image
except ImportError:  # pragma: no cover - the only hard dependency
    sys.exit("hwo-view: needs Pillow (pip install pillow)")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PAGE = """<!doctype html><meta charset=utf-8><title>Hartwell OS - live view</title>
<style>
:root{--bg:#0b0f14;--panel:#101720;--line:#1d2733;--dim:#7f8fa3;--text:#e8eef5;--ok:#3ddc91}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:#c9d4e0;font:13px/1.45 ui-monospace,SFMono-Regular,Menlo,monospace}
header{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:11px 18px;
 border-bottom:1px solid var(--line);letter-spacing:.12em;text-transform:uppercase;font-size:11px;color:var(--dim)}
header b{color:var(--text);letter-spacing:.14em}
header .hint{letter-spacing:0;text-transform:none;font-size:12px;color:#5f7183}
main{display:grid;grid-template-columns:repeat(var(--cols,1),minmax(0,1fr));gap:16px;padding:16px}
figure{margin:0;min-width:0}
.screen{position:relative;border:1px solid var(--line);border-radius:8px;overflow:hidden;background:#000;
 aspect-ratio:16/10;cursor:crosshair}
.screen img{width:100%;height:100%;object-fit:contain;display:block;image-rendering:auto}
.screen:focus{outline:2px solid var(--ok);outline-offset:-2px}
.badge{position:absolute;top:10px;right:10px;font-size:11px;padding:3px 8px;border-radius:99px;
 background:#101d29;color:var(--dim);border:1px solid var(--line);pointer-events:none}
figure:focus-within .badge{background:#0f2b21;color:var(--ok);border-color:#1d5f45}
figcaption{display:flex;justify-content:space-between;gap:10px;padding-top:7px;color:var(--dim)}
figcaption b{color:var(--text);font-weight:600;letter-spacing:.04em}
.lat{color:#4b5c6e}
</style>
<header><span><b>Hartwell OS</b> &nbsp;live view</span>
 <span class=hint>click a screen to control it &middot; keyboard and mouse are forwarded</span></header>
<main id=grid>__PANES__</main>
<script>
const KEY={KeyA:"a",KeyB:"b",KeyC:"c",KeyD:"d",KeyE:"e",KeyF:"f",KeyG:"g",KeyH:"h",KeyI:"i",
KeyJ:"j",KeyK:"k",KeyL:"l",KeyM:"m",KeyN:"n",KeyO:"o",KeyP:"p",KeyQ:"q",KeyR:"r",KeyS:"s",KeyT:"t",
KeyU:"u",KeyV:"v",KeyW:"w",KeyX:"x",KeyY:"y",KeyZ:"z",Digit0:"0",Digit1:"1",Digit2:"2",Digit3:"3",
Digit4:"4",Digit5:"5",Digit6:"6",Digit7:"7",Digit8:"8",Digit9:"9",Enter:"ret",Space:"spc",
Backspace:"backspace",Tab:"tab",Escape:"esc",Minus:"minus",Equal:"equal",Comma:"comma",Period:"dot",
Slash:"slash",Semicolon:"semicolon",Apostrophe:"apostrophe",BracketLeft:"bracket_left",
BracketRight:"bracket_right",Backslash:"backslash",ArrowUp:"up",ArrowDown:"down",ArrowLeft:"left",
ArrowRight:"right",F1:"f1",F2:"f2",F3:"f3",F4:"f4",Home:"home",End:"end",Delete:"delete"};
function send(path,body){fetch(path,{method:"POST",body:JSON.stringify(body),keepalive:true}).catch(()=>{});}

document.querySelectorAll(".screen").forEach(el=>{
  const vm=el.dataset.vm, badge=el.querySelector(".badge");
  el.addEventListener("focus",()=>badge.textContent="controlling");
  el.addEventListener("blur",()=>badge.textContent="click to control");
  el.addEventListener("keydown",e=>{const q=KEY[e.code]; if(!q) return;
    e.preventDefault(); send("/key",{vm,key:q,down:true});});
  el.addEventListener("keyup",e=>{const q=KEY[e.code]; if(!q) return;
    e.preventDefault(); send("/key",{vm,key:q,down:false});});
  const at=e=>{const r=el.getBoundingClientRect();
    const iw=el.querySelector("img");
    const nw=iw.naturalWidth||1280, nh=iw.naturalHeight||800;
    const s=Math.min(r.width/nw,r.height/nh);
    const dw=nw*s, dh=nh*s;
    const ox=r.left+(r.width-dw)/2, oy=r.top+(r.height-dh)/2;
    return {x:Math.max(0,Math.min(1,(e.clientX-ox)/dw)),
            y:Math.max(0,Math.min(1,(e.clientY-oy)/dh))};};
  let pending=null, raf=0;
  el.addEventListener("mousemove",e=>{pending=at(e);
    if(!raf) raf=requestAnimationFrame(()=>{raf=0; if(pending) send("/mouse",{vm,...pending});});});
  el.addEventListener("mousedown",e=>{e.preventDefault(); el.focus();
    send("/mouse",{vm,...at(e),button:"left",down:true});});
  el.addEventListener("mouseup",e=>send("/mouse",{vm,...at(e),button:"left",down:false}));
  el.addEventListener("contextmenu",e=>e.preventDefault());
});
</script>
"""

BOUNDARY = b"hwoview"
DEBUG = bool(os.environ.get("HWOVIEW_DEBUG"))


def log(*a):
    print("hwo-view:", *a, file=sys.stderr, flush=True)


class Machine:
    """One QEMU instance: capture its screen, forward input to it."""

    def __init__(self, name, spec, opts):
        self.name = name
        self.spec = spec
        self.opts = opts
        self.frame = None            # newest JPEG bytes
        self.frame_seq = 0
        self.serial_path = os.path.join(opts.serial_dir, f"hwo-view-{name}.log")
        self.proc = None
        self.qmp_file = None
        self.qmp_lock = threading.Lock()          # capture side
        self.input_lock = threading.Lock()        # input side, never behind a screendump
        self.qmp_in_file = None
        self.qmp_sock_path = os.path.join(opts.serial_dir, f"hwo-view-{name}.qmp")
        self.shot_dir = "/dev/shm" if os.path.isdir("/dev/shm") else opts.serial_dir
        self.shot = os.path.join(self.shot_dir, f"hwo-view-{name}.img")
        self.fmt = "ppm"
        self.dims = None
        self.header = 0

    # -- process ---------------------------------------------------------
    def start(self):
        if self.spec["kind"] == "attach":
            self.qmp_sock_path = self.spec["path"]
            self._connect_qmp()
            return

        cmd = ["qemu-system-x86_64", "-m", self.opts.memory,
               "-enable-kvm", "-vga", "std", "-display", "none",
               "-usb", "-device", "usb-tablet",
               "-qmp", f"unix:{self.qmp_sock_path},server,nowait",
               "-serial", f"file:{self.serial_path}"]
        if self.spec["kind"] == "iso":
            cmd += ["-cdrom", self.spec["iso"]]
        else:
            cmd += ["-kernel", self.spec["kernel"], "-initrd", self.spec["initrd"]]
            if self.spec.get("append"):
                cmd += ["-append", self.spec["append"]]
        if self.spec.get("uuid"):
            cmd += ["-uuid", self.spec["uuid"]]
        self.out_path = os.path.join(self.opts.serial_dir, f"hwo-view-{self.name}.out")
        log(f"{self.name}: starting qemu")
        self.proc = subprocess.Popen(cmd, cwd=ROOT,
                                     stdout=open(self.out_path, "wb"),
                                     stderr=subprocess.STDOUT)
        self._connect_qmp()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def _connect_qmp(self):
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(self.qmp_sock_path)
                f = s.makefile("rwb")
                f.readline()
                self._qmp_send(f, {"execute": "qmp_capabilities"})
                self.qmp_file = f
                return
            except OSError:
                if self.proc and self.proc.poll() is not None:
                    tail = ""
                    try:
                        tail = open(self.out_path).read()[-400:]
                    except OSError:
                        pass
                    raise RuntimeError(f"{self.name}: qemu exited: {tail.strip()}")
                time.sleep(0.2)
        raise RuntimeError(f"{self.name}: no QMP on {self.qmp_sock_path}")

    def _qmp_send(self, f, obj):
        f.write(json.dumps(obj).encode() + b"\n")
        f.flush()
        return json.loads(f.readline())

    def qmp(self, obj):
        with self.qmp_lock:
            if not self.qmp_file:
                return None
            try:
                return self._qmp_send(self.qmp_file, obj)
            except Exception:
                return None

    # -- input -----------------------------------------------------------
    def qmp_in(self, obj):
        # QMP serves a single client, so input shares the capture socket. A
        # screendump is a few milliseconds, which keeps the queue short.
        return self.qmp(obj)

    def key(self, qcode, down):
        return self.qmp_in({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"down": bool(down),
                                     "key": {"type": "qcode", "data": qcode}}}]}})

    def pointer(self, x, y, button=None, down=None):
        events = [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767)}},
                  {"type": "abs", "data": {"axis": "y", "value": int(y * 32767)}}]
        if button:
            events.append({"type": "btn", "data": {"down": bool(down), "button": button}})
        return self.qmp_in({"execute": "input-send-event", "arguments": {"events": events}})

    # -- capture ---------------------------------------------------------
    def _encode(self, raw):
        """PPM -> JPEG without paying for a plugin decode.

        A 1280x800 PPM is 3 MB and the screendump is ~9 ms; the naive
        Image.open() round trip costs six times more than this path.
        """
        try:
            # the mode changes during boot (text console -> 1280x800), so the
            # header is read every frame: a regex over 15 bytes costs nothing
            m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", raw)
            if not m:
                return None
            w, h = int(m.group(1)), int(m.group(2))
            body = raw[m.end():m.end() + w * h * 3]
            if len(body) < w * h * 3:
                return None
            img = Image.frombytes("RGB", (w, h), body)
            buf = io.BytesIO()
            img.save(buf, "JPEG", quality=self.opts.quality)
            return buf.getvalue()
        except Exception as exc:
            log(f"{self.name}: encode failed: {exc}")
            return None

    def run(self):
        period = 1.0 / self.opts.fps
        last_sig = None
        failures = 0
        while True:
            t0 = time.time()
            if self.proc and self.proc.poll() is not None:
                log(f"{self.name}: qemu exited, capture stopped")
                return
            shot = self.shot
            try:
                res = self.qmp({"execute": "screendump",
                                "arguments": {"filename": shot}})
                if res and "error" in res:
                    raise RuntimeError(res["error"].get("desc", "screendump failed"))
                raw = open(shot, "rb").read()
                failures = 0
            except Exception:
                failures += 1
                if failures == 50:
                    log(f"{self.name}: capture keeps failing")
                time.sleep(0.1)
                continue

            dump_ms = (time.perf_counter() - t0) * 1000
            sig = hash(raw)
            if sig != last_sig:
                last_sig = sig
                te = time.perf_counter()
                payload = self._encode(raw)
                enc_ms = (time.perf_counter() - te) * 1000
                if payload:
                    self.frame = payload
                    self.frame_seq += 1
                    if DEBUG:
                        log(f"{self.name}: frame {self.frame_seq} {len(payload)} B "
                            f"enc {enc_ms:.1f} t={time.time():.3f}")
                # a changed screen means the machine is busy: poll again at once
                continue
            time.sleep(max(0.002, period - (time.time() - t0)))


QCODE = {"-": "minus", " ": "spc", ".": "dot", "\n": "ret"}


def type_text(m, text, delay=0.04):
    """Type on the machine's keyboard, for unlocking a demo unit."""
    for ch in text:
        if ch in QCODE:
            m.key(QCODE[ch], True); m.key(QCODE[ch], False)
        elif ch.isupper():
            m.key("shift", True); m.key(ch.lower(), True)
            m.key(ch.lower(), False); m.key("shift", False)
        else:
            m.key(ch.lower(), True); m.key(ch.lower(), False)
        time.sleep(delay)


def parse_target(name, value, opts):
    if value.startswith("iso:"):
        return {"kind": "iso", "iso": os.path.abspath(value[4:])}
    if value.startswith("kernel:"):
        rest = value[len("kernel:"):]
        parts = rest.split(",")
        if len(parts) < 2:
            raise SystemExit(f"hwo-view: kernel target needs kernel,initrd[,append]: {value}")
        return {"kind": "kernel", "kernel": os.path.abspath(parts[0]),
                "initrd": os.path.abspath(parts[1]),
                "append": ",".join(parts[2:]) or None}
    if value.startswith("attach:"):
        return {"kind": "attach", "path": value[len("attach:"):]}
    if value.startswith("uuid="):
        # iso:path with a bound uuid: "uuid=....:iso:path"
        uuid, _, sub = value.partition(":")
        spec = parse_target(name, sub, opts)
        spec["uuid"] = uuid[len("uuid="):]
        return spec
    raise SystemExit(f"hwo-view: unknown target spec for {name}: {value}")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    machines = {}

    def log_message(self, *a):
        pass

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
            msg = json.loads(self.rfile.read(n) or b"{}")
        except Exception:
            return self._json(400, {"ok": False})
        m = self.machines.get(msg.get("vm"))
        if not m:
            return self._json(404, {"ok": False})
        if self.path.startswith("/key"):
            q = msg.get("key")
            if not q:
                return self._json(400, {"ok": False})
            t = time.perf_counter()
            m.key(q, msg.get("down"))
            if DEBUG:
                log(f"{m.name}: key {q} up={msg.get('down')} in {(time.perf_counter()-t)*1000:.1f} ms t={time.time():.3f}")
        elif self.path.startswith("/mouse"):
            m.pointer(msg.get("x", 0), msg.get("y", 0), msg.get("button"), msg.get("down"))
        else:
            return self._json(404, {"ok": False})
        return self._json(200, {"ok": True})

    def do_GET(self):
        u = urlparse(self.path)
        if u.path.endswith(".mjpg"):
            name = u.path.lstrip("/").split(".")[0]
            if name in self.machines:
                return self._stream(self.machines[name])
            return self._json(404, {"ok": False})
        if u.path == "/serial":
            name = (parse_qs(u.query).get("vm") or [""])[0]
            m = self.machines.get(name)
            if not m:
                return self._json(404, {"ok": False})
            try:
                body = open(m.serial_path, "rb").read()[-16000:]
            except OSError:
                body = b"(no serial output)"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if u.path == "/frame":
            name = (parse_qs(u.query).get("vm") or [""])[0]
            m = self.machines.get(name)
            if not m or m.frame is None:
                return self._json(404, {"ok": False})
            data = m.frame
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)
            return
        if u.path == "/health":
            return self._json(200, {"ok": True, "machines": {
                n: {"frames": m.frame_seq, "bytes": len(m.frame or b"")}
                for n, m in self.machines.items()}})
        body = PAGE.replace("__PANES__", "".join(
            f'<figure><div class="screen" tabindex="0" data-vm="{n}">'
            f'<img src="/{n}.mjpg"><span class="badge">click to control</span></div>'
            f'<figcaption><span><b>{n}</b></span><span class="lat" id="lat-{n}"></span>'
            f'</figcaption></figure>' for n in self.machines)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _stream(self, m):
        self.send_response(200)
        self.send_header("Content-Type",
                         "multipart/x-mixed-replace; boundary=" + BOUNDARY.decode())
        self.send_header("X-Frame-Type", getattr(m, "fmt", "jpeg"))
        self.send_header("Cache-Control", "no-store, no-transform")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sent_seq = -1
        try:
            # a viewer that connects to an idle screen must still see it: send
            # the current frame first, then only changes
            while True:
                if m.frame is not None and m.frame_seq != sent_seq:
                    data = m.frame
                    sent_seq = m.frame_seq
                    self.wfile.write(b"--" + BOUNDARY + b"\r\n")
                    ctype = b"image/png" if m.fmt == "png" else b"image/jpeg"
                    self.wfile.write(b"Content-Type: " + ctype + b"\r\n")
                    self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(data))
                    self.wfile.write(data)
                    self.wfile.write(b"\r\n")
                    self.wfile.flush()
                else:
                    time.sleep(0.004)
        except (BrokenPipeError, ConnectionResetError, OSError):
            return


def main():
    ap = argparse.ArgumentParser(description="Live view of a Hartwell OS machine")
    ap.add_argument("--target", action="append", default=[], metavar="NAME=SPEC",
                    help="iso:PATH | kernel:VMLINUX,INITRD[,APPEND] | attach:QMP_SOCKET")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--fps", type=float, default=60.0,
                    help="capture rate cap (frames are only sent when the screen changes)")
    ap.add_argument("--quality", type=int, default=65, help="JPEG quality, 55-85")
    ap.add_argument("--memory", default="512")
    ap.add_argument("--serial-dir", default="/tmp")
    ap.add_argument("--no-input", action="store_true", help="view only")
    ap.add_argument("--unlock", metavar="KEY",
                    help="type this licence key on the target after boot (demo units)")
    ap.add_argument("--unlock-delay", type=float, default=20.0)
    opts = ap.parse_args()

    if not opts.target:
        iso = os.path.join(ROOT, "dist", "hartwell-os-1.0.iso")
        opts.target = [f"os=iso:{iso}"]

    Handler.machines = {}
    for t in opts.target:
        name, _, spec = t.partition("=")
        if not spec:
            raise SystemExit(f"hwo-view: --target wants NAME=SPEC, got {t!r}")
        Handler.machines[name] = Machine(name, parse_target(name, spec, opts), opts)

    for m in Handler.machines.values():
        try:
            m.start()
        except Exception as exc:
            log(exc)
            for other in Handler.machines.values():
                other.stop()
            raise SystemExit(1)
        threading.Thread(target=m.run, daemon=True).start()

    if opts.unlock and "normal" in Handler.machines:
        def unlock():
            time.sleep(opts.unlock_delay)
            log(f"typing licence key on normal")
            type_text(Handler.machines["normal"], opts.unlock + "\n")
        threading.Thread(target=unlock, daemon=True).start()

    if opts.no_input:
        Handler.do_POST = lambda self: self._json(403, {"ok": False})

    srv = ThreadingHTTPServer((opts.bind, opts.port), Handler)
    srv.daemon_threads = True
    log(f"http://{opts.bind}:{opts.port}/  targets: {', '.join(Handler.machines)}")

    def shutdown(*_):
        log("stopping")
        for m in Handler.machines.values():
            m.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    try:
        srv.serve_forever()
    finally:
        for m in Handler.machines.values():
            m.stop()


if __name__ == "__main__":
    main()
