#!/usr/bin/env bash
# Build a flashable Volcano OS SD-card image (Raspberry Pi OS Lite + the launcher).
#
#   ./image/build.sh            -> image/out/<date>-volcano-os.img.xz
#
# Needs Docker (Docker Desktop on macOS, or any Linux box) and ~15 GB free disk.
# Settings via environment:
#   VOLCANO_COUNTRY   Wi-Fi regulatory country code        (default GB)
#   VOLCANO_TZ        timezone                             (default Europe/London)
#   VOLCANO_PASSWORD  password of the 'volcano' user       (default: random, printed at the end)
#   VOLCANO_SSH       1 = enable the SSH server            (default 0)
#   PIGEN_REF         pi-gen branch/tag to build with      (default bookworm-arm64)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
WORK="${WORK:-$HERE/work}"
OUT="$HERE/out"
PIGEN_REF="${PIGEN_REF:-bookworm-arm64}"
COUNTRY="${VOLCANO_COUNTRY:-GB}"
TZONE="${VOLCANO_TZ:-Europe/London}"
SSH="${VOLCANO_SSH:-0}"
GENERATED_PW=0
if [ -z "${VOLCANO_PASSWORD:-}" ]; then
  VOLCANO_PASSWORD="$(LC_ALL=C tr -dc 'a-z0-9' </dev/urandom | head -c 12)"
  GENERATED_PW=1
fi

command -v docker >/dev/null || { echo "docker is required (https://docs.docker.com/get-docker/)" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "docker is installed but not running" >&2; exit 1; }

mkdir -p "$WORK" "$OUT"
if [ ! -d "$WORK/pi-gen/.git" ]; then
  git clone --depth 1 --branch "$PIGEN_REF" https://github.com/RPi-Distro/pi-gen "$WORK/pi-gen"
fi
PIGEN="$WORK/pi-gen"

# Our stage + a snapshot of the launcher source (tracked and untracked, minus ignored files).
rm -rf "$PIGEN/stage-volcano"
cp -R "$HERE/stage-volcano" "$PIGEN/stage-volcano"
SRC="$PIGEN/stage-volcano/01-volcano/files/volcano-src"
mkdir -p "$SRC"
git -C "$REPO" ls-files -z --cached --others --exclude-standard -- . ':(exclude)image' ':(exclude).github' \
  | tar -C "$REPO" --null -T - -cf - | tar -C "$SRC" -xf -
chmod +x "$PIGEN"/stage-volcano/prerun.sh "$PIGEN"/stage-volcano/01-volcano/*.sh

# Lite (stage0-2) is the base; only our stage exports an image.
touch "$PIGEN/stage2/SKIP_IMAGES" "$PIGEN/stage2/SKIP_NOOBS"

cat > "$PIGEN/config" <<EOF
IMG_NAME=volcano-os
RELEASE=bookworm
TARGET_HOSTNAME=volcano
LOCALE_DEFAULT=en_GB.UTF-8
KEYBOARD_KEYMAP=gb
KEYBOARD_LAYOUT="English (UK)"
TIMEZONE_DEFAULT=${TZONE}
WPA_COUNTRY=${COUNTRY}
FIRST_USER_NAME=volcano
FIRST_USER_PASS=${VOLCANO_PASSWORD}
DISABLE_FIRST_BOOT_USER_RENAME=1
ENABLE_SSH=${SSH}
DEPLOY_COMPRESSION=xz
STAGE_LIST="stage0 stage1 stage2 stage-volcano"
EOF

echo ">> building with pi-gen ($PIGEN_REF); this takes a while (30-90 min the first time)"
( cd "$PIGEN" && ./build-docker.sh )

IMG="$(ls -t "$PIGEN"/deploy/*volcano-os*.img.xz 2>/dev/null | head -n1 || true)"
[ -n "$IMG" ] || { echo "build finished but no image was found in $PIGEN/deploy" >&2; exit 1; }
cp "$IMG" "$OUT/"
echo
echo "Image:    $OUT/$(basename "$IMG")"
echo "Login:    volcano / ${VOLCANO_PASSWORD}  (console rescue on tty2, and SSH if enabled)"
[ "$GENERATED_PW" = 1 ] && echo "          (password was generated; set VOLCANO_PASSWORD to choose your own)"
echo "Flash it: see image/README.md"
