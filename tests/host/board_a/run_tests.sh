#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CORE_DIR="$ROOT_DIR/firmware/board_a/core"
BUILD_DIR="$ROOT_DIR/build/host/board_a"
BINARY="$BUILD_DIR/test_board_a"
CC="${CC:-clang}"

mkdir -p "$BUILD_DIR"

"$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -I"$CORE_DIR" \
  "$SCRIPT_DIR/test_board_a.c" \
  "$CORE_DIR/modbus_crc.c" \
  "$CORE_DIR/modbus_rtu.c" \
  "$CORE_DIR/modbus_rtu_rx.c" \
  "$CORE_DIR/board_a_log_schedule.c" \
  "$CORE_DIR/board_a_persistence.c" \
  "$CORE_DIR/board_a_record_format.c" \
  "$CORE_DIR/board_a_model.c" \
  "$CORE_DIR/board_a_slave.c" \
  -o "$BINARY"

"$BINARY"
