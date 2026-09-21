#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SENSOR_DIR="$ROOT_DIR/firmware/board_a/sensors"
BUILD_DIR="$ROOT_DIR/build/host/board_a_sensors"
BINARY="$BUILD_DIR/test_dht11"
CC="${CC:-clang}"

mkdir -p "$BUILD_DIR"

"$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -I"$SENSOR_DIR" \
  "$SCRIPT_DIR/test_dht11.c" \
  "$SENSOR_DIR/dht11.c" \
  "$SENSOR_DIR/sensor_manager.c" \
  -o "$BINARY"

"$BINARY"
