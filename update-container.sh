#!/bin/bash
set -e

echo "Updating container for Lindroid create-disp..."

# Install build dependencies
apt-get update
apt-get install -y cmake g++ pkg-config libdrm-dev libsystemd-dev

# Clone create-disp source
echo "Cloning create-disp..."
rm -rf /tmp/create-disp
git clone https://github.com/alghiffaryfa19/create-disp.git /tmp/create-disp

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
systemctl start create-disp.service

systemctl stop evdi-bridge.service
systemctl disable evdi-bridge.service

echo "create-disp successfully built and installed!"
