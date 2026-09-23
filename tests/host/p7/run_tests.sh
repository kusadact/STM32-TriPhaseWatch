#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"

run_step() {
  printf '\n==> %s\n' "$*"
  "$@"
}

run_step "$ROOT_DIR/tests/host/board_a_alarm/run_tests.sh"
run_step "$ROOT_DIR/tests/host/board_a_alarm_output/run_tests.sh"
run_step "$ROOT_DIR/tests/host/board_a_event_buffer/run_tests.sh"
run_step "$ROOT_DIR/tests/host/board_a_sensors/run_tests.sh"
run_step "$ROOT_DIR/tests/host/board_a/run_tests.sh"
run_step "$ROOT_DIR/tests/host/board_a_persistence/run_tests.sh"
run_step "$ROOT_DIR/tests/host/board_a_rtos/run_tests.sh"
run_step "$ROOT_DIR/tests/host/eeprom/run_tests.sh"
if [[ "${P7_SKIP_MAC_CLI:-0}" != "1" ]]; then
  run_step "$ROOT_DIR/tests/host/mac_cli/run_tests.sh"
fi
run_step "$ROOT_DIR/tests/host/modbus_gui/run_tests.sh"
run_step python3 -m unittest discover -s "$ROOT_DIR/tests/host/persistence_verifier" -v
run_step "$ROOT_DIR/firmware/build/build_board_a.sh"
run_step "$ROOT_DIR/firmware/build/build_board_b.sh"
run_step python3 -m compileall -q "$ROOT_DIR/tools" "$ROOT_DIR/tests/host"

printf '\nP7 host integration: PASS\n'
