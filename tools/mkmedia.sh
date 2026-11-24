#!/bin/sh
# Turn a clip or a still into the content the Media screen plays.
#
#   ./tools/mkmedia.sh clip.mp4 out/     # PNG frame sequence (frame0000.png ...) + fps
#   ./tools/mkmedia.sh photo.png out/    # single poster.png
#
# Put the result on the state partition as /state/media (mkdisk.sh does it with
# MEDIA_DIR=out) and press 4 / F4 on the unit.
set -e
cd "$(dirname "$0")/.."

IN=${1:-}
OUT=${2:-}
FPS=${FPS:-10}
WIDTH=${WIDTH:-640}

[ -n "$IN" ] && [ -n "$OUT" ] && [ -f "$IN" ] || {
    echo "usage: $0 CLIP_OR_PNG OUTDIR   (FPS=10 WIDTH=640)" >&2
    exit 2
}

IN_ABS=$(cd "$(dirname "$IN")" && pwd)/$(basename "$IN")
mkdir -p "$OUT"
OUT_ABS=$(cd "$OUT" && pwd)

case "$IN" in
*.png|*.PNG)
    cp "$IN_ABS" "$OUT_ABS/poster.png"
    echo "==> $OUT_ABS/poster.png"
    ;;
*)
    docker run --rm -v "$(dirname "$IN_ABS"):/in:ro" -v "$OUT_ABS:/out" \
        -e SRC="/in/$(basename "$IN_ABS")" -e FPS="$FPS" -e WIDTH="$WIDTH" \
        alpine:latest sh -c '
      set -e
      apk add --no-cache ffmpeg >/dev/null 2>&1
      rm -f /out/frame*.png
      ffmpeg -loglevel error -y -i "$SRC" -vf "scale=$WIDTH:-2" -r "$FPS" \
             -start_number 0 /out/frame%04d.png
      echo "$FPS" > /out/fps
      n=$(ls /out/frame*.png | wc -l)
      echo "==> $n frames at $FPS fps (${WIDTH}px wide) in /out"
    '
    docker run --rm -v "$OUT_ABS:/out" alpine:latest chown "$(id -u):$(id -g)" /out/* 2>/dev/null || true
    ;;
esac
