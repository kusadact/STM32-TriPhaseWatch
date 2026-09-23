#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CORE_DIR="$ROOT_DIR/firmware/board_a/core"
ALARM_DIR="$ROOT_DIR/firmware/board_a/alarm"
SENSOR_DIR="$ROOT_DIR/firmware/board_a/sensors"
BUILD_DIR="$ROOT_DIR/build/host/board_a_event_buffer"
BINARY="$BUILD_DIR/test_board_a_event_buffer"
CC="${CC:-clang}"

mkdir -p "$BUILD_DIR"

"$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -I"$CORE_DIR" \
  -I"$ALARM_DIR" \
  -I"$SENSOR_DIR" \
  "$SCRIPT_DIR/test_board_a_event_buffer.c" \
  "$CORE_DIR/board_a_event_buffer.c" \
  -o "$BINARY"

"$BINARY"
