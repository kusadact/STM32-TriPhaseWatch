"""Deterministic transport doubles used by unit and CLI tests."""

from __future__ import annotations

from collections import deque
from collections.abc import Callable
import time


class ScriptedTransport:
    def __init__(
        self,
        handler: Callable[[bytes], bytes | list[bytes] | None],
        *,
        write_fragments: list[int] | None = None,
        recover_result: bool = True,
    ) -> None:
        self.handler = handler
        self.write_fragments = write_fragments
        self.recover_result = recover_result
        self.opened = False
        self.closed = False
        self.writes: list[bytes] = []
        self.recoveries = 0
        self.read_deadlines: list[float] = []
        self._chunks: deque[bytes] = deque()
        self._extra = bytearray()

    def queue_extra(self, data: bytes) -> None:
        self._extra.extend(data)

    @property
    def description(self) -> str:
        return "fake"

    def open(self) -> None:
        self.opened = True
        self.closed = False

    def write_all(self, data: bytes, deadline: float) -> list[bytes]:
        if not self.opened or self.closed:
            raise RuntimeError("fake transport is not open")
        fragments: list[bytes] = []
        offset = 0
        if self.write_fragments:
            for size in self.write_fragments:
                if offset >= len(data):
                    break
                fragment = data[offset : offset + size]
                fragments.append(fragment)
                self.writes.append(fragment)
                offset += len(fragment)
        if offset < len(data):
            fragment = data[offset:]
            fragments.append(fragment)
            self.writes.append(fragment)

        response = self.handler(data)
        if isinstance(response, list):
            self._chunks.extend(response)
        elif response is not None:
            self._chunks.append(response)
        return fragments

    def read(self, max_bytes: int, deadline: float) -> bytes | None:
        self.read_deadlines.append(deadline)
        if self._chunks:
            chunk = self._chunks.popleft()
            if len(chunk) > max_bytes:
                self._chunks.appendleft(chunk[max_bytes:])
                return chunk[:max_bytes]
            return chunk
        if time.monotonic() >= deadline:
            return None
        return None

    def read_available(self, max_bytes: int = 512) -> bytes:
        data = b"".join(self._chunks)
        self._chunks.clear()
        data += bytes(self._extra)
        self._extra.clear()
        return data[:max_bytes]

    def recover(
        self,
        duration_seconds: float,
        quiet_seconds: float,
        deadline: float,
    ) -> bool:
        self.recoveries += 1
        return self.recover_result

    def close(self) -> None:
        self.closed = True
