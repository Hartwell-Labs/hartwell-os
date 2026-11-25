#!/bin/sh
# Hartwell OS 1.0 — PID 1. Mounts, supervises the appliance, never gives a shell.
export PATH=/bin:/sbin:/usr/sbin:/usr/bin

/bin/busybox --install -s /bin 2>/dev/null

mount -t proc     proc  /proc  2>/dev/null
mount -t sysfs    sysfs /sys   2>/dev/null
mount -t devtmpfs dev   /dev   2>/dev/null
mount -t tmpfs    tmpfs /run   2>/dev/null
mount -t tmpfs    tmpfs /tmp   2>/dev/null
mkdir -p /dev/pts /run /tmp
mount -t devpts devpts /dev/pts 2>/dev/null

# graphics: DRM modules for the common x86 display adapters
for m in $(cat /etc/hartwell/modules.load 2>/dev/null); do
    insmod "/lib/modules/$(ls /lib/modules | head -1)/$m" 2>>/run/modprobe.log
done
if grep -q 'hwo.debug=1' /proc/cmdline 2>/dev/null; then
    echo "--MODPROBE--"; cat /run/modprobe.log 2>/dev/null; echo "--END PROBE--"
    /hwo fbprobe
fi
if [ -e /dev/fb0 ]; then
    echo "hartwell-os: framebuffer /dev/fb0 $(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null) $(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null)bpp"
else
    echo "hartwell-os: no framebuffer, falling back to text console"
    /hwo err F1001 no /dev/fb0 after modprobe
    sed "s/^/hartwell-os: modprobe: /" /run/modprobe.log 2>/dev/null | head -12
fi

# persistent state: any disk or partition labelled HWO_CFG is mounted at /state.
# A licence typed on the unit and content pushed by the fleet agent live there and
# survive reboot; without it the unit runs entirely from RAM.
mkdir -p /state
: > /run/state.log
state=""
for d in /dev/vda /dev/vda[0-9]* /dev/vdb /dev/vdb[0-9]* \
         /dev/sda /dev/sda[0-9]* /dev/sdb /dev/sdb[0-9]* \
         /dev/nvme0n1 /dev/nvme0n1p[0-9]* /dev/mmcblk0 /dev/mmcblk0p[0-9]*; do
    [ -b "$d" ] || continue
    # busybox blkid's -t is not a filter: it succeeds for any filesystem and just
    # limits the *output*. Parse the line and match the label ourselves, or the
    # boot partition (HWBOOT) gets mounted as /state.
    info=$(blkid "$d" 2>/dev/null)
    case "$info" in
    *'LABEL="HWO_CFG"'*) ;;
    *)  echo "probe $d no HWO_CFG: ${info:-no filesystem}" >> /run/state.log
        continue ;;
    esac
    echo "probe $d is HWO_CFG" >> /run/state.log
    if mount -t vfat -o rw,iocharset=utf8,umask=0022 "$d" /state 2>>/run/state.log; then
        state="$d"
        break
    fi
    echo "mount $d failed" >> /run/state.log
    /hwo err E4002 cannot mount $d read-write
done
if [ -n "$state" ]; then
    echo "hartwell-os: state partition $state at /state"
    [ -f /state/license ]     && cp /state/license     /etc/hartwell/license
    [ -f /state/content.txt ] && cp /state/content.txt /etc/hartwell/content.txt
else
    echo "hartwell-os: no HWO_CFG partition, licence and content live in RAM"
    /hwo err W4001 no HWO_CFG partition
fi

# boot partition: the update channel writes the next initramfs here. It is the
# FAT partition labelled HWBOOT that the unit boots from; a live ISO has none,
# and hwo update then says so (E9003) instead of pretending to update.
mkdir -p /boot
for d in /dev/vda1 /dev/sda1 /dev/nvme0n1p1 /dev/mmcblk0p1; do
    [ -b "$d" ] || continue
    case "$(blkid "$d" 2>/dev/null)" in
    *'LABEL="HWBOOT"'*) mount -t vfat -o rw,iocharset=utf8,umask=0022 "$d" /boot 2>/dev/null && break ;;
    esac
