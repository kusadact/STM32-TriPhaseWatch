#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"

echo "RUN GUI model/backend/controller/Tk smoke tests"
PYTHONPATH="$ROOT_DIR:$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" \
  python3 -W error::ResourceWarning -m unittest discover \
  -s "$SCRIPT_DIR" -p 'test_*.py' -v
