#!/bin/sh
# Boot the kernel in QEMU, drive the shell over a monitor socket, dump screens.
set -e
apk add --no-cache qemu-system-i386 socat >/dev/null 2>&1

rm -f serial.log boot.ppm about.ppm licensed.ppm mon.sock

qemu-system-i386 \
  -kernel /w/hartwell-os.bin \
  -m 32 -vga std -display none \
  -serial file:/w/serial.log \
  -monitor unix:/w/mon.sock,server,nowait \
  -no-reboot 2>/w/qemu.err &
QPID=$!

# wait for the socket
i=0
while [ ! -S /w/mon.sock ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i+1)); done
sleep 2

mon() { printf '%s\n' "$1" | socat - UNIX-CONNECT:/w/mon.sock >/dev/null; }

mon "screendump /w/boot.ppm"

for k in a b o u t; do mon "sendkey $k"; sleep 0.15; done
mon "sendkey ret"
sleep 1
mon "screendump /w/about.ppm"

for k in l i c e n s e; do mon "sendkey $k"; sleep 0.12; done
mon "sendkey spc"
for k in h w o; do mon "sendkey shift-$k"; sleep 0.12; done
mon "sendkey minus"
for k in 2 0 2 6; do mon "sendkey $k"; sleep 0.12; done
mon "sendkey minus"
for k in 0 0 0 4; do mon "sendkey $k"; sleep 0.12; done
sleep 0.4
mon "sendkey ret"
sleep 1
mon "screendump /w/licensed.ppm"
sleep 0.5

mon "quit"
wait $QPID 2>/dev/null || true

echo "--- serial ---"
cat serial.log 2>/dev/null
ls -l *.ppm 2>/dev/null
