#!/bin/bash
set -e

cd "$(dirname "$0")/.."

echo "=== Phase 8a integration test ==="

# Start container
CID=$(sudo ./minicontainer start --pid --rootfs ./rootfs \
        --hostname insp-test --memory 100M --pids 20 \
        /bin/sh -c 'while true; do sleep 60; done')
echo "Container: $CID"
sleep 1

# Each subcommand should succeed
echo ""; echo "--- inspect ---"
sudo ./minicontainer inspect "$CID"

echo ""; echo "--- stats (single shot) ---"
sudo ./minicontainer stats "$CID"

echo ""; echo "--- top ---"
sudo ./minicontainer top "$CID"

echo ""; echo "--- netstat -v ---"
sudo ./minicontainer netstat -v "$CID"

# Cleanup
sudo ./minicontainer stop "$CID"
echo ""; echo "=== test_inspector.sh PASS ==="
