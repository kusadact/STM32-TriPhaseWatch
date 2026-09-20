from __future__ import annotations

from collections import deque
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
import os
import pty
import struct
import subprocess
import threading
import time
import unittest

from tools.modbus_cli import main
from tools.modbus_client.client import ModbusClient
from tools.modbus_client.errors import (
    ModbusException,
    TransactionTimeout,
    UnsupportedProtocolError,
)
from tools.modbus_client.protocol import (
    FUNCTION_READ_HOLDING,
    FUNCTION_READ_INPUT,
    build_read_request,
    build_write_single_request,
)
from tools.modbus_client.service import ModbusService
from tools.modbus_client.transport import MacOSTTYTransport


class CoreAdapterTransport:
    """Binary transport for tests/host/mac_cli/board_a_core_adapter.c."""

    def __init__(self, executable: str) -> None:
        self.executable = executable
        self.process: subprocess.Popen[bytes] | None = None
        self.pending: deque[bytes] = deque()
        self.writes: list[bytes] = []

    @property
    def description(self) -> str:
        return "real-c-core-adapter"

    def open(self) -> None:
        if self.process is not None and self.process.poll() is None:
            return
        self.process = subprocess.Popen(
            [self.executable],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def _require_process(self) -> subprocess.Popen[bytes]:
        if self.process is None:
            raise RuntimeError("adapter is not open")
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
        assert process.stdin is not None
        process.stdin.write(b"\x01" + struct.pack(">H", len(data)) + data)
        process.stdin.flush()
        status = self._read_exact(1)
        if status != b"\x00":
            raise RuntimeError(f"adapter status {status!r}")
        response_length = struct.unpack(">H", self._read_exact(2))[0]
        response = self._read_exact(response_length) if response_length else b""
        self.pending.append(response)
        self.writes.append(data)
        return [data]

    def read(self, max_bytes: int, deadline: float) -> bytes | None:
        if not self.pending:
            return None
        chunk = self.pending.popleft()
        if len(chunk) > max_bytes:
            self.pending.appendleft(chunk[max_bytes:])
            return chunk[:max_bytes]
        return chunk

    def read_available(self, max_bytes: int = 512) -> bytes:
        return b""

    def recover(
        self,
        duration_seconds: float,
        quiet_seconds: float,
        deadline: float,
    ) -> bool:
        return True

    def tick(self, now_us: int) -> None:
        process = self._require_process()
        assert process.stdin is not None
        process.stdin.write(b"\x02" + struct.pack(">Q", now_us))
        process.stdin.flush()
        self._read_exact(1)

    def reset(self, session_id: int = 1) -> None:
        process = self._require_process()
        assert process.stdin is not None
        process.stdin.write(b"\x03" + struct.pack(">I", session_id))
        process.stdin.flush()
        self._read_exact(1)

    def crc(self, data: bytes) -> int:
        process = self._require_process()
        assert process.stdin is not None
        process.stdin.write(b"\x04" + struct.pack(">H", len(data)) + data)
        process.stdin.flush()
        return struct.unpack(">H", self._read_exact(2))[0]

    def close(self) -> None:
        process = self.process
        self.process = None
        if process is None:
            return
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
            process.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=1.0)
        if process.returncode != 0:
            stderr = b""
            if process.stderr is not None:
                stderr = process.stderr.read()
            raise RuntimeError(
                f"adapter exited {process.returncode}: {stderr.decode(errors='replace')}"
            )
        if process.stdout is not None:
            process.stdout.close()
        if process.stderr is not None:
            process.stderr.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


class RealCCoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        executable = os.environ.get("MODBUS_TEST_CORE_ADAPTER")
        if not executable:
            raise unittest.SkipTest("MODBUS_TEST_CORE_ADAPTER is not set")
        if not os.path.isfile(executable):
            raise RuntimeError(f"adapter does not exist: {executable}")
        cls.executable = executable

    def setUp(self) -> None:
        self.transport = CoreAdapterTransport(self.executable)
        self.transport.open()
        self.client = ModbusClient(
            self.transport,
            timeout_seconds=0.1,
            recovery_seconds=0.001,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.01,
        )
        self.client.open()
        self.service = ModbusService(self.client)

    def tearDown(self) -> None:
        self.client.close()

    def test_c06_config_staging_apply_and_active_version(self) -> None:
        self.service.config(10, 3, 0)
        before = self.service.status()
        self.assertEqual(before["active_config"]["channel_mask"], 1)
        self.assertEqual(before["active_config"]["version"], 0)
        self.service.apply()
        after = self.service.status()
        self.assertEqual(after["active_config"]["channel_mask"], 3)
        self.assertEqual(after["active_config"]["version"], 1)
        self.assertEqual(after["last_command"]["name"], "APPLY_CONFIG")
        self.assertEqual(after["last_command"]["result"], "ACCEPTED")

    def test_python_and_c_crc_standard_vector_match(self) -> None:
        self.assertEqual(self.transport.crc(b"123456789"), 0x4B37)

    def test_p3b_time_commands_reject_real_protocol_1_without_new_writes(self) -> None:
        with self.assertRaises(UnsupportedProtocolError):
            self.service.time_set(123)
        self.assertEqual(len(self.transport.writes), 1)
        self.assertEqual(self.transport.writes[0][1], FUNCTION_READ_INPUT)

    def test_c07_single_dedup_window_and_eviction(self) -> None:
        first = self.service.single(1)
        self.assertFalse(first["duplicate"])
        self.assertEqual(first["snapshot"]["sequence"], 1)
        duplicate = self.service.single(1)
        self.assertTrue(duplicate["duplicate"])
        self.assertEqual(duplicate["snapshot"]["sequence"], 1)
        for command_id in range(2, 17):
            self.service.single(command_id)
        seventeenth = self.service.single(17)
        self.assertFalse(seventeenth["duplicate"])
        self.assertEqual(seventeenth["snapshot"]["sequence"], 17)
        reused = self.service.single(1)
        self.assertFalse(reused["duplicate"])
        self.assertEqual(reused["snapshot"]["sequence"], 18)

    def test_c08_start_stop_repeat_and_finite_count(self) -> None:
        self.service.config(10, 1, 3)
        self.service.apply()
        self.service.start()
        self.service.start()
        self.transport.tick(1_000_000)
        running = self.service.status()
        self.assertEqual(running["run_state"], "RUNNING")
        self.assertEqual(running["records_this_run"], 1)
        self.transport.tick(11_000_000)
        self.transport.tick(21_000_000)
        stopped = self.service.status()
        self.assertEqual(stopped["run_state"], "STOPPED")
        self.assertEqual(stopped["records_this_run"], 3)
        snapshot = self.service.snapshot()
        self.assertEqual(snapshot["sequence"], 3)
        self.service.stop()
        self.service.stop()
        self.assertEqual(self.service.status()["run_state"], "STOPPED")

    def test_c09_save_returns_exception_4_and_increments_error_count(self) -> None:
        with self.assertRaises(ModbusException) as raised:
            self.service.save()
        self.assertEqual(raised.exception.exception_code, 4)
        self.assertEqual(
            raised.exception.observation["last_command"]["result"],
            "UNSUPPORTED",
        )
        status = self.service.status()
        self.assertEqual(status["persistence_status"], 0)
        self.assertEqual(status["last_command"]["name"], "SAVE_CONFIG")
        self.assertEqual(status["last_command"]["result"], "UNSUPPORTED")
        stats = self.service.stats()
        self.assertEqual(stats["persistence_errors"], 1)

    def test_c10_invalid_addresses_and_atomic_config_write(self) -> None:
        with self.assertRaises(ModbusException) as raised:
            self.service.read_holding(0x0003, 1)
        self.assertEqual(raised.exception.exception_code, 2)
        with self.assertRaises(ModbusException) as raised:
            self.service.read_holding(0x0002, 2)
        self.assertEqual(raised.exception.exception_code, 2)

        self.service.write_multiple(0, [30, 15, 7])
        with self.assertRaises(ModbusException) as raised:
            self.service.write_multiple(0, [9, 15, 7])
        self.assertEqual(raised.exception.exception_code, 3)
        self.assertEqual(self.service.read_holding(0, 3), (30, 15, 7))

    def test_c11_host_core_no_response_cases(self) -> None:
        bad_crc = bytearray(build_read_request(1, FUNCTION_READ_HOLDING, 0, 1))
        bad_crc[-1] ^= 1
        with self.assertRaises(TransactionTimeout):
            self.client.transaction(bytes(bad_crc))
        wrong_address = build_read_request(2, FUNCTION_READ_HOLDING, 0, 1)
        with self.assertRaises(TransactionTimeout):
            self.client.transaction(wrong_address)
        broadcast_read = build_read_request(0, FUNCTION_READ_HOLDING, 0, 1)
        with self.assertRaises(TransactionTimeout):
            self.client.transaction(broadcast_read)
        broadcast_write = build_write_single_request(0, 0, 25)
        with self.assertRaises(TransactionTimeout):
            self.client.transaction(broadcast_write)
        self.assertEqual(self.service.read_holding(0, 1), (25,))

    def test_c13_test_channel_masks_and_sequence_values(self) -> None:
        for mask in (1, 3, 15):
            self.transport.reset(session_id=1)
            self.service.config(10, mask, 0)
            self.service.apply()
            snapshot = self.service.single(100 + mask)["snapshot"]
            self.assertEqual(snapshot["sequence"], 1)
            for channel in snapshot["channels"]:
                if mask & (1 << channel["index"]):
                    self.assertEqual(channel["value"], 10 + channel["index"])
                    self.assertEqual(channel["quality"], "TEST_VALID")
                else:
                    self.assertEqual(channel["value"], 0)
                    self.assertEqual(channel["quality"], "UNAVAILABLE")

    def test_cli_main_drives_pty_and_real_c_core(self) -> None:
        master, slave = pty.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        backend = CoreAdapterTransport(self.executable)
        backend.open()
        stopped = threading.Event()
        errors: list[BaseException] = []

        def read_pty_exact(length: int) -> bytes:
            data = bytearray()
            while len(data) < length:
                chunk = os.read(master, length - len(data))
                if chunk:
                    data.extend(chunk)
            return bytes(data)

        def read_pty_request() -> bytes:
            prefix = read_pty_exact(2)
            function = prefix[1]
            if function in (0x03, 0x04, 0x06):
                return prefix + read_pty_exact(6)
            if function == 0x10:
                header = read_pty_exact(5)
                byte_count = header[-1]
                return prefix + header + read_pty_exact(byte_count + 2)
            raise AssertionError(f"unexpected function 0x{function:02X}")

        def peer() -> None:
            try:
                while not stopped.is_set():
                    request = read_pty_request()
                    backend.write_all(request, time.monotonic() + 1.0)
                    response = backend.read(256, time.monotonic() + 1.0)
                    if response is None:
                        raise AssertionError("C adapter returned no response")
                    os.write(master, response)
            except BaseException as exc:
                if not stopped.is_set():
                    errors.append(exc)

        def factory(args, logger):
            return MacOSTTYTransport(
                path,
                initial_recovery_seconds=0.01,
                initial_quiet_seconds=0.002,
                initial_timeout_seconds=0.2,
                logger=logger,
            )

        thread = threading.Thread(target=peer)
        thread.start()
        stdout = StringIO()
        stderr = StringIO()
        try:
            with redirect_stdout(stdout), redirect_stderr(stderr):
                code = main(
                    [
                        "--port",
                        path,
                        "--json",
                        "config",
                        "--period",
                        "10",
                        "--channels",
                        "3",
                        "--count",
                        "0",
                    ],
                    factory,
                )
            self.assertEqual(code, 0, stderr.getvalue())
            self.assertEqual(json.loads(stdout.getvalue())["ok"], True)
        finally:
            stopped.set()
            try:
                os.close(master)
            except OSError:
                pass
            thread.join(timeout=1.0)
            backend.close()
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
