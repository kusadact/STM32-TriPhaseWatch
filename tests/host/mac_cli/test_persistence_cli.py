from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
import struct
import unittest

from tools.modbus_cli import main
from tools.modbus_client.client import ModbusClient
from tools.modbus_client.service import ModbusService
from tools.modbus_client.protocol import (
    FUNCTION_READ_INPUT,
    FUNCTION_WRITE_MULTIPLE,
    FUNCTION_WRITE_SINGLE,
    append_crc,
)
from fake_transport import ScriptedTransport


def read_response(request: bytes, values: tuple[int, ...]) -> bytes:
    payload = bytes((request[0], request[1], len(values) * 2))
    payload += b"".join(struct.pack(">H", value) for value in values)
    return append_crc(payload)


def write_response(request: bytes) -> bytes:
    return append_crc(request[:6])


def identity_values(protocol_version: int = 3) -> tuple[int, ...]:
    return (1, 1, 0, 0, protocol_version)


def observation_values(command: int, result: int = 1) -> tuple[int, ...]:
    return (0, 1, 10, 1, 0, 0, 0, command, result, 0, 0)


def persistence_values(
    *,
    save_state: int = 0,
    save_command_id: int = 0,
    save_error: int = 0,
    storage_state: int = 1,
    drain_state: int = 0,
    drain_generation: int = 0,
    captured_period: int = 0,
    captured_mask: int = 0,
    captured_count: int = 0,
    generated: int = 0,
    synced: int = 0,
    queued: int = 0,
) -> tuple[int, ...]:
    values = [0] * 48
    values[0] = 1
    values[1] = save_state
    values[2] = save_command_id >> 16
    values[3] = save_command_id & 0xFFFF
    values[6] = save_error
    values[7] = 1
    values[8] = storage_state
    values[10] = queued
    values[12] = generated >> 16
    values[13] = generated & 0xFFFF
    values[14] = synced >> 16
    values[15] = synced & 0xFFFF
    values[0x15] = drain_state
    values[0x1E] = drain_generation >> 16
    values[0x1F] = drain_generation & 0xFFFF
    values[0x24] = captured_period >> 16
    values[0x25] = captured_period & 0xFFFF
    values[0x26] = captured_mask
    values[0x27] = captured_count
    return tuple(values)


