#!/usr/bin/env bash
# Layout rule check: no key prompt may sit on top of text, a button or another prompt,
# and a button's label must fit inside it. Renders every demo screen (and a few
# interaction states) with --ui-lint and fails if any breaks the rule.
#
#   tools/ui-lint.sh [path/to/ZontoVolcano]
set -uo pipefail

BIN="${1:-$(dirname "$0")/../build/ZontoVolcano}"
[ -x "$BIN" ] || { echo "binary not found: $BIN (build first: cmake --build build)" >&2; exit 2; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export VOS_DATA_DIR="$TMP/data"

# name | extra args
CASES=(
  "home|"
  "home, game menu|--press x"
  "home, system menu|--press start"
  "sign in|--screen signin"
  "sign in with code|--screen signin-phone"
  "wifi|--screen wifi"
  "wifi, other network|--screen wifi --press down,down,down,down,down,down,down"
  "bluetooth|--screen bluetooth"
  "about|--screen about"
  "achievements|--screen achievements"
  "social|--screen social"
  "profile|--screen profile"
)

fail=0
for c in "${CASES[@]}"; do
  name="${c%%|*}"; args="${c#*|}"
  # shellcheck disable=SC2086
  out="$("$BIN" --demo --windowed --size 1920x1080 --ui-lint $args --shot "$TMP/shot.png" --shot-frame 90 2>&1)"
  code=$?
  if [ $code -eq 0 ]; then
    printf '  ok    %s\n' "$name"
  else
    fail=1
    printf '  FAIL  %s\n' "$name"
    printf '%s\n' "$out" | grep 'UI LINT' | sed 's/^/          /'
  fi
done
exit $fail
