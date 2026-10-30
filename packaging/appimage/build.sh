#!/usr/bin/env bash
# Build a portable Zonto Volcano AppImage (Steam Deck / any x86_64 or aarch64 Linux desktop).
#
#   packaging/appimage/build.sh            -> dist/Zonto_Volcano-<version>-<arch>.AppImage
#
# Needs: cmake, a C++17 compiler, SDL2/SDL2_ttf/SDL2_image/libcurl/libarchive dev packages,
# librsvg2-bin (rsvg-convert), imagemagick, curl, unzip, file, patchelf. Build on the oldest distro
# you want to support: the bundle uses the build machine's glibc (Debian bookworm = 2.36 is
# older than SteamOS's, which is what matters for the Deck).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ARCH="${ARCH:-$(uname -m)}"            # x86_64 or aarch64
WORK="${WORK:-$REPO/build-appimage}"
OUT="${OUT:-$REPO/dist}"
LINUXDEPLOY_VER="${LINUXDEPLOY_VER:-1-alpha-20251107-1}"
APPIMAGE_PLUGIN_VER="${APPIMAGE_PLUGIN_VER:-1-alpha-20250213-1}"

VERSION="$(sed -n 's/^project(ZontoVolcano VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")"
[ -n "$VERSION" ] || { echo "could not read the version from CMakeLists.txt" >&2; exit 1; }

TOOLS="$WORK/tools"
APPDIR="$WORK/AppDir"
mkdir -p "$TOOLS" "$OUT"

fetch() {  # fetch <url> <dest>
  [ -x "$2" ] || { curl -fsSL --retry 3 -o "$2" "$1" && chmod +x "$2"; }
}
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VER}/linuxdeploy-${ARCH}.AppImage" \
      "$TOOLS/linuxdeploy-${ARCH}.AppImage"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/${APPIMAGE_PLUGIN_VER}/linuxdeploy-plugin-appimage-${ARCH}.AppImage" \
      "$TOOLS/linuxdeploy-plugin-appimage-${ARCH}.AppImage"

# CI containers have no FUSE: let the AppImages unpack themselves instead of mounting.
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$TOOLS:$PATH"
export NO_STRIP=1   # stripping breaks some distro libs' debuglinks and saves little

echo ">> building the launcher"
rm -rf "$APPDIR"
cmake -S "$REPO" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build "$WORK/build" -j"$(nproc)"
DESTDIR="$APPDIR" cmake --install "$WORK/build" >/dev/null

# Icon: the logo is SVG and not square; linuxdeploy wants a square PNG.
echo ">> rendering the icon"
rsvg-convert -w 256 -h 256 --keep-aspect-ratio "$REPO/assets/logo/zonto-volcano.svg" -o "$WORK/logo.png"
convert "$WORK/logo.png" -background none -gravity center -extent 256x256 "$WORK/zonto-volcano.png"

echo ">> bundling the launcher's libraries"
export VERSION
export LDAI_OUTPUT="Zonto_Volcano-${VERSION}-${ARCH}.AppImage"
( cd "$WORK" && "$TOOLS/linuxdeploy-${ARCH}.AppImage" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/ZontoVolcano" \
    --desktop-file "$HERE/zonto-volcano.desktop" \
    --icon-file "$WORK/zonto-volcano.png" )

# Web games run in the bundled web runner (a pinned Electron with no browser UI), so nothing needs
# installing on the target machine. It is added *after* linuxdeploy on purpose: linuxdeploy scans
# every ELF file in the AppDir and would try to bundle Electron's own system-library dependencies
# (GTK, NSS...), which belong to the target system, not to us. Checksum-verified inside bundle.sh.
echo ">> bundling the web runner"
CACHE="$WORK/cache" "$REPO/webrunner/bundle.sh" "$ARCH" "$APPDIR/usr/lib/zonto-webrunner"

echo ">> packing the AppImage"
( cd "$WORK" && "$TOOLS/linuxdeploy-plugin-appimage-${ARCH}.AppImage" --appdir "$APPDIR" )

mv "$WORK/$LDAI_OUTPUT" "$OUT/"
echo
echo "AppImage: $OUT/$LDAI_OUTPUT"
