#!/bin/sh
# Create a FAT32 filesystem image from iso/rootfs/ contents.
OUT="$1"
SRC="$2"
dd if=/dev/zero of="$OUT" bs=1K count=4096 2>/dev/null
mkfs.fat -F 32 "$OUT" 2>/dev/null
mcopy -i "$OUT" "$SRC"/* ::/ 2>/dev/null || true
