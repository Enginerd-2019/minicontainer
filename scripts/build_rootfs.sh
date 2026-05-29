#!/bin/bash
set -euo pipefail

ROOTFS="$(dirname "$0")/../rootfs"

# Clean slate
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/{bin,lib,lib64,proc,tmp,etc,etc/ssl/certs}

# Binaries to include
BINS=(sh ls echo wc mount grep ps rm env cp chmod mknod cat mkdir hostname id ip ipcs ipcmk ipcrm curl python3 nano sleep)

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

# NSS modules for getaddrinfo (DNS via /etc/resolv.conf). These are
# dlopen'd by glibc at runtime, so ldd does not pick them up. Without
# them, curl reports "Could not resolve host" even with a valid
# /etc/resolv.conf. Cover Debian/Ubuntu and RHEL/Fedora layouts.
for so in /lib/x86_64-linux-gnu/libnss_dns.so.2 \
          /lib/x86_64-linux-gnu/libnss_files.so.2 \
          /lib/x86_64-linux-gnu/libresolv.so.2 \
          /usr/lib64/libnss_dns.so.2 \
          /usr/lib64/libnss_files.so.2 \
          /usr/lib64/libresolv.so.2; do
    [ -r "$so" ] && cp "$so" "$ROOTFS/lib/"
done

# CA certificate bundle for HTTPS (curl). First readable path wins.
for ca in /etc/ssl/certs/ca-certificates.crt \
          /etc/pki/tls/certs/ca-bundle.crt; do
    if [ -r "$ca" ]; then
        cp "$ca" "$ROOTFS/etc/ssl/certs/ca-certificates.crt"
        break
    fi
done

# Terminfo for curses-based binaries (nano, less, vi). ncurses opens
# $TERM at /etc/terminfo, /lib/terminfo, or /usr/share/terminfo; ldd
# cannot see these data files, so they need their own copy step. Without
# them, nano dies with "Error opening terminal: xterm." even though TERM
# is inherited from the host. Cover Debian/Ubuntu and RHEL/Fedora layouts.
for tdir in /usr/share/terminfo /lib/terminfo /etc/terminfo; do
    if [ -d "$tdir" ]; then
        mkdir -p "$ROOTFS/usr/share/terminfo/x"
        for entry in xterm xterm-256color xterm-color; do
            [ -r "$tdir/x/$entry" ] && \
                cp "$tdir/x/$entry" "$ROOTFS/usr/share/terminfo/x/"
        done
        break
    fi
done

# Minimal DNS resolver config. Prefer the host's resolv.conf; fall back
# to public resolvers if the host doesn't have one (e.g. systemd-resolved
# stub that points only at 127.0.0.53, which the container cannot reach).
if [ -r /etc/resolv.conf ] && ! grep -q '127\.0\.0\.53' /etc/resolv.conf; then
    cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
else
    printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$ROOTFS/etc/resolv.conf"
fi

echo "rootfs built: $(ls "$ROOTFS/bin" | tr '\n' ' ')"
