"""Run the GUI with ``python3 -m tools.modbus_gui [--demo]``."""

from __future__ import annotations

import sys


def main() -> int:
    demo = "--demo" in sys.argv[1:]
    try:
        from .ui import main as ui_main
    except ModuleNotFoundError as exc:
        if exc.name not in {"tkinter", "_tkinter"}:
            raise
        print(
            "Tkinter 不可用。请使用带 Tk 的 Python 3 解释器，例如 "
            "`/opt/homebrew/bin/python3 -m tools.modbus_gui`。",
            file=sys.stderr,
        )
        return 2
    return ui_main(demo=demo)


if __name__ == "__main__":
    raise SystemExit(main())
