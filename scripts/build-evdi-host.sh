#!/bin/bash
set -e

# Script ini dijalankan dari luar container (Host / GitHub Actions)
# Pastikan Anda menginstall cross-compiler jika beda arsitektur (misal gcc-aarch64-linux-gnu)

# Cari direktori rootfs-templates secara dinamis
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
TEMPLATE_DIR="$(dirname "$SCRIPT_DIR")"

SRC_FILE="${TEMPLATE_DIR}/overlay/usr/src/evdi_bridge/evdi_bridge.c"
OUT_BIN="${TEMPLATE_DIR}/overlay/usr/bin/evdi_bridge"

echo "Building evdi_bridge for rootfs overlay..."

# Gunakan CROSS_COMPILE jika di set (berguna untuk GitHub Actions ubuntu-latest ke arm64)
# Contoh di Github Actions: CROSS_COMPILE=aarch64-linux-gnu- ./build-evdi-host.sh
CC="${CROSS_COMPILE}gcc"

mkdir -p "${TEMPLATE_DIR}/overlay/usr/bin"

$CC "$SRC_FILE" -o "$OUT_BIN" -ldrm
chmod +x "$OUT_BIN"

echo "evdi_bridge successfully built and placed in ${OUT_BIN}!"
