#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CORE_DIR="$ROOT_DIR/firmware/board_a/core"
SENSOR_DIR="$ROOT_DIR/firmware/board_a/sensors"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mac-modbus-cli-host.XXXXXX")"
CC="${CC:-clang}"
PYTHON="${PYTHON:-python3}"

cleanup() {
  rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

echo "CC  board_a_core_adapter.c + fixed bare-metal core"
"$CC" -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g \
  -I"$CORE_DIR" \
  "$SCRIPT_DIR/board_a_core_adapter.c" \
  "$CORE_DIR/board_a_slave.c" \
  "$CORE_DIR/modbus_rtu_rx.c" \
  "$CORE_DIR/modbus_crc.c" \
  "$CORE_DIR/modbus_rtu.c" \
  "$CORE_DIR/board_a_persistence.c" \
  "$CORE_DIR/board_a_record_format.c" \
  "$SENSOR_DIR/ds18b20.c" \
  "$CORE_DIR/board_a_model.c" \
  -o "$BUILD_DIR/board_a_core_adapter"

echo "RUN Python protocol/transport/CLI/C-core tests"
PYTHONPATH="$ROOT_DIR:$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" \
  MODBUS_TEST_CORE_ADAPTER="$BUILD_DIR/board_a_core_adapter" \
  "$PYTHON" -W error::ResourceWarning -m unittest discover \
  -s "$SCRIPT_DIR" -p 'test_*.py' -v
