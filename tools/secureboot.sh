#!/bin/sh
# Secure Boot for Hartwell OS: keys, signed EFI loaders, firmware enrollment files
# and an OVMF vars image for testing the signed loaders under real Secure Boot.
#
#   ./tools/secureboot.sh init                  # create keys/ (once)
#   ./tools/secureboot.sh sign IN OUT           # sign a loader with the db key
#   ./tools/secureboot.sh verify FILE...        # check a signature against db.crt
#   ./tools/secureboot.sh vars OUT.fd           # OVMF vars with PK/KEK/db enrolled
#
# keys/db.cer is the certificate a customer enrolls in firmware; db.auth, KEK.auth
# and PK.auth are the signed update lists for the firmware's key management page.
set -e
cd "$(dirname "$0")/.."

IMG=hartwell-sbtools:1

ensure_image()
{
    docker image inspect "$IMG" >/dev/null 2>&1 && return 0
    echo "==> building $IMG (once)" >&2
    docker build -t "$IMG" - >&2 <<'DOCKER'
FROM debian:bookworm
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ovmf sbsigntool efitools openssl python3-pip qemu-system-x86 mtools dosfstools \
    && pip3 install --break-system-packages --quiet virt-firmware \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /w
DOCKER
}

sbtools()
{
    ensure_image
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$PWD:/w" -w /w "$IMG" "$@"
}

case "${1:-}" in
init)
    sbtools sh -c '
set -e
mkdir -p keys
cd keys
[ -f db.key ] && exit 0
for n in PK KEK db; do
    case $n in
    PK)  cn="Hartwell OS platform key" ;;
    KEK) cn="Hartwell OS key exchange key" ;;
    db)  cn="Hartwell OS db" ;;
    esac
    openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
        -keyout "$n.key" -out "$n.crt" -subj "/CN=$cn/" \
        -addext "basicConstraints=critical,CA:FALSE" \
        -addext "keyUsage=critical,digitalSignature" >/dev/null 2>&1
    openssl x509 -in "$n.crt" -outform DER -out "$n.cer"
done
g=$(python3 -c "import uuid;print(uuid.uuid4())")
for n in PK KEK db; do cert-to-efi-sig-list -g "$g" "$n.crt" "$n.esl"; done
sign-efi-sig-list -g "$g" -k PK.key  -c PK.crt  PK  PK.esl  PK.auth
sign-efi-sig-list -g "$g" -k PK.key  -c PK.crt  KEK KEK.esl KEK.auth
sign-efi-sig-list -g "$g" -k KEK.key -c KEK.crt db  db.esl  db.auth
chmod 600 ./*.key
'
    echo "==> keys in keys/ (db.cer to enroll, *.auth for firmware key management)"
    ;;
sign)
    [ -n "${3:-}" ] || { echo "usage: $0 sign IN.efi OUT.efi" >&2; exit 2; }
    sbtools sbsign --key keys/db.key --cert keys/db.crt --output "$3" "$2"
    ;;
sign-inplace)
    [ -n "${2:-}" ] || { echo "usage: $0 sign-inplace FILE.efi" >&2; exit 2; }
    sbtools sbsign --key keys/db.key --cert keys/db.crt --output /tmp/signed.efi "$2"
    sbtools sh -c "cp /tmp/signed.efi '$2'"
    ;;
verify)
    shift
    [ -n "${1:-}" ] || { echo "usage: $0 verify FILE..." >&2; exit 2; }
    sbtools sh -c 'rc=0; for f in "$@"; do sbverify --cert keys/db.crt "$f" || rc=1; done; exit $rc' sh "$@"
    ;;
vars)
    out=${2:-build/OVMF_VARS.sb.fd}
    sbtools sh -c '
set -e
mkdir -p "$(dirname "'"$out"'")"
virt-fw-vars -i /usr/share/OVMF/OVMF_VARS_4M.fd -o "'"$out"'" --sb \
    --set-pk 0x11111111-2222-3333-4444-555555555555 keys/PK.cer \
    --add-kek 0x11111111-2222-3333-4444-555555555555 keys/KEK.cer \
    --add-db 0x11111111-2222-3333-4444-555555555555 keys/db.cer
'
    echo "==> $out (Secure Boot on, Hartwell PK/KEK/db enrolled)"
    ;;
*)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac
