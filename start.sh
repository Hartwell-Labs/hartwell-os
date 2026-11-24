#!/bin/bash
# Hartwell OS - Uruchamianie w QEMU przez Dockera
# Usage: ./start.sh [options]
# Options: -u (unlicensed), -l (licensed), -m (media), -s (secure), -h (help)

set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIST_DIR="$PROJECT_DIR/dist"

echo "=== Hartwell OS QEMU Launcher ==="
echo "Projekt: $PROJECT_DIR"
echo ""

# Sprawdź czy Docker działa
if ! command -v docker >/dev/null 2>&1; then
    echo "BŁĄD: Docker nie jest zainstalowany"
    echo "Instalacja: sudo apt-get install docker.io"
    exit 1
fi

# Sprawdź czy obrazy istnieją
if [ ! -f "$DIST_DIR/hartwell-os-1.0.iso" ]; then
    echo "Budowanie projektu..."
    cd "$PROJECT_DIR"
    ./build.sh >/dev/null 2>&1
fi

# Parsowanie argumentów
UNLICENSED=0
LICENSED=0
MEDIA=0
SECURE=0
KEY=""
DEVICE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -u|--unlicensed) UNLICENSED=1; shift ;;
        -l|--licensed) LICENSED=1; shift ;;
        -k|--key)
            KEY="$2"
            DEVICE="$3"
            shift 3
            ;;
        -m|--media) MEDIA=1; shift ;;
        -s|--secure) SECURE=1; shift ;;
        -h|--help)
            echo "Użycie: $0 [opcje]"
            echo "  -u, --unlicensed   Bez licencji (ekran aktywacji)"
            echo "  -l, --licensed     Z licencją wbudowaną"
            echo "  -k, --key KEY      Własny klucz HWO-YYYY-XXXX"
            echo "  -d, --device UUID  UUID urządzenia"
            echo "  -m, --media        Sekwencja PNG (Media screen)"
            echo "  -s, --secure       Secure Boot (OVMF)"
            echo "  -h, --help         Ta pomoc"
            exit 0
            ;;
        *) shift ;;
    esac
done

# Wybór trybu
echo "Uruchamianie QEMU..."

if [ "$SECURE" = "1" ]; then
    echo "Secure Boot (OVMF)..."
    cd "$PROJECT_DIR"
    ./tools/secureboot.sh init >/dev/null 2>&1
    ./tools/secureboot.sh sign build/unsigned/syslinux.efi build/signed/BOOTX64.EFI >/dev/null 2>&1
    ./tools/secureboot.sh vars build/OVMF_VARS.sb.fd >/dev/null 2>&1
    docker run --rm -v "$PROJECT_DIR/dist:/d" debian:bookworm sh -c '
        qemu-system-x86_64 -drive file=/d/hartwell-os-1.0-usb.img -m 512 \
            -bios build/OVMF_CODE_4M.secboot.fd \
            -drive file=build/OVMF_VARS.sb.fd,format=raw \
            -serial file:/dev/null -no-reboot -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f' 2>/dev/null
elif [ "$MEDIA" = "1" ]; then
    echo "Media (PNG sequence)..."
    docker run --rm -v "$PROJECT_DIR/dist:/d" -v "$PROJECT_DIR/tools/mkmedia.sh:/tmp/mkmedia.sh" debian:bookworm sh -c '
        apk add --no-cache qemu-system-x86 python3-pip vncdotool >/dev/null 2>&1
        qemu-system-x86_64 -cdrom /d/hartwell-os-1.0.iso -m 512 -display vnc=0:0 -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f &
        sleep 45
        echo "key 4" | vncctl 127.0.0.1::5900 2>/dev/null || true
        sleep 2
        qemu-system-x86_64 -cdrom /d/hartwell-os-1.0.iso -m 512 -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f' 2>/dev/null || echo "Media test"
else
    if [ "$LICENSED" = "1" ]; then
        if [ -z "$KEY" ]; then
            KEY="HWO-2026-V8NS"
            DEVICE="HWO-3DD5-B123"
        fi
        echo "Z licencją: $KEY / $DEVICE"
        docker run --rm -v "$PROJECT_DIR/dist:/d" debian:bookworm sh -c '
            apk add --no-cache qemu-system-x86 >/dev/null 2>&1
            HWO_KEY='$KEY' HWO_DEVICE='$DEVICE' qemu-system-x86_64 -cdrom /d/hartwell-os-1.0.iso -m 256 -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f' 2>/dev/null
    elif [ "$UNLICENSED" = "1" ]; then
        echo "Bez licencji (ekran aktywacji)..."
        docker run --rm -v "$PROJECT_DIR/dist:/d" debian:bookworm sh -c '
            apk add --no-cache qemu-system-x86 >/dev/null 2>&1
            qemu-system-x86_64 -cdrom /d/hartwell-os-1.0.iso -m 256 -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f' 2>/dev/null
    else
        # Domyślnie: z licencją
        echo "Domyślnie: z licencją..."
        docker run --rm -v "$PROJECT_DIR/dist:/d" debian:bookworm sh -c '
            apk add --no-cache qemu-system-x86 >/dev/null 2>&1
            HWO_KEY=HWO-2026-V8NS HWO_DEVICE=HWO-3DD5-B123 qemu-system-x86_64 -cdrom /d/hartwell-os-1.0.iso -m 256 -uuid 9d2f7c1e-8b4a-4c61-9f2d-3a5b6c7d8e9f' 2>/dev/null
    fi
fi

echo "=== Gotowe ==="