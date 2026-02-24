#!/bin/bash
set -euo pipefail

ROOTFS="$(dirname "$0")/../rootfs"

# Clean slate
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/{bin,lib,lib64,proc,tmp,etc}

# Binaries to include
BINS=(sh ls echo wc mount grep ps rm)

for bin in "${BINS[@]}"; do
    cp "/bin/$bin" "$ROOTFS/bin/"
done

# Copy shared libraries
for bin in "${BINS[@]}"; do
    ldd "/bin/$bin" 2>/dev/null | grep "=> /" | awk '{print $3}' | \
        xargs -I '{}' cp --update=none '{}' "$ROOTFS/lib/"
done

# Dynamic linker
cp /lib64/ld-linux-x86-64.so.2 "$ROOTFS/lib64/"

echo "rootfs built: $(ls "$ROOTFS/bin" | tr '\n' ' ')"