done

# networking: outbound only. Bring up every NIC, take a DHCP lease, and set the
# fleet endpoint so /agent.sh can actually reach it. No inbound listeners.
cat > /etc/resolv.conf <<'RESOLV'
nameserver 1.1.1.1
nameserver 8.8.8.8
RESOLV

# endpoint: /etc/hartwell/agent.conf (url=...) or kernel cmdline hwo.url=...
url=""
[ -f /etc/hartwell/agent.conf ] && url=$(sed -n 's/^url=//p' /etc/hartwell/agent.conf 2>/dev/null | head -1)
for a in $(cat /proc/cmdline 2>/dev/null); do case "$a" in hwo.url=*) url="${a#hwo.url=}" ;; esac; done
export HWO_URL="$url"
printf '%s\n' "$url" > /run/fleet.url 2>/dev/null

cat > /etc/hartwell/udhcpc.script <<'DHCP'
#!/bin/sh
case "$1" in bound|renew) ;;
    *) exit 0 ;;
esac
ip addr flush dev "$interface" 2>/dev/null
ip addr add "$ip/$mask" dev "$interface" 2>/dev/null
[ -n "$router" ] && ip route add default via "$router" dev "$interface" 2>/dev/null
: > /etc/resolv.conf
for d in $dns; do echo "nameserver $d" >> /etc/resolv.conf; done
DHCP
chmod 0755 /etc/hartwell/udhcpc.script

net_up=0
for ifc in $(ls /sys/class/net 2>/dev/null); do
    [ "$ifc" = lo ] && continue
    ip link set "$ifc" up 2>/dev/null
    if udhcpc -i "$ifc" -s /etc/hartwell/udhcpc.script -q -n -t 4 -T 2 >/dev/null 2>&1; then
        net_up=1
        echo "hartwell-os: network $ifc $(ip -4 addr show "$ifc" 2>/dev/null | sed -n 's,.*inet \([0-9.]*/[0-9]*\).*,\1,p' | head -1)"
        break
    fi
done
[ "$net_up" = 0 ] && echo "hartwell-os: no network lease (agent will run local-only)"

echo hartwell-os > /proc/sys/kernel/hostname 2>/dev/null
echo 0 > /proc/sys/kernel/sysrq 2>/dev/null
hwclock -s 2>/dev/null
year=$(date +%Y 2>/dev/null || echo 1970)
[ "$year" -lt 2020 ] 2>/dev/null && /hwo err W7001 clock reads $year

echo "hartwell-os: cmdline $(cat /proc/cmdline 2>/dev/null)"
echo "hartwell-os: boot $(/hwo id) $(/hwo license >/dev/null 2>&1 && echo licensed || echo unlicensed)"
echo "hartwell-os: init at $(cut -d' ' -f1 /proc/uptime)s after kernel start"

# rescue mode: a physical flag on the kernel command line, and nothing else.
# The shipped unit has no shell; this is the only door, and it needs the boot
# menu, i.e. someone standing at the machine.
if grep -q 'hwo.rescue=1' /proc/cmdline 2>/dev/null; then
    . /rescue.sh
fi

if [ -x /agent.sh ] && /hwo license >/dev/null 2>&1; then
    /agent.sh &
    echo "hartwell-os: fleet agent started"
fi

# proof mode: dump the real console buffer to the serial port, then keep running
if grep -q 'hwo.proof=1' /proc/cmdline 2>/dev/null; then
    ( sleep 12; /hwo vcsa /run/screen.txt && { echo "--CONSOLE--"; cat /run/screen.txt; echo "--END--"; } ) &
fi

# supervise the appliance; five crashes and the box reboots itself
crashes=0
while :; do
    /hwo ui
    crashes=$((crashes + 1))
    echo "hartwell-os: ui exited ($crashes)"
    /hwo err E1003 ui exited attempt $crashes
    if [ "$crashes" -ge 5 ]; then
        echo "hartwell-os: crash loop, rebooting"
        /hwo err F1004 five UI exits, rebooting
        sync
        reboot -f
        sleep 5
        crashes=0
    fi
    sleep 1
done
