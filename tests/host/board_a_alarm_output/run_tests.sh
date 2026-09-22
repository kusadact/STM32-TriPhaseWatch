#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
ALARM_DIR="$ROOT_DIR/firmware/board_a/alarm"
PLATFORM_DIR="$ROOT_DIR/firmware/board_a/platform"
BUILD_DIR="$ROOT_DIR/build/host/board_a_alarm_output"
BINARY="$BUILD_DIR/test_board_a_alarm_output"
CC="${CC:-clang}"

mkdir -p "$BUILD_DIR"

"$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -I"$ALARM_DIR" \
  -I"$PLATFORM_DIR" \
  "$SCRIPT_DIR/test_board_a_alarm_output.c" \
  "$ALARM_DIR/board_a_alarm_output.c" \
  -o "$BINARY"

"$BINARY"
