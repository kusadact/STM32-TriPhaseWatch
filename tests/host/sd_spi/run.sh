#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/host"
CC="${CC:-cc}"

mkdir -p "$BUILD_DIR"

echo "CC  tests/host/sd_spi"
"$CC" -std=c99 -Wall -Wextra -O1 -g \
  -I"$ROOT_DIR/firmware/app/sdcard" \
  -I"$ROOT_DIR/firmware/bsp/FATFS" \
  "$ROOT_DIR/firmware/app/sdcard/sd_spi.c" \
  "$ROOT_DIR/firmware/app/sdcard/sd_diskio.c" \
  "$ROOT_DIR/firmware/app/sdcard/sd_selftest.c" \
  "$ROOT_DIR/firmware/bsp/FATFS/ff.c" \
  "$SCRIPT_DIR/host_sd_bus.c" \
  "$SCRIPT_DIR/host_test_main.c" \
  -o "$BUILD_DIR/sd_spi_test"

echo "RUN $BUILD_DIR/sd_spi_test"
"$BUILD_DIR/sd_spi_test"
