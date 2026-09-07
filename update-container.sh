#!/bin/bash
set -e

echo "Updating container for evdi-bridge and SDDM Wayland..."

# Re-enable evdi-bridge, disable create-disp
systemctl stop create-disp.service || true
systemctl disable create-disp.service || true
systemctl enable evdi-bridge.service
systemctl start evdi-bridge.service

# Compile evdi-bridge
echo "Compiling evdi-bridge..."
gcc -o /usr/bin/evdi-bridge /usr/src/evdi_bridge/evdi_bridge.c -ldrm

# Mask tmp.mount so Android socket isn't hidden
echo "Masking tmp.mount..."
systemctl mask tmp.mount
umount /tmp || true

# Fix CanGraphical=no issue due to read-only /sys
echo "Setting up udev rules for evdi-lindroid master-of-seat..."
cat << 'UDEF' > /etc/udev/rules.d/99-evdi-seat.rules
SUBSYSTEM=="drm", KERNEL=="card*", DRIVERS=="evdi-lindroid", TAG+="master-of-seat", TAG+="seat", TAG+="uaccess"
UDEF

# Manually trigger the seat tag generation if logind is running
mkdir -p /run/udev/data /run/udev/tags/master-of-seat /run/udev/tags/seat
if [ -e /dev/dri/card1 ]; then
    cat << 'UDEF' > /run/udev/data/c226:1
I:1000000
E:DEVPATH=/devices/evdi-lindroid/evdi-lindroid.0/drm/card1
E:DEVNAME=/dev/dri/card1
E:DEVTYPE=drm_minor
E:MAJOR=226
E:MINOR=1
E:SUBSYSTEM=drm
E:ID_PATH=platform-evdi-lindroid.0
E:ID_PATH_TAG=platform-evdi-lindroid_0
E:DEVLINKS=/dev/dri/by-path/platform-evdi-lindroid.0-card
E:TAGS=:master-of-seat:seat:uaccess:
E:CURRENT_TAGS=:master-of-seat:seat:uaccess:
E:ID_FOR_SEAT=drm-platform-evdi-lindroid_0
G:uaccess
G:seat
G:master-of-seat
Q:uaccess
Q:seat
Q:master-of-seat
V:1
UDEF
    touch /run/udev/tags/master-of-seat/c226:1 /run/udev/tags/seat/c226:1
fi

if [ -e /dev/dri/card2 ]; then
    cat << 'UDEF' > /run/udev/data/c226:2
I:1000000
E:DEVPATH=/devices/evdi-lindroid/evdi-lindroid.1/drm/card2
E:DEVNAME=/dev/dri/card2
E:DEVTYPE=drm_minor
E:MAJOR=226
E:MINOR=2
E:SUBSYSTEM=drm
E:ID_PATH=platform-evdi-lindroid.1
E:ID_PATH_TAG=platform-evdi-lindroid_1
E:DEVLINKS=/dev/dri/by-path/platform-evdi-lindroid.1-card
E:TAGS=:master-of-seat:seat:uaccess:
E:CURRENT_TAGS=:master-of-seat:seat:uaccess:
E:ID_FOR_SEAT=drm-platform-evdi-lindroid_1
G:uaccess
G:seat
G:master-of-seat
Q:uaccess
Q:seat
Q:master-of-seat
V:1
UDEF
    touch /run/udev/tags/master-of-seat/c226:2 /run/udev/tags/seat/c226:2
fi

systemctl restart systemd-logind || true

# Setup SDDM Autologin
echo "Configuring SDDM autologin..."
mkdir -p /etc/sddm.conf.d
cat << 'UDEF' > /etc/sddm.conf.d/autologin.conf
[Autologin]
User=lindroid
Session=plasma.desktop
Relogin=false
UDEF

# Ensure DRM device config is set
cat << 'UDEF' > /etc/sddm.conf.d/plasma-wayland.conf
[General]
DisplayServer=wayland

GreeterEnvironment=QT_WAYLAND_SHELL_INTEGRATION=xdg-shell,KWIN_COMPOSE=O2ES,KWIN_DRM_DEVICES=/dev/dri/card1:/dev/dri/card2:/dev/dri/card0,GBM_BACKEND=hybris,__GLX_VENDOR_LIBRARY_NAME=libhybris,__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json,EGL_PLATFORM=lindroid-drm

[Wayland]
CompositorCommand=kwin_wayland --drm --no-global-shortcuts --no-lockscreen --inputmethod maliit-keyboard --locale1
UDEF

echo "Setup complete! Ready for evdi-bridge connection."
