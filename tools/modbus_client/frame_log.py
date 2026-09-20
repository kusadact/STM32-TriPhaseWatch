"""Exclusive JSONL frame logging with monotonic transaction timing."""

from __future__ import annotations

from datetime import datetime, timezone
import json
import time
from typing import Any

from .errors import FrameLogError


class FrameLogger:
    def __init__(self, path: str, port_description: str) -> None:
        self.path = path
        self.port_description = port_description
        self.started_monotonic = time.monotonic()
        try:
            self._stream = open(path, "x", encoding="utf-8")
        except OSError as exc:
            raise FrameLogError(f"cannot create frame log {path}: {exc}") from exc

    def log(
        self,
        direction: str,
        data: bytes,
        *,
        transaction_id: int,
        deadline: float | None = None,
        result: str | None = None,
        error: str | None = None,
        fragment: bool = False,
        extra: dict[str, Any] | None = None,
    ) -> None:
        now_mono = time.monotonic()
        elapsed_ms = (now_mono - self.started_monotonic) * 1000.0
        event: dict[str, Any] = {
            "utc_time": datetime.now(timezone.utc).isoformat(),
            "monotonic_ms": round(elapsed_ms, 3),
            "elapsed_ms": round(elapsed_ms, 3),
            "transaction": transaction_id,
            "direction": direction,
            "fragment": fragment,
            "bytes_hex": data.hex(" "),
            "byte_count": len(data),
            "port": self.port_description,
            "deadline_ms": (
                round((deadline - self.started_monotonic) * 1000, 3)
                if deadline is not None
                else None
            ),
            "result": result,
            "error": error,
        }
        if extra:
            event.update(extra)
        try:
            self._stream.write(json.dumps(event, separators=(",", ":")) + "\n")
            self._stream.flush()
        except OSError as exc:
            raise FrameLogError(f"cannot write frame log {self.path}: {exc}") from exc

    def close(self) -> None:
        try:
            self._stream.close()
        except OSError as exc:
            raise FrameLogError(f"cannot close frame log {self.path}: {exc}") from exc
