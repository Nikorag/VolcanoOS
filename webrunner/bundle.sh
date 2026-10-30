#!/usr/bin/env bash
# Assemble the web runner (a pinned Electron + our small app) into a directory.
#
#   webrunner/bundle.sh <x86_64|aarch64> <dest-dir>
#
# The AppImage build puts it at usr/lib/zonto-webrunner; the launcher finds it there.
# Needs curl and unzip. The Electron download is verified against a pinned SHA-256.
set -euo pipefail

ARCH="${1:?usage: bundle.sh <x86_64|aarch64> <dest-dir>}"
DEST="${2:?usage: bundle.sh <x86_64|aarch64> <dest-dir>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CACHE="${CACHE:-$HERE/../build-appimage/cache}"

ELECTRON_VERSION="44.10.0"
case "$ARCH" in
  x86_64)  PLATFORM="linux-x64";   SHA256="62df9375ef035f2835a18cd7f9bff1959a185da22f2ecba1daa37cd9caee1dc5" ;;
  aarch64) PLATFORM="linux-arm64"; SHA256="558ae899d98b70236325d81da44bb6b2c988dd9a7bd16be17cea25f06e56cc14" ;;
  *) echo "unsupported arch: $ARCH" >&2; exit 2 ;;
esac

ZIP="$CACHE/electron-v${ELECTRON_VERSION}-${PLATFORM}.zip"
mkdir -p "$CACHE"
if [ ! -f "$ZIP" ]; then
  curl -fsSL --retry 3 -o "$ZIP" \
    "https://github.com/electron/electron/releases/download/v${ELECTRON_VERSION}/electron-v${ELECTRON_VERSION}-${PLATFORM}.zip"
fi

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
GOT="$(sha256 "$ZIP")"
if [ "$GOT" != "$SHA256" ]; then
  rm -f "$ZIP"
  echo "Electron checksum mismatch for $PLATFORM: expected $SHA256, got $GOT" >&2
  exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST"
unzip -q "$ZIP" -d "$DEST"

# The setuid sandbox helper can't work from an AppImage (and a non-setuid copy makes Chromium abort):
# the wrapper relies on user namespaces instead.
rm -f "$DEST/chrome-sandbox"
# English UI only: nothing here shows browser UI, so the other locales are dead weight.
find "$DEST/locales" -type f ! -name 'en-US.pak' -delete

mkdir -p "$DEST/resources/app"
cp "$HERE/app/main.js" "$HERE/app/package.json" "$DEST/resources/app/"
install -m 755 "$HERE/zonto-web-runner" "$DEST/zonto-web-runner"
echo "web runner (Electron $ELECTRON_VERSION, $PLATFORM) -> $DEST"
