#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CORE_DIR="$ROOT_DIR/firmware/board_a/core"
BUILD_DIR="$ROOT_DIR/build/host/board_a_persistence"
CC="${CC:-clang}"

mkdir -p "$BUILD_DIR"

echo "CC  tests/host/board_a_persistence"
"$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -I"$CORE_DIR" \
  "$SCRIPT_DIR/test_persistence.c" \
  "$CORE_DIR/board_a_persistence.c" \
  "$CORE_DIR/board_a_record_format.c" \
  "$CORE_DIR/board_a_storage_engine.c" \
  -o "$BUILD_DIR/test_persistence"

echo "RUN $BUILD_DIR/test_persistence"
"$BUILD_DIR/test_persistence"

"$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -I"$CORE_DIR" \
  "$SCRIPT_DIR/test_model_persistence.c" \
  "$CORE_DIR/modbus_crc.c" \
  "$CORE_DIR/modbus_rtu.c" \
  "$CORE_DIR/board_a_persistence.c" \
  "$CORE_DIR/board_a_record_format.c" \
  "$CORE_DIR/board_a_model.c" \
  -o "$BUILD_DIR/test_model_persistence"

echo "RUN $BUILD_DIR/test_model_persistence"
"$BUILD_DIR/test_model_persistence"
