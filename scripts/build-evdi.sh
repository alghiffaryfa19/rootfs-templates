#!/bin/sh
set -e
echo "Building evdi_bridge..."
apt install -y git build-essential libdrm-dev
cd overlay/usr/src/evdi_bridge
gcc evdi_bridge.c -o /usr/bin/evdi_bridge -ldrm
chmod +x /usr/bin/evdi_bridge
echo "evdi_bridge built successfully!"
cp overlay/usr/lib/systemd/system/evdi-bridge.service /usr/lib/systemd/system/
systemctl enable evdi-bridge
