# Rescue console.
#
# Sourced by /init when the kernel command line carries hwo.rescue=1, i.e. when
# someone standing at the boot menu picked "Rescue". No UI, no fleet agent, no
# network reachability: a console and the error log.

G=$(printf '\033[1;32m'); D=$(printf '\033[2m'); A=$(printf '\033[33m')
R=$(printf '\033[31m');   N=$(printf '\033[0m')
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

# A rescue console is read by a person under pressure: one framed block that
# answers who, which image, which state, what the log holds - then the four
# things worth doing.
printf '\n  %s+--------------------------------------------------------------+%s\n' "$G" "$N"
printf '  %s|%s  %sHARTWELL OS%s %s.%s %srescue console%s%*s%s|%s\n' \
    "$G" "$N" "$G" "$N" "$D" "$N" "$G" "$N" 19 "" "$G" "$N"
printf '  %s+--------------------------------------------------------------+%s\n\n' "$G" "$N"

printf '  %s%-8s%s%s\n' "$D" "unit" "$N" "$mine"
printf '  %s%-8s%s%s  %s·%s  %skernel %s%s\n' "$D" "image" "$N" "hartwell-os 1.0" "$D" "$N" "$D" "$krel" "$N"
printf '  %s%-8s%s%s\n' "$D" "state" "$N" "$state"
if [ "$errs" -gt 0 ] 2>/dev/null; then
    printf '  %s%-8s%s%s%d error(s)%s, %d warning(s)   %stype: hwo errs%s\n' \
        "$D" "log" "$N" "$R" "$errs" "$N" "$warns" "$D" "$N"
else
    printf '  %s%-8s%s%d error(s), %s%d warning(s)%s   %stype: hwo errs%s\n' \
        "$D" "log" "$N" "$errs" "$A" "$warns" "$N" "$D" "$N"
fi

printf '\n  %shwo errs%s   log, with the fix for every code\n' "$G" "$N"
printf '  %shelp%s       the short card of commands\n' "$G" "$N"
printf '  %sls /state%s   licence and content, if the partition mounted\n' "$G" "$N"
printf '  %spoweroff%s   leave rescue mode\n\n' "$G" "$N"

# Interactive dressing: a coloured prompt, three helpers, a help card. Kept in
# ENV so a nested shell is dressed the same way.
mkdir -p /etc
cat > /etc/hwo-rescue.sh <<'RESCUE'

PS1='\[\033[1;32m\]rescue\[\033[0m\] \[\033[2m\]\w\[\033[0m\] # '

errs()  { hwo errs "$@"; }
fixes() { hwo errs | sed -n '/Fixes/,$p'; }
unit()  { hwo id; }
help() {
    printf '\n  \033[1;32mRESCUE\033[0m \033[2m- commands\033[0m\n\n'
    printf '  errs     error log, plus the fix for each code\n'
    printf '  fixes    only the fix lines\n'
    printf '  unit     unit id, the value a licence is bound to\n'
    printf '  help     this card\n\n'
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
