#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FQBN="${FQBN:-m5stack:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,USBMode=hwcdc,CDCOnBoot=cdc}"
GBPUTER_MODE="${GBPUTER_MODE:-fast}"
USER_EXTRA_FLAGS="${EXTRA_FLAGS:-}"
case "$GBPUTER_MODE" in
  fast|fast-dmg|dmg)
    MODE_NAME="fast"
    MODE_FLAGS="-DGBPUTER_FAST_DMG=1"
    ;;
  compat|gbc|full)
    MODE_NAME="compat"
    MODE_FLAGS="-DGBPUTER_FAST_DMG=0"
    ;;
  *)
    echo "Unknown GBPUTER_MODE: $GBPUTER_MODE" >&2
    echo "Use GBPUTER_MODE=fast or GBPUTER_MODE=compat" >&2
    exit 64
    ;;
esac
BUILD_PATH="${BUILD_PATH:-$ROOT/build/gameboy-puter-$MODE_NAME}"
EXTRA_FLAGS_COMBINED="$MODE_FLAGS ${USER_EXTRA_FLAGS}"
PORT="${1:-${PORT:-}}"

if [[ -z "$PORT" ]]; then
  echo "Usage: $0 /dev/cu.usbmodemXXXX"
  echo
  arduino-cli board list
  exit 64
fi

echo "Building Gameboy-Puter mode=$MODE_NAME"
arduino-cli compile \
  --fqbn "$FQBN" \
  --build-path "$BUILD_PATH" \
  --build-property "build.extra_flags=$EXTRA_FLAGS_COMBINED" \
  "$ROOT"

arduino-cli upload \
  --fqbn "$FQBN" \
  --port "$PORT" \
  --input-dir "$BUILD_PATH" \
  "$ROOT"
