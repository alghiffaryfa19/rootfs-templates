#!/bin/sh
set -e
echo "Building evdi_bridge..."
apt update -y || true
apt install -y git build-essential libdrm-dev libevdi-dev

# Deteksi apakah skrip dijalankan dari dalam repo hasil clone (manual debug) atau via debos (Github Actions)
if [ -d "overlay/usr/src/evdi_bridge" ]; then
    echo "Detected manual build from cloned repository..."
    cd overlay/usr/src/evdi_bridge
    gcc evdi_bridge.c -o /usr/bin/evdi_bridge -ldrm -levdi
    chmod +x /usr/bin/evdi_bridge
    cd ../../../..
    cp overlay/usr/lib/systemd/system/evdi-bridge.service /usr/lib/systemd/system/
elif [ -d "/usr/src/evdi_bridge" ]; then
    echo "Detected debos rootfs build..."
    cd /usr/src/evdi_bridge
    gcc evdi_bridge.c -o /usr/bin/evdi_bridge -ldrm -levdi
    chmod +x /usr/bin/evdi_bridge
else
    echo "Error: Cannot find evdi_bridge source code!"
    exit 1
fi

echo "evdi_bridge built successfully!"
systemctl enable evdi-bridge || true
systemctl restart evdi-bridge || true
