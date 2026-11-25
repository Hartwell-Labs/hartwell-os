# Rescue console.
#
# Sourced by /init when the kernel command line carries hwo.rescue=1, i.e. when
# someone standing at the boot menu picked "Rescue". No UI, no fleet agent, no
# network reachability: a console and the error log.

B=$(printf '\033[1m'); D=$(printf '\033[2m'); A=$(printf '\033[33m')
R=$(printf '\033[31m'); N=$(printf '\033[0m')
export PATH="/:/bin:/sbin:/usr/bin:/usr/sbin"

mine=$(/hwo id 2>/dev/null)
krel=$(uname -r)
if [ -d /state ]; then state="HWO_CFG mounted at /state"
else                   state="RAM only, no HWO_CFG partition"; fi

warns=0; errs=0
if [ -r /run/hwo.err ]; then
    warns=$(awk '$3 == "W"' /run/hwo.err 2>/dev/null | wc -l)
    errs=$(awk '$3 != "W" && NF' /run/hwo.err 2>/dev/null | wc -l)
fi

# Same language as the screen: a sheet of facts separated by rules, one colour
# for what is wrong, plain text for everything else. A person reads this under
# pressure, so it answers who/which/what before it lists commands.
rule="${D}--------------------------------------------------------------${N}"

printf '\n  %sHartwell OS%s  %srescue console%s\n' "$B" "$N" "$D" "$N"
printf '  %s\n\n' "$rule"

printf '  %s%-8s%s %s\n' "$D" "unit"  "$N" "$mine"
printf '  %s%-8s%s %s 1.0  %skernel %s%s\n' "$D" "image" "$N" "hartwell-os" "$D" "$krel" "$N"
printf '  %s%-8s%s %s\n' "$D" "state" "$N" "$state"
if [ "$errs" -gt 0 ] 2>/dev/null; then
    printf '  %s%-8s%s %s%d error(s)%s, %s%d warning(s)%s   %shwo errs%s\n' \
        "$D" "log" "$N" "$R" "$errs" "$N" "$A" "$warns" "$N" "$D" "$N"
else
    printf '  %s%-8s%s %d error(s), %s%d warning(s)%s   %shwo errs%s\n' \
        "$D" "log" "$N" "$errs" "$A" "$warns" "$N" "$D" "$N"
fi

printf '\n  %s\n\n' "$rule"
printf '  %s%-10s%s %s\n' "$N" "hwo errs" "$N" "${D}log, with the fix for every code$N"
printf '  %-10s %s\n' "help"     "${D}this card$N"
printf '  %-10s %s\n' "ls /state" "${D}licence and content, if the partition mounted$N"
printf '  %-10s %s\n\n' "poweroff" "${D}leave rescue mode$N"

# Interactive dressing: a quiet prompt and three helpers, kept in ENV so a
# nested shell is dressed the same way.
mkdir -p /etc
cat > /etc/hwo-rescue.sh <<'RESCUE'
PS1='\[\033[2m\]rescue\[\033[0m\] # '

errs()  { hwo errs "$@"; }
fixes() { hwo errs | sed -n '/Fixes/,$p'; }
unit()  { hwo id; }
help() {
    printf '\n'
    printf '  errs      error log, plus the fix for each code\n'
    printf '  fixes     only the fix lines\n'
    printf '  unit      unit id, the value a licence is bound to\n'
    printf '  help      this card\n\n'
}
RESCUE
export ENV=/etc/hwo-rescue.sh

# cttyhack is not in every busybox build; /dev/console is the same tty the
# kernel prints to, so the shell inherits the console either way.
# Job control needs a controlling terminal. busybox has no cttyhack here, but
# setsid -c adopts /dev/console, which is exactly what cttyhack would do.
if [ -x /bin/cttyhack ]; then
    exec setsid cttyhack sh -i
fi
exec setsid -c sh -i </dev/console >/dev/console 2>&1
