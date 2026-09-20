"""Raw evidence capture shared by the P5 runner and product client."""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import json
from pathlib import Path
import threading
import time
from typing import Any, Iterator, Mapping

from .manifest import RunArtifacts


def _json_safe(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, bytes):
        return value.hex(" ")
    if isinstance(value, Mapping):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_json_safe(item) for item in value]
    return repr(value)


class JsonlWriter:
    def __init__(self, path: Path) -> None:
        self.path = path
        self._stream = path.open("a", encoding="utf-8")
        self._lock = threading.Lock()

    def write(self, value: Mapping[str, Any]) -> None:
        encoded = json.dumps(
            _json_safe(dict(value)),
            ensure_ascii=True,
            sort_keys=True,
            separators=(",", ":"),
        )
        with self._lock:
            self._stream.write(encoded + "\n")
            self._stream.flush()

    def close(self) -> None:
        with self._lock:
            if not self._stream.closed:
                self._stream.close()


class EvidenceRecorder:
    """Writes command intent, transaction, observation, and frame evidence."""

    def __init__(self, artifacts: RunArtifacts) -> None:
        self.artifacts = artifacts
        self.commands = JsonlWriter(artifacts.commands_path)
        self.transactions = JsonlWriter(artifacts.transactions_path)
        self.frames = JsonlWriter(artifacts.frames_path)
        self.observations = JsonlWriter(artifacts.observations_path)
        self.checkpoints = JsonlWriter(artifacts.checkpoints_path)
        self._command_counter = 0
        self._transaction_counter = 0
        self._current_command_id: int | None = None
        self._command_transactions: dict[int, list[int]] = {}

    @property
    def current_command_id(self) -> int | None:
        return self._current_command_id

    @contextmanager
    def command(
        self,
        operation: str,
        parameters: Mapping[str, Any] | None = None,
    ) -> Iterator[int]:
        self._command_counter += 1
        command_id = self._command_counter
        previous = self._current_command_id
        self._current_command_id = command_id
        self._command_transactions[command_id] = []
        started_utc = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        started_mono = time.monotonic()
        self.commands.write(
            {
                "command_id": command_id,
                "operation": operation,
                "parameters": dict(parameters or {}),
                "started_utc": started_utc,
                "started_monotonic_s": started_mono,
                "status": "RUNNING",
            }
        )
        try:
            yield command_id
        except BaseException as exc:
            self.commands.write(
                {
                    "command_id": command_id,
                    "operation": operation,
                    "parameters": dict(parameters or {}),
                    "ended_utc": datetime.now(timezone.utc).isoformat(
                        timespec="milliseconds"
                    ),
                    "ended_monotonic_s": time.monotonic(),
                    "elapsed_ms": (time.monotonic() - started_mono) * 1000.0,
                    "status": (
                        "PARTIAL" if isinstance(exc, KeyboardInterrupt) else "FAILED"
                    ),
                    "error": _json_safe(exc),
                    "transaction_ids": self._command_transactions[command_id],
                }
            )
            raise
        else:
            self.commands.write(
                {
                    "command_id": command_id,
                    "operation": operation,
                    "parameters": dict(parameters or {}),
                    "ended_utc": datetime.now(timezone.utc).isoformat(
                        timespec="milliseconds"
                    ),
                    "ended_monotonic_s": time.monotonic(),
                    "elapsed_ms": (time.monotonic() - started_mono) * 1000.0,
                    "status": "PASS",
                    "transaction_ids": self._command_transactions[command_id],
                }
            )
        finally:
            self._current_command_id = previous

    def record_transaction(
        self,
        *,
        request: bytes,
        started_monotonic_s: float,
        started_utc: str,
        elapsed_ms: float,
        response_bytes: bytes | None,
        error: BaseException | None,
    ) -> int:
        self._transaction_counter += 1
        transaction_id = self._transaction_counter
        if self._current_command_id is not None:
            self._command_transactions[self._current_command_id].append(transaction_id)
        self.transactions.write(
            {
                "transaction_id": transaction_id,
                "command_id": self._current_command_id,
                "request_hex": request.hex(" "),
                "response_hex": (
                    response_bytes.hex(" ") if response_bytes is not None else None
                ),
                "started_utc": started_utc,
                "started_monotonic_s": started_monotonic_s,
                "ended_monotonic_s": time.monotonic(),
                "elapsed_ms": elapsed_ms,
                "status": "PASS" if error is None else "FAILED",
                "error": None if error is None else _json_safe(error),
            }
        )
        self.frames.write(
            {
                "source": "assembled",
                "transaction": transaction_id,
                "direction": "tx",
                "bytes_hex": request.hex(" "),
                "byte_count": len(request),
                "result": "assembled",
            }
        )
        if response_bytes is not None:
            self.frames.write(
                {
                    "source": "assembled",
                    "transaction": transaction_id,
                    "direction": "rx",
                    "bytes_hex": response_bytes.hex(" "),
                    "byte_count": len(response_bytes),
                    "result": "assembled",
                }
            )
        return transaction_id

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
        extra: Mapping[str, Any] | None = None,
    ) -> None:
        self.frames.write(
            {
                "source": "product_frame_logger",
                "transaction": transaction_id,
                "direction": direction,
                "fragment": fragment,
                "bytes_hex": data.hex(" "),
                "byte_count": len(data),
                "deadline": deadline,
                "result": result,
                "error": error,
                "extra": dict(extra or {}),
            }
        )

    def record_observation(
        self,
        kind: str,
        value: Mapping[str, Any],
        *,
        command_id: int | None = None,
        transaction_ids: list[int] | None = None,
        metadata: Mapping[str, Any] | None = None,
    ) -> None:
        self.observations.write(
            {
                "observed_utc": datetime.now(timezone.utc).isoformat(
                    timespec="milliseconds"
                ),
                "observed_monotonic_s": time.monotonic(),
                "kind": kind,
                "command_id": (
                    command_id
                    if command_id is not None
                    else self._current_command_id
                ),
                "transaction_ids": list(transaction_ids or []),
                "value": dict(value),
                "metadata": dict(metadata or {}),
            }
        )

    def checkpoint(
        self,
        label: str,
        value: Mapping[str, Any],
        *,
        transaction_ids: list[int] | None = None,
    ) -> None:
        self.checkpoints.write(
            {
                "checkpoint": label,
                "observed_utc": datetime.now(timezone.utc).isoformat(
                    timespec="milliseconds"
                ),
                "observed_monotonic_s": time.monotonic(),
                "command_id": self._current_command_id,
                "transaction_ids": list(transaction_ids or []),
                "value": dict(value),
            }
        )

    def close(self) -> None:
        for writer in (
            self.commands,
            self.transactions,
            self.frames,
            self.observations,
            self.checkpoints,
        ):
            writer.close()


@dataclass
class CapturingClient:
    """Delegates transactions while preserving request/response evidence."""

    client: Any
    recorder: EvidenceRecorder

    def transaction(self, request: bytes) -> Any:
        started_mono = time.monotonic()
        started_utc = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        try:
            result = self.client.transaction(request)
        except BaseException as exc:
            self.recorder.record_transaction(
                request=request,
                started_monotonic_s=started_mono,
                started_utc=started_utc,
                elapsed_ms=(time.monotonic() - started_mono) * 1000.0,
                response_bytes=None,
                error=exc,
            )
            raise
        self.recorder.record_transaction(
            request=request,
            started_monotonic_s=started_mono,
            started_utc=started_utc,
            elapsed_ms=float(getattr(result, "elapsed_ms", 0.0)),
            response_bytes=bytes(getattr(result, "response_bytes", b"")),
            error=None,
        )
        return result

    def __getattr__(self, name: str) -> Any:
        return getattr(self.client, name)
