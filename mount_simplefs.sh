#!/bin/bash

set -e

MODULE="simplefs"
MOUNTPOINT="/mnt/simplefs"
IMG="$HOME/Projects/linux-leti/disk.img"
PROJECT="$HOME/Projects/linux-leti"

echo "[*] cleanup old state"

sudo umount "$MOUNTPOINT" 2>/dev/null || true
sudo rmmod "$MODULE" 2>/dev/null || true

LOOPDEV=$(losetup -j "$IMG" | cut -d: -f1)

if [ -n "$LOOPDEV" ]; then
    sudo losetup -d "$LOOPDEV" || true
fi

echo "[*] rebuilding module"

cd "$PROJECT"

make clean
make

echo "[*] recreating disk image"

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=20 status=none

echo "[*] creating loop device"

LOOPDEV=$(sudo losetup -fP --show "$IMG")

echo "[*] loop device: $LOOPDEV"

echo "[*] loading module"

sudo insmod simplefs.ko

echo "[*] creating mountpoint"

sudo mkdir -p "$MOUNTPOINT"

echo "[*] mounting filesystem"

sudo mount -t simplefs "$LOOPDEV" "$MOUNTPOINT"

echo
echo "[+] mounted successfully"
echo "[+] loop device: $LOOPDEV"
echo "[+] mountpoint : $MOUNTPOINT"
echo

mount | grep simplefs || true
