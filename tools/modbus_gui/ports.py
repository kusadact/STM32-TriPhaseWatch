"""Serial-port discovery for the GUI connection bar."""

from __future__ import annotations

import glob


def list_serial_ports() -> list[str]:
    """Return macOS callout devices without opening or reading any device."""

    return sorted(set(glob.glob("/dev/cu.*")))
