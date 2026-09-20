"""Transport interface and a dependency-free macOS termios implementation."""

from __future__ import annotations

import errno
import fcntl
import os
import select
import termios
import time
import tty
from typing import Protocol

from .errors import TransportError
from .frame_log import FrameLogger


class Transport(Protocol):
    @property
    def description(self) -> str: ...

    def open(self) -> None: ...

    def write_all(self, data: bytes, deadline: float) -> list[bytes]: ...

    def read(self, max_bytes: int, deadline: float) -> bytes | None: ...

    def read_available(self, max_bytes: int = 512) -> bytes: ...

    def recover(
        self,
        duration_seconds: float,
        quiet_seconds: float,
        deadline: float,
    ) -> bool: ...

    def close(self) -> None: ...


class MacOSTTYTransport:
    """Raw 9600 8E1 serial transport using only the Python standard library."""

    def __init__(
        self,
        port: str,
        *,
        baudrate: int = 9600,
        initial_recovery_seconds: float = 1.5,
        initial_quiet_seconds: float = 0.01,
        initial_timeout_seconds: float = 3.0,
        logger: FrameLogger | None = None,
    ) -> None:
        self.port = port
        self.baudrate = baudrate
        self.initial_recovery_seconds = initial_recovery_seconds
        self.initial_quiet_seconds = initial_quiet_seconds
        self.initial_timeout_seconds = initial_timeout_seconds
        self.logger = logger
        self._fd: int | None = None
        self._original_attributes: list[object] | None = None

    @property
    def description(self) -> str:
        return f"{self.port}:{self.baudrate}:8E1"

    def open(self) -> None:
        if self._fd is not None:
            return
        try:
            fd = os.open(
                self.port,
                os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK,
            )
        except OSError as exc:
            raise TransportError(f"cannot open {self.port}: {exc}") from exc

        original: list[object] | None = None
        try:
            original = termios.tcgetattr(fd)
            ioctl_exclusive = getattr(termios, "TIOCEXCL", None)
            if ioctl_exclusive is not None:
                fcntl.ioctl(fd, ioctl_exclusive, 0)
            tty.setraw(fd, termios.TCSANOW)
            settings = termios.tcgetattr(fd)
            settings[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
            settings[2] &= ~(
                termios.CSIZE
                | termios.PARENB
                | termios.PARODD
                | termios.CSTOPB
                | termios.CRTSCTS
            )
            settings[2] |= termios.CS8 | termios.PARENB | termios.CLOCAL | termios.CREAD
            settings[4] = termios.B9600
            settings[5] = termios.B9600
            settings[6][termios.VMIN] = 0
            settings[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, settings)
            self._verify_attributes(fd)
            self._fd = fd
            self._original_attributes = original
            stale = self.read_available(512)
            if stale and self.logger is not None:
                self.logger.log(
                    "open_drain",
                    stale,
                    transaction_id=0,
                    result="discarded",
                )
            termios.tcflush(fd, termios.TCIOFLUSH)
            deadline = time.monotonic() + self.initial_timeout_seconds
            if not self.recover(
                self.initial_recovery_seconds,
                self.initial_quiet_seconds,
                deadline,
            ):
                raise TransportError(
                    "serial input did not become quiet during initialization"
                )
        except Exception:
            if self._fd == fd:
                self._fd = None
                self._original_attributes = None
            if original is not None:
                try:
                    termios.tcsetattr(fd, termios.TCSANOW, original)
                except OSError:
                    pass
            try:
                os.close(fd)
            except OSError:
                pass
            raise

    @staticmethod
    def _verify_attributes(fd: int) -> None:
        settings = termios.tcgetattr(fd)
        cflag = settings[2]
        if (cflag & termios.CSIZE) != termios.CS8:
            raise TransportError("serial readback is not configured for 8 data bits")
        if not cflag & termios.PARENB:
            raise TransportError("serial readback is not configured for parity")
        if cflag & termios.PARODD:
            raise TransportError("serial readback is not configured for even parity")
        if cflag & termios.CSTOPB:
            raise TransportError("serial readback is not configured for 1 stop bit")
        if settings[4] != termios.B9600 or settings[5] != termios.B9600:
            raise TransportError("serial readback is not configured for 9600 baud")
        if cflag & termios.CRTSCTS:
            raise TransportError("serial readback has hardware flow control enabled")

    def write_all(self, data: bytes, deadline: float) -> list[bytes]:
        fd = self._require_open()
        written = 0
        fragments: list[bytes] = []
        while written < len(data):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TransportError(
                    f"serial write deadline expired after {written}/{len(data)} bytes",
                    bytes_written=written,
                )
            try:
                count = os.write(fd, data[written:])
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    self._wait_writable(remaining)
                    continue
                raise TransportError(
                    f"serial write failed: {exc}",
                    bytes_written=written,
                ) from exc
            if count <= 0:
                raise TransportError(
                    "serial write returned zero bytes",
                    bytes_written=written,
                )
            fragment = data[written : written + count]
            fragments.append(fragment)
            if self.logger is not None:
                self.logger.log(
                    "tx",
                    fragment,
                    transaction_id=getattr(self, "_log_transaction", 0),
                    deadline=deadline,
                    fragment=True,
                    extra={"written_total": written + count},
                )
            written += count
        return fragments

    def read(self, max_bytes: int, deadline: float) -> bytes | None:
        fd = self._require_open()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                data = os.read(fd, max_bytes)
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    self._wait_readable_fd(remaining)
                    continue
                raise TransportError(f"serial read failed: {exc}") from exc
            if not data:
                self._wait_readable_fd(remaining)
                continue
            if self.logger is not None:
                self.logger.log(
                    "rx",
                    data,
                    transaction_id=getattr(self, "_log_transaction", 0),
                    deadline=deadline,
                    fragment=True,
                )
            return data

    def read_available(self, max_bytes: int = 512) -> bytes:
        fd = self._require_open()
        chunks: list[bytes] = []
        remaining = max(0, max_bytes)
        while remaining > 0:
            try:
                data = os.read(fd, min(4096, remaining))
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    break
                raise TransportError(f"serial read failed: {exc}") from exc
            if not data:
                break
            chunks.append(data)
            remaining -= len(data)
        return b"".join(chunks)

    def recover(
        self,
        duration_seconds: float,
        quiet_seconds: float,
        deadline: float,
    ) -> bool:
        fd = self._require_open()
        started = time.monotonic()
        last_data = started
        while True:
            now = time.monotonic()
            if now >= deadline:
                return False
            data = self.read_available()
            if data:
                last_data = time.monotonic()
                if self.logger is not None:
                    self.logger.log(
                        "rx_discard",
                        data,
                        transaction_id=getattr(self, "_log_transaction", 0),
                        deadline=deadline,
                        result="discarded",
                    )
            now = time.monotonic()
            if now - started >= duration_seconds and now - last_data >= quiet_seconds:
                return True
            wait = min(quiet_seconds, max(0.0, deadline - now))
            if wait > 0:
                select.select([fd], [], [], wait)

    def close(self) -> None:
        fd = self._fd
        if fd is None:
            return
        self._fd = None
        first_error: OSError | None = None
        if self._original_attributes is not None:
            try:
                termios.tcsetattr(
                    fd,
                    termios.TCSANOW,
                    self._original_attributes,
                )
            except OSError as exc:
                first_error = exc
        try:
            os.close(fd)
        except OSError as exc:
            if first_error is None:
                first_error = exc
        self._original_attributes = None
        if first_error is not None:
            raise TransportError(
                f"cannot restore or close {self.port}: {first_error}"
            ) from first_error

    def _require_open(self) -> int:
        if self._fd is None:
            raise TransportError("serial transport is not open")
        return self._fd

    def _wait_writable(self, timeout: float) -> None:
        fd = self._require_open()
        select.select([], [fd], [], timeout)

    def _wait_readable_fd(self, timeout: float) -> None:
        fd = self._require_open()
        select.select([fd], [], [], timeout)

    def __enter__(self) -> "MacOSTTYTransport":
        self.open()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()
