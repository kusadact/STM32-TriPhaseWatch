#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PYTHON="${PYTHON:-python3}"

echo "RUN GUI model/backend/controller/Tk smoke tests"
PYTHONPATH="$ROOT_DIR:$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" \
  "$PYTHON" -W error::ResourceWarning -m unittest discover \
  -s "$SCRIPT_DIR" -p 'test_*.py' -v
