from __future__ import annotations

import os
import pty
import select
import termios
import threading
import time
import unittest

from tools.modbus_client.protocol import append_crc
from tools.modbus_client.transport import MacOSTTYTransport


def read_exact(fd: int, length: int, timeout: float) -> bytes:
    data = bytearray()
    deadline = time.monotonic() + timeout
    while len(data) < length:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("peer read timed out")
        ready, _, _ = select.select([fd], [], [], remaining)
        if not ready:
            raise TimeoutError("peer read timed out")
        chunk = os.read(fd, length - len(data))
        if chunk:
            data.extend(chunk)
    return bytes(data)


class PtyTransportTests(unittest.TestCase):
    def test_binary_round_trip_and_attributes(self) -> None:
        master, slave = pty.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        transport = MacOSTTYTransport(
            path,
            initial_recovery_seconds=0.01,
            initial_quiet_seconds=0.002,
            initial_timeout_seconds=0.2,
        )
        received: list[bytes] = []
        errors: list[BaseException] = []
        response = append_crc(bytes.fromhex("0104021234"))

        def peer() -> None:
            try:
                request = read_exact(master, 8, 1.0)
                received.append(request)
                os.write(master, response[:2])
                time.sleep(0.001)
                os.write(master, response[2:])
            except BaseException as exc:
                errors.append(exc)

        try:
            transport.open()
            attributes = termios.tcgetattr(transport._fd)
            self.assertEqual(attributes[2] & termios.CSIZE, termios.CS8)
            self.assertTrue(attributes[2] & termios.PARENB)
            self.assertFalse(attributes[2] & termios.PARODD)
            self.assertFalse(attributes[2] & termios.CSTOPB)

            thread = threading.Thread(target=peer)
            thread.start()
            request = append_crc(bytes.fromhex("010400000001"))
            transport.write_all(request, time.monotonic() + 0.5)
            response_bytes = bytearray()
            while len(response_bytes) < len(response):
                chunk = transport.read(
                    len(response) - len(response_bytes),
                    time.monotonic() + 0.5,
                )
                if chunk is None:
                    break
                response_bytes.extend(chunk)
            thread.join(timeout=1.0)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(received, [request])
            self.assertEqual(bytes(response_bytes), response)
        finally:
            transport.close()
            os.close(master)

    def test_timeout_is_bounded(self) -> None:
        master, slave = pty.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        transport = MacOSTTYTransport(
            path,
            initial_recovery_seconds=0.01,
            initial_quiet_seconds=0.002,
            initial_timeout_seconds=0.2,
        )
        received: list[bytes] = []

        def peer() -> None:
            received.append(read_exact(master, 8, 1.0))

        try:
            transport.open()
            thread = threading.Thread(target=peer)
            thread.start()
            request = append_crc(bytes.fromhex("010300000001"))
            transport.write_all(request, time.monotonic() + 0.5)
            started = time.monotonic()
            result = transport.read(7, time.monotonic() + 0.03)
            elapsed = time.monotonic() - started
            self.assertIsNone(result)
            self.assertLess(elapsed, 0.2)
            thread.join(timeout=1.0)
            self.assertEqual(received, [request])
        finally:
            transport.close()
            os.close(master)

    def test_close_restores_and_releases_fd(self) -> None:
        master, slave = pty.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        transport = MacOSTTYTransport(
            path,
            initial_recovery_seconds=0.01,
            initial_quiet_seconds=0.002,
            initial_timeout_seconds=0.2,
        )
        try:
            transport.open()
            fd = transport._fd
            self.assertIsNotNone(fd)
            transport.close()
            self.assertIsNone(transport._fd)
            with self.assertRaises(OSError):
                os.fstat(fd)
        finally:
            transport.close()
            os.close(master)


if __name__ == "__main__":
    unittest.main()
