#!/bin/bash
# Turn a fresh Debian 12 (bookworm) arm64 VM into a Volcano OS console, using the same
# package list, service, Plymouth theme and Chromium settings as the Pi image.
#
#   sudo ./utm/provision.sh        (from a checkout of this repo, inside the VM)
#
# Afterwards: sudo reboot. It boots straight into Volcano OS like the Pi does.
# Updating after code changes: sudo ./utm/provision.sh --update   (rebuild + restart only)
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }

REPO="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$REPO/image/stage-volcano"
USER_NAME="${VOLCANO_USER:-volcano}"
BUILD_DEPS="build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libcurl4-openssl-dev libarchive-dev"

build() {
  apt-get install -y --no-install-recommends $BUILD_DEPS
  rm -rf /tmp/volcano-build
  cmake -S "$REPO" -B /tmp/volcano-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/volcano
  cmake --build /tmp/volcano-build -j"$(nproc)"
  cmake --install /tmp/volcano-build
}

if [ "${1:-}" = "--update" ]; then
  build
  systemctl restart volcano.service
  exit 0
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
# Same runtime packages as the image (skip Pi-only ones: pi-bluetooth).
grep -v '^\s*#' "$STAGE/00-packages/00-packages" | grep -v '^\s*$' | grep -v '^pi-bluetooth$' \
  | xargs apt-get install -y --no-install-recommends
build

id "$USER_NAME" >/dev/null 2>&1 || useradd -m -s /bin/bash "$USER_NAME"
passwd -d "$USER_NAME"
usermod -aG bluetooth,video,render,input,audio,netdev "$USER_NAME" 2>/dev/null || true

F="$STAGE/01-volcano/files"
install -m 644 "$F/volcano.service" /etc/systemd/system/volcano.service
install -d /usr/libexec/volcano
install -m 755 "$F/volcano-session" /usr/libexec/volcano/session
install -m 755 "$F/xdg-open" /usr/local/bin/xdg-open
install -d /etc/chromium.d
install -m 644 "$F/chromium-volcano" /etc/chromium.d/volcano
install -d /etc/NetworkManager/conf.d
install -m 644 "$F/networkmanager-volcano.conf" /etc/NetworkManager/conf.d/volcano.conf
install -d /usr/share/plymouth/themes/volcano
install -m 644 "$F"/plymouth/volcano/* /usr/share/plymouth/themes/volcano/
plymouth-set-default-theme volcano
update-initramfs -u || true

# Quiet boot + Plymouth, as on the Pi.
if ! grep -q 'plymouth.ignore-serial-consoles' /etc/default/grub; then
  sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT="quiet splash plymouth.ignore-serial-consoles systemd.show_status=false loglevel=3 vt.global_cursor_default=0"/' /etc/default/grub
  update-grub || true
fi

# Debian's desktop-less installs manage the network with ifupdown; hand it to NetworkManager.
sed -i 's/^\(auto\|allow-hotplug\|iface\)/#\1/' /etc/network/interfaces 2>/dev/null || true
systemctl disable --now systemd-networkd 2>/dev/null || true
systemctl enable NetworkManager.service bluetooth.service volcano.service
systemctl set-default multi-user.target
echo
echo "Done. Reboot to boot into Volcano OS:  sudo reboot"
