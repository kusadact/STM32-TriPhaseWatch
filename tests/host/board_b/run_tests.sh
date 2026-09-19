#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CORE_DIR="$ROOT_DIR/firmware/board_b"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/board-b-host.XXXXXX")"
CC="${CC:-clang}"

cleanup() {
  rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

echo "CC  host bridge_core.c test_bridge_core.c"
"$CC" -std=c11 -Wall -Wextra -Werror -pedantic -I"$CORE_DIR" \
  -c "$CORE_DIR/bridge_core.c" -o "$BUILD_DIR/bridge_core.o"
"$CC" -std=c11 -Wall -Wextra -Werror -pedantic -I"$CORE_DIR" \
  -c "$CORE_DIR/bridge_dispatch.c" -o "$BUILD_DIR/bridge_dispatch.o"
"$CC" -std=c11 -Wall -Wextra -Werror -pedantic -I"$CORE_DIR" \
  -c "$CORE_DIR/board_b_rx_queue.c" -o "$BUILD_DIR/board_b_rx_queue.o"

"$CC" -std=c11 -Wall -Wextra -Werror -pedantic -I"$CORE_DIR" \
  "$SCRIPT_DIR/test_bridge_core.c" "$BUILD_DIR/bridge_core.o" \
  "$BUILD_DIR/bridge_dispatch.o" "$BUILD_DIR/board_b_rx_queue.o" \
  -o "$BUILD_DIR/board_b_bridge_core_tests"

"$CC" -std=c11 -Wall -Wextra -Werror -pedantic -I"$CORE_DIR" \
  "$SCRIPT_DIR/test_bridge_dispatch.c" "$BUILD_DIR/bridge_core.o" \
  "$BUILD_DIR/bridge_dispatch.o" "$BUILD_DIR/board_b_rx_queue.o" \
  -o "$BUILD_DIR/board_b_bridge_dispatch_tests"

echo "RUN board_b_bridge_core_tests"
"$BUILD_DIR/board_b_bridge_core_tests"

echo "RUN board_b_bridge_dispatch_tests"
"$BUILD_DIR/board_b_bridge_dispatch_tests"
