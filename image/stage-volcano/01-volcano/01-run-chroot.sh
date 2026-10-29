#!/bin/bash -e
# Inside the image (aarch64, via qemu on x86 hosts): build the launcher, wire up
# the boot service, strip what a console doesn't need.

BUILD_DEPS="build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libcurl4-openssl-dev libarchive-dev"

apt-get install -y --no-install-recommends ${BUILD_DEPS}
cmake -S /usr/src/volcano -B /tmp/volcano-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/volcano
cmake --build /tmp/volcano-build -j"$(nproc)"
cmake --install /tmp/volcano-build
rm -rf /tmp/volcano-build /usr/src/volcano

# Drop the toolchain, but never the runtime libs the launcher links against.
apt-mark manual libsdl2-2.0-0 libsdl2-ttf-2.0-0 libsdl2-image-2.0-0 libcurl4 libarchive13
apt-get purge -y ${BUILD_DEPS}
apt-get autoremove -y --purge

# Not needed on a console (and swap/man pages only cost SD-card space and wear).
for p in man-db manpages triggerhappy dphys-swapfile ntfs-3g cifs-utils nfs-common apt-listchanges; do
	if dpkg-query -W "$p" >/dev/null 2>&1; then
		apt-get purge -y --auto-remove "$p"
	fi
done

# NetworkManager is the only network stack: the launcher talks to it via nmcli.
# (wpa_supplicant stays: NetworkManager drives it over D-Bus for Wi-Fi.)
systemctl disable dhcpcd.service 2>/dev/null || true
systemctl enable NetworkManager.service bluetooth.service
# Bluetooth firmware loader on Pi 3/4/Zero W (no-op elsewhere).
systemctl enable hciuart.service 2>/dev/null || true

# Boot straight into Volcano OS on tty1 (no login prompt).
systemctl enable volcano.service
systemctl set-default multi-user.target

# No system password anywhere: the console boots straight into the launcher and the
# 'volcano' account has an empty password (tty2 rescue login needs none; SSH password
# login stays refused by sshd's defaults, so use keys if you enable SSH).
passwd -d "${FIRST_USER_NAME}"

# Hardware access for the kiosk user.
usermod -aG bluetooth,video,render,input,audio,netdev "${FIRST_USER_NAME}"

apt-get clean
rm -rf /var/lib/apt/lists/*
