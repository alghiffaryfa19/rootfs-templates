#!/bin/sh
set -e
echo "Building evdi_bridge..."
cd /usr/src/evdi_bridge
gcc evdi_bridge.c -o /usr/bin/evdi_bridge -ldrm
chmod +x /usr/bin/evdi_bridge
echo "evdi_bridge built successfully!"
