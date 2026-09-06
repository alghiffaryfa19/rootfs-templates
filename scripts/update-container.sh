#!/bin/bash
set -e

# Pastikan script dijalankan sebagai root
if [ "$EUID" -ne 0 ]; then
  echo "Tolong jalankan script ini sebagai root di dalam kontainer."
  exit 1
fi

# Pastikan script dijalankan dari root direktori repository rootfs-templates
if [ ! -d "overlay" ] || [ ! -d "scripts" ]; then
  echo "Tolong jalankan script ini dari direktori rootfs-templates."
  echo "Contoh: cd /rootfs-templates && ./scripts/update-container.sh"
  exit 1
fi

echo "[*] Menyalin file konfigurasi terbaru dari overlay ke sistem (/, /etc, /usr, dll)..."
# Menggunakan cp -a agar struktur dan atribut file terjaga
cp -a overlay/* /

echo "[*] Memuat ulang konfigurasi systemd..."
systemctl daemon-reload

echo "[*] Melakukan kompilasi ulang evdi_bridge..."
./scripts/build-evdi.sh

echo "[*] Memastikan systemd-udevd berjalan dengan override yang baru..."
systemctl restart systemd-udevd

echo "[*] Merestart layanan sddm dan evdi-bridge..."
systemctl restart evdi-bridge
systemctl restart sddm

echo "[*] Selesai! Kontainer Anda sekarang sudah menjalankan versi terbaru."
