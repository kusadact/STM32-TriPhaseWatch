#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/buscomm-eeprom-tests.XXXXXX")"
BOARD_A_CORE_DIR="$ROOT_DIR/firmware/board_a/core"
BOARD_A_ALARM_DIR="$ROOT_DIR/firmware/board_a/alarm"
CC="${CC:-cc}"

cleanup() {
  rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -pedantic)

echo "HOST test_at24c02"
"$CC" "${CFLAGS[@]}" \
  -I"$ROOT_DIR/firmware/bsp/HARDWARE/IIC" \
  -I"$ROOT_DIR/firmware/bsp/HARDWARE/24CXX" \
  -I"$SCRIPT_DIR/fakes" \
  "$ROOT_DIR/firmware/bsp/HARDWARE/24CXX/at24c02.c" \
  "$SCRIPT_DIR/fakes/soft_i2c.c" \
  "$SCRIPT_DIR/fakes/at24c02_port.c" \
  "$SCRIPT_DIR/test_at24c02.c" \
  -o "$BUILD_DIR/test_at24c02"
"$BUILD_DIR/test_at24c02"

echo "HOST test_config_store"
"$CC" "${CFLAGS[@]}" \
  -I"$ROOT_DIR/firmware/app" \
  -I"$BOARD_A_CORE_DIR" \
  "$ROOT_DIR/firmware/app/config_store.c" \
  "$BOARD_A_CORE_DIR/board_a_record_format.c" \
  "$BOARD_A_ALARM_DIR/board_a_alarm.c" \
  "$ROOT_DIR/firmware/board_a/sensors/ds18b20.c" \
  "$SCRIPT_DIR/test_config_store.c" \
  -o "$BUILD_DIR/test_config_store"
"$BUILD_DIR/test_config_store"

echo "PASS eeprom host tests"
