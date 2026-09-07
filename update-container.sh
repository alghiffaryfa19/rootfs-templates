#!/bin/bash
set -e

echo "Updating container for Lindroid create-disp..."

# Install build dependencies
apt-get update
apt-get install -y cmake g++ pkg-config libdrm-dev libsystemd-dev git

# Clone create-disp source
echo "Cloning create-disp..."
rm -rf /tmp/create-disp
git clone https://github.com/Linux-on-droid/create-disp /tmp/create-disp

cd /tmp/create-disp

# Build create-disp
mkdir -p build
cd build
cmake ..
make -j$(nproc)

# Install binary
cp create-disp /usr/bin/create-disp
chmod +x /usr/bin/create-disp

# Install systemd service
cp ../create-disp.service /etc/systemd/system/create-disp.service
systemctl daemon-reload
systemctl enable create-disp.service

# Remove old Anland evdi_bridge if exists
if [ -f "/etc/systemd/system/evdi_bridge.service" ]; then
    systemctl disable evdi_bridge.service || true
    rm -f /etc/systemd/system/evdi_bridge.service
fi
if [ -f "/etc/systemd/system/display_daemon.service" ]; then
    systemctl disable display_daemon.service || true
    rm -f /etc/systemd/system/display_daemon.service
fi

echo "create-disp successfully built and installed!"
