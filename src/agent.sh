#!/bin/sh
# Hartwell OS fleet agent — pulls content, reports health. No shell, no inbound ports.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

URL="${HWO_URL:-}"
[ -z "$URL" ] && [ -f /etc/hartwell/agent.conf ] && URL=$(sed -n 's/^url=//p' /etc/hartwell/agent.conf 2>/dev/null | head -1)
LOG=/run/agent.log
: > "$LOG"
printf '%s\n' "$URL" > /run/fleet.url 2>/dev/null

log() { echo "$(date -u +%H:%M:%S) $*" >> "$LOG"; }

while :; do
    if ! /hwo license >/dev/null 2>&1; then
        echo "unlicensed" > /run/fleet.status
        sleep 30
        continue
    fi

    if [ -z "$URL" ]; then
        echo "local" > /run/fleet.status
        log "no HWO_URL, local content only"
        sleep 30
        continue
    fi

    if wget -q -T 8 -O /tmp/content.new "$URL/content.txt" 2>>"$LOG" && [ -s /tmp/content.new ]; then
        mv /tmp/content.new /etc/hartwell/content.txt
        [ -d /state ] && cp /etc/hartwell/content.txt /state/content.txt 2>/dev/null
        echo "online" > /run/fleet.status
        log "content updated"
    else
        echo "offline" > /run/fleet.status
        log "content fetch failed, keeping cached copy"
    fi

    wget -q -T 5 -O /dev/null --post-data "device=$(/hwo id)&state=$(cat /run/fleet.status)" \
         "$URL/heartbeat" 2>>"$LOG" || log "heartbeat failed"

    date +%s > /run/fleet.since
    sleep 30
done