class PersistenceCliTests(unittest.TestCase):
    def _run(self, argv: list[str], transport: ScriptedTransport):
        def factory(_args, _logger):
            return transport

        stdout = StringIO()
        stderr = StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = main(argv, factory)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_future_protocol_is_not_sent_non_idempotent_writes(self) -> None:
        for command in (["save"], ["stop"]):
            with self.subTest(command=command):
                transport = ScriptedTransport(
                    lambda request: read_response(
                        request,
                        identity_values(protocol_version=4),
                    )
                )
                code, stdout, _ = self._run(
                    ["--port", "/dev/fake", "--json", *command],
                    transport,
                )
                self.assertEqual(code, 7)
                self.assertEqual(
                    json.loads(stdout)["error"]["kind"],
                    "unsupported_protocol",
                )
                self.assertEqual(len(transport.writes), 1)
                self.assertEqual(transport.writes[0][1], FUNCTION_READ_INPUT)

    def test_storage_status_decodes_full_block(self) -> None:
        values = persistence_values(
            storage_state=4,
            drain_state=3,
            drain_generation=0x12345678,
            generated=40,
            synced=31,
            queued=8,
        )
        transport = ScriptedTransport(
            lambda request: read_response(request, values)
        )
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "storage-status"],
            transport,
        )
        self.assertEqual(code, 0, stdout or stderr)
        result = json.loads(stdout)["result"]
        self.assertEqual(result["contract_revision"], 1)
        self.assertEqual(result["storage_state_name"], "IO_ERROR")
        self.assertEqual(result["drain_state_name"], "FAILED")
        self.assertEqual(result["drain_generation"], 0x12345678)
        self.assertEqual(result["generated"], 40)
        self.assertEqual(result["synced"], 31)
        self.assertEqual(result["queued"], 8)

    def test_storage_status_decodes_u32_captured_period_words(self) -> None:
        values = persistence_values(
            captured_period=0x12345678,
            captured_mask=0x000A,
            captured_count=0x0102,
        )
        transport = ScriptedTransport(
            lambda request: read_response(request, values)
        )
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "storage-status"],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        result = json.loads(stdout)["result"]
        self.assertEqual(result["captured_period_s"], 0x12345678)
        self.assertEqual(result["captured_mask"], 0x000A)
        self.assertEqual(result["captured_count"], 0x0102)

    def test_save_waits_for_matching_terminal_success(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            start = struct.unpack_from(">H", request, 2)[0]
            if request[1] == FUNCTION_READ_INPUT and start == 0:
                return read_response(request, identity_values())
            if request[1] == FUNCTION_WRITE_MULTIPLE and start == 0x40:
                self.assertEqual(
                    struct.unpack_from(">HHH", request, 7),
                    (2, 0, 42),
                )
                return write_response(request)
            if request[1] == FUNCTION_READ_INPUT and start == 0x80:
                return read_response(
                    request,
                    persistence_values(
                        save_state=1 if calls < 4 else 2,
                        save_command_id=42,
                        captured_period=20,
                        captured_mask=3,
                        captured_count=4,
                    ),
                )
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            [
                "--port",
                "/dev/fake",
                "--json",
                "save",
                "--id",
                "42",
                "--wait-timeout",
                "1",
            ],
            transport,
        )
        self.assertEqual(code, 0, stdout or stderr)
        result = json.loads(stdout)["result"]
        self.assertTrue(result["accepted"])
        self.assertTrue(result["completed"])
        self.assertEqual(result["command_id"], 42)
        self.assertEqual(result["save_state_name"], "SUCCESS")
        self.assertEqual(result["captured_config"]["period_sec"], 20)
        self.assertEqual(result["captured_config"]["channel_mask"], 3)

    def test_save_timeout_is_nonzero_and_inconclusive(self) -> None:
        def handler(request: bytes) -> bytes:
            start = struct.unpack_from(">H", request, 2)[0]
            if request[1] == FUNCTION_READ_INPUT and start == 0:
                return read_response(request, identity_values())
            if request[1] == FUNCTION_WRITE_MULTIPLE and start == 0x40:
                return write_response(request)
            if request[1] == FUNCTION_READ_INPUT and start == 0x80:
                return read_response(
                    request,
                    persistence_values(
                        save_state=1,
                        save_command_id=77,
                    ),
                )
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        code, stdout, _ = self._run(
            [
                "--port",
                "/dev/fake",
                "--json",
                "save",
                "--id",
                "77",
                "--wait-timeout",
                "0.02",
            ],
            transport,
        )
        self.assertEqual(code, 7)
        payload = json.loads(stdout)
        self.assertEqual(payload["error"]["kind"], "state_mismatch")
        self.assertTrue(payload["result"]["save_result"]["inconclusive"])
        self.assertFalse(payload["result"]["save_result"]["completed"])

    def test_save_write_timeout_can_be_resolved_by_matching_status(self) -> None:
        first_write = True

        def handler(request: bytes) -> bytes:
            nonlocal first_write
            start = struct.unpack_from(">H", request, 2)[0]
            if request[1] == FUNCTION_READ_INPUT and start == 0:
                return read_response(request, identity_values())
            if request[1] == FUNCTION_WRITE_MULTIPLE and start == 0x40:
                if first_write:
                    first_write = False
                    return None
                return write_response(request)
            if request[1] == FUNCTION_READ_INPUT and start == 0x80:
                return read_response(
                    request,
                    persistence_values(
                        save_state=2,
                        save_command_id=99,
                        captured_period=10,
                        captured_mask=1,
                    ),
                )
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        client = ModbusClient(
            transport,
            timeout_seconds=0.01,
            recovery_seconds=0.001,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.01,
        )
        client.open()
        result = ModbusService(client).save(
            wait_timeout=1.0,
            command_id=99,
        )
        client.close()
        self.assertTrue(result["completed"])
        self.assertEqual(result["save_state_name"], "SUCCESS")

    def test_save_failed_state_returns_nonzero(self) -> None:
        def handler(request: bytes) -> bytes:
            start = struct.unpack_from(">H", request, 2)[0]
            if request[1] == FUNCTION_READ_INPUT and start == 0:
                return read_response(request, identity_values())
            if request[1] == FUNCTION_WRITE_MULTIPLE and start == 0x40:
                return write_response(request)
            if request[1] == FUNCTION_READ_INPUT and start == 0x80:
                return read_response(
                    request,
                    persistence_values(
                        save_state=3,
                        save_command_id=88,
                        save_error=3,
                    ),
                )
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        code, stdout, _ = self._run(
            [
                "--port",
                "/dev/fake",
                "--json",
                "save",
                "--id",
                "88",
            ],
            transport,
        )
        self.assertEqual(code, 7)
        save_result = json.loads(stdout)["result"]["save_result"]
        self.assertTrue(save_result["accepted"])
        self.assertTrue(save_result["completed"])
        self.assertEqual(save_result["save_error_name"], "IO")

    def test_stop_waits_for_drain_done(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            start = struct.unpack_from(">H", request, 2)[0]
            if request[1] == FUNCTION_READ_INPUT and start == 0:
                return read_response(request, identity_values())
            if request[1] == FUNCTION_READ_INPUT and start == 0x80:
                return read_response(
                    request,
                    persistence_values(
                        drain_state=0 if calls == 2 else 2,
                        drain_generation=5 if calls == 2 else 6,
                    ),
                )
            if request[1] == FUNCTION_WRITE_SINGLE and start == 0x40:
                return write_response(request)
            if request[1] == FUNCTION_READ_INPUT and start == 0x05:
                return read_response(request, observation_values(4))
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            [
                "--port",
                "/dev/fake",
                "--json",
                "stop",
                "--wait-timeout",
                "1",
            ],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        result = json.loads(stdout)["result"]
        self.assertTrue(result["drain_waited"])
        self.assertTrue(result["safe_to_remove"])
        self.assertEqual(result["persistence_status"]["drain_state_name"], "DONE")

    def test_stop_no_wait_never_claims_safe_removal(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            start = struct.unpack_from(">H", request, 2)[0]
            if request[1] == FUNCTION_READ_INPUT and start == 0:
                return read_response(request, identity_values())
            if request[1] == FUNCTION_READ_INPUT and start == 0x80:
                return read_response(
                    request,
                    persistence_values(drain_generation=2),
                )
            if request[1] == FUNCTION_WRITE_SINGLE and start == 0x40:
                return write_response(request)
            if request[1] == FUNCTION_READ_INPUT and start == 0x05:
                return read_response(request, observation_values(4))
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "stop", "--no-wait"],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        result = json.loads(stdout)["result"]
        self.assertFalse(result["drain_waited"])
        self.assertFalse(result["safe_to_remove"])
        self.assertEqual(calls, 4)


if __name__ == "__main__":
    unittest.main()
