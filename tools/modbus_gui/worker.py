"""Single serial worker and result queue for the Tk main thread."""

from __future__ import annotations

from dataclasses import dataclass
import queue
import threading
from typing import Any


@dataclass(frozen=True)
class OperationResult:
    operation: str
    operation_id: int
    value: Any = None
    error: Exception | None = None


@dataclass(frozen=True)
class _Command:
    operation: str
    operation_id: int
    args: tuple[Any, ...]
    kwargs: dict[str, Any]


_CLOSE = object()


class WorkerClosedError(RuntimeError):
    pass


class CommandWorker:
    """Execute backend operations serially on one daemon worker thread."""

    def __init__(self, backend: Any) -> None:
        self._backend = backend
        self._commands: queue.Queue[Any] = queue.Queue()
        self._results: queue.Queue[OperationResult] = queue.Queue()
        self._submit_lock = threading.Lock()
        self._closing = threading.Event()
        self._thread = threading.Thread(
            target=self._run,
            name="modbus-gui-worker",
            daemon=True,
        )
        self._next_operation_id = 0
        self._close_error: Exception | None = None
        self._thread.start()

    def submit(self, operation: str, *args: Any, **kwargs: Any) -> int:
        with self._submit_lock:
            if self._closing.is_set():
                raise WorkerClosedError("worker is closing")
            method = getattr(self._backend, operation, None)
            if not callable(method):
                raise AttributeError(f"backend has no operation {operation!r}")
            self._next_operation_id += 1
            operation_id = self._next_operation_id
            self._commands.put(
                _Command(
                    operation=operation,
                    operation_id=operation_id,
                    args=args,
                    kwargs=dict(kwargs),
                )
            )
            return operation_id

    def drain_results(self) -> list[OperationResult]:
        results: list[OperationResult] = []
        while True:
            try:
                results.append(self._results.get_nowait())
            except queue.Empty:
                return results

    def close(self, timeout: float) -> bool:
        if timeout < 0:
            raise ValueError("timeout must be non-negative")
        with self._submit_lock:
            self._closing.set()
            self._commands.put(_CLOSE)
        self._thread.join(timeout)
        return not self._thread.is_alive()

    @property
    def close_error(self) -> Exception | None:
        return self._close_error

    @property
    def is_alive(self) -> bool:
        return self._thread.is_alive()

    def _run(self) -> None:
        while True:
            command = self._commands.get()
            if command is _CLOSE:
                break
            if self._closing.is_set():
                continue
            if not isinstance(command, _Command):
                continue
            try:
                method = getattr(self._backend, command.operation)
                value = method(*command.args, **command.kwargs)
            except Exception as exc:
                self._results.put(
                    OperationResult(
                        operation=command.operation,
                        operation_id=command.operation_id,
                        error=exc,
                    )
                )
            else:
                self._results.put(
                    OperationResult(
                        operation=command.operation,
                        operation_id=command.operation_id,
                        value=value,
                    )
                )

        try:
            self._backend.close()
        except Exception as exc:
            self._close_error = exc
