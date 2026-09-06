#!/bin/sh
set -e
echo "Building evdi_bridge..."
apt update -y || true
apt install -y git build-essential libdrm-dev python3 pkg-config

# Build custom libevdi from source to support "evdi-lindroid" driver name
if [ ! -f "/usr/local/lib/libevdi.so" ]; then
    echo "Building custom libevdi..."
    rm -rf /tmp/evdi-src
    git clone https://github.com/alghiffaryfa19/evdi.git /tmp/evdi-src
    cd /tmp/evdi-src/library
    make
    cp libevdi.so* /usr/local/lib/
    cp evdi_lib.h /usr/local/include/
    ldconfig
    cd -
fi

export C_INCLUDE_PATH=/usr/local/include
export LIBRARY_PATH=/usr/local/lib
export LD_LIBRARY_PATH=/usr/local/lib

# Deteksi apakah skrip dijalankan dari dalam repo hasil clone (manual debug) atau via debos (Github Actions)
if [ -d "overlay/usr/src/evdi_bridge" ]; then
    echo "Detected manual build from cloned repository..."
    cd overlay/usr/src/evdi_bridge
    gcc evdi_bridge.c -o /usr/bin/evdi_bridge -I/usr/include/libdrm -ldrm -levdi -L/usr/local/lib -I/usr/local/include
    chmod +x /usr/bin/evdi_bridge
    cd ../../../..
    cp overlay/usr/lib/systemd/system/evdi-bridge.service /usr/lib/systemd/system/
elif [ -d "/usr/src/evdi_bridge" ]; then
    echo "Detected debos rootfs build..."
    cd /usr/src/evdi_bridge
    gcc evdi_bridge.c -o /usr/bin/evdi_bridge -I/usr/include/libdrm -ldrm -levdi -L/usr/local/lib -I/usr/local/include
    chmod +x /usr/bin/evdi_bridge
else
    echo "Error: Cannot find evdi_bridge source code!"
    exit 1
fi

echo "evdi_bridge built successfully!"
systemctl enable evdi-bridge || true
systemctl restart evdi-bridge || true
