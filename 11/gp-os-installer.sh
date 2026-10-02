#!/bin/sh
set -eu

usage() {
    echo "Usage: $0 [--dry-run] IMAGE [DEVICE]"
    echo
    echo "Install GitrexOS raw image to a BIOS disk or USB device."
    echo "Example: $0 build/gp-os.img /dev/sdb"
}

DRY_RUN=0
if [ "${1:-}" = "--dry-run" ]; then
    DRY_RUN=1
    shift
fi

IMAGE=${1:-}
DEVICE=${2:-}
if [ -z "$IMAGE" ]; then usage; exit 2; fi
if [ ! -f "$IMAGE" ]; then echo "Image not found: $IMAGE" >&2; exit 1; fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY RUN: would install $IMAGE"
    [ -n "$DEVICE" ] && echo "DRY RUN: target $DEVICE"
    exit 0
fi

if [ -z "$DEVICE" ]; then
    echo "Available block devices:"
    lsblk -dpno NAME,SIZE,MODEL 2>/dev/null || true
    printf "Target device (for example /dev/sdb): "
    read -r DEVICE
fi

case "$DEVICE" in
    /dev/*) ;;
    *) echo "Refusing unsafe target: $DEVICE" >&2; exit 1 ;;
esac

echo "WARNING: all data on $DEVICE will be overwritten."
printf "Type INSTALL GITREXOS to continue: "
read -r CONFIRM
[ "$CONFIRM" = "INSTALL GITREXOS" ] || { echo "Cancelled."; exit 1; }

dd if="$IMAGE" of="$DEVICE" bs=4M conv=fsync status=progress
sync
echo "GitrexOS installed. Reboot and select the target disk in BIOS."