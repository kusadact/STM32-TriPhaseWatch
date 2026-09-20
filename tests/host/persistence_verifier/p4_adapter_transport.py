"""Subprocess transport for the P5 real-C storage adapter."""

from __future__ import annotations

from collections import deque
import os
from pathlib import Path
import struct
import subprocess
import time


ADAPTER_EXCHANGE = 0x01


class SubprocessAdapterTransport:
    """Speaks the adapter framing; it is a serial substitute, not a device."""

    def __init__(
        self,
        executable: Path,
        storage_root: Path,
        config_image: Path,
        *,
        time_step_us: int = 10_000_000,
        save_delay_calls: int = 2,
    ) -> None:
        self.executable = Path(executable)
        self.storage_root = Path(storage_root)
        self.config_image = Path(config_image)
        self.time_step_us = time_step_us
        self.save_delay_calls = save_delay_calls
        self.description = f"synthetic-p4-adapter:{self.executable}"
        self.open_count = 0
        self.close_count = 0
        self.process: subprocess.Popen[bytes] | None = None
        self._pending: deque[bytes] = deque()

    def open(self) -> None:
        if self.process is not None and self.process.poll() is None:
            return
        self.storage_root.mkdir(parents=True, exist_ok=True)
        environment = os.environ.copy()
        environment["P5_STORAGE_ROOT"] = str(self.storage_root)
        environment["P5_CONFIG_IMAGE"] = str(self.config_image)
        environment["P5_TIME_STEP_US"] = str(self.time_step_us)
        environment["P5_SAVE_DELAY_CALLS"] = str(self.save_delay_calls)
        self.process = subprocess.Popen(
            [str(self.executable), str(self.storage_root), str(self.config_image)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        self.open_count += 1

    def _require_process(self) -> subprocess.Popen[bytes]:
        if self.process is None or self.process.poll() is not None:
            raise RuntimeError("synthetic adapter is not running")
        return self.process

    def _read_exact(self, length: int) -> bytes:
        process = self._require_process()
        assert process.stdout is not None
        data = process.stdout.read(length)
        if data is None or len(data) != length:
            stderr = b""
            if process.stderr is not None:
                stderr = process.stderr.read()
            raise RuntimeError(
                f"adapter ended early: wanted {length} bytes, got "
                f"{0 if data is None else len(data)}; stderr={stderr!r}"
            )
        return data

    def write_all(self, data: bytes, deadline: float) -> list[bytes]:
        process = self._require_process()
        if time.monotonic() >= deadline:
            raise TimeoutError("synthetic adapter write deadline expired")
        assert process.stdin is not None
        process.stdin.write(
            bytes((ADAPTER_EXCHANGE,))
            + struct.pack(">H", len(data))
            + data
        )
        process.stdin.flush()
        status = self._read_exact(1)
        if status != b"\x00":
            raise RuntimeError(f"adapter status {status!r}")
        response_length = struct.unpack(">H", self._read_exact(2))[0]
        if response_length:
            self._pending.append(self._read_exact(response_length))
        return [data]

    def read(self, max_bytes: int, deadline: float) -> bytes | None:
        if not self._pending:
            return None
        response = self._pending.popleft()
        if len(response) > max_bytes:
            self._pending.appendleft(response[max_bytes:])
            return response[:max_bytes]
        return response

    def read_available(self, max_bytes: int = 512) -> bytes:
        if not self._pending:
            return b""
        data = b"".join(self._pending)
        self._pending.clear()
        return data[:max_bytes]

    def recover(
        self,
        duration_seconds: float,
        quiet_seconds: float,
        deadline: float,
    ) -> bool:
        del duration_seconds, quiet_seconds, deadline
        return True

    def close(self) -> None:
        process = self.process
        self.process = None
        if process is None:
            return
        self.close_count += 1
        if process.stdin is not None:
            try:
                process.stdin.write(b"\x00")
                process.stdin.flush()
            except (BrokenPipeError, OSError):
                pass
            try:
                process.stdin.close()
            except OSError:
                pass
        try:
            process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2.0)
        if process.returncode != 0:
            stderr = b""
            if process.stderr is not None:
                stderr = process.stderr.read()
            raise RuntimeError(
                f"synthetic adapter exited {process.returncode}: "
                f"{stderr.decode(errors='replace')}"
            )
        if process.stdout is not None:
            process.stdout.close()
        if process.stderr is not None:
            process.stderr.close()

    def __enter__(self) -> "SubprocessAdapterTransport":
        self.open()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()
