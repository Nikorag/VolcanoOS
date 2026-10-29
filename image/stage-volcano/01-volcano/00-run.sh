#!/bin/bash -e
# Host side: copy files into the image root. (build.sh drops the launcher
# source into files/volcano-src before pi-gen starts.)

install -d "${ROOTFS_DIR}/usr/src/volcano"
cp -a files/volcano-src/. "${ROOTFS_DIR}/usr/src/volcano/"

install -m 644 files/volcano.service "${ROOTFS_DIR}/etc/systemd/system/volcano.service"
install -d "${ROOTFS_DIR}/usr/libexec/volcano"
install -m 755 files/volcano-session "${ROOTFS_DIR}/usr/libexec/volcano/session"
install -m 755 files/xdg-open "${ROOTFS_DIR}/usr/local/bin/xdg-open"
install -d "${ROOTFS_DIR}/etc/chromium.d"
install -m 644 files/chromium-volcano "${ROOTFS_DIR}/etc/chromium.d/volcano"
install -d "${ROOTFS_DIR}/etc/systemd/journald.conf.d"
install -m 644 files/journald-volcano.conf "${ROOTFS_DIR}/etc/systemd/journald.conf.d/volcano.conf"
install -m 644 files/modprobe-volcano.conf "${ROOTFS_DIR}/etc/modprobe.d/volcano.conf"
install -m 644 files/modules-volcano.conf "${ROOTFS_DIR}/etc/modules-load.d/volcano.conf"
install -d "${ROOTFS_DIR}/etc/NetworkManager/conf.d"
install -m 644 files/networkmanager-volcano.conf "${ROOTFS_DIR}/etc/NetworkManager/conf.d/volcano.conf"

# Quiet, flicker-free boot straight into the launcher, and a regulatory domain
# so the Wi-Fi radio is not left blocked on first boot.
COUNTRY="${WPA_COUNTRY:-GB}"
sed -i "1s/\$/ quiet loglevel=3 logo.nologo vt.global_cursor_default=0 consoleblank=0 cfg80211.ieee80211_regdom=${COUNTRY}/" \
	"${ROOTFS_DIR}/boot/firmware/cmdline.txt"
cat >> "${ROOTFS_DIR}/boot/firmware/config.txt" <<'EOF'

# --- Volcano OS ---
disable_splash=1
boot_delay=0
EOF
