from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
import struct
import unittest

from tools.modbus_cli import main
from tools.modbus_client.protocol import (
    FUNCTION_READ_INPUT,
    FUNCTION_WRITE_MULTIPLE,
    FUNCTION_WRITE_SINGLE,
    append_crc,
)
from tools.modbus_client.timeparse import (
    MAX_UTC_SECONDS,
    parse_utc_seconds,
    split_u32,
)
from fake_transport import ScriptedTransport


def read_response(request: bytes, values: tuple[int, ...]) -> bytes:
    payload = bytes((request[0], request[1], len(values) * 2))
    payload += b"".join(struct.pack(">H", value) for value in values)
    return append_crc(payload)


def write_response(request: bytes) -> bytes:
    return append_crc(request[:6])


def exception_response(request: bytes, code: int) -> bytes:
    return append_crc(bytes((request[0], request[1] | 0x80, code)))


def identity_values(protocol_version: int) -> tuple[int, ...]:
    return (1, 1, 0, 0, protocol_version)


def observation_values(
    command: int,
    result: int,
    *,
    run_state: int = 0,
) -> tuple[int, ...]:
    return (
        run_state,
        1,
        10,
        1,
        0,
        0,
        0,
        command,
        result,
        0,
        0,
    )


def time_status_values(
    *,
    time_status: int = 1,
    schedule_state: int = 0,
    current_utc: int = 0x12345678,
    armed_start_utc: int = 0xFFFFFFFF,
) -> tuple[int, ...]:
    current = split_u32(current_utc) if current_utc != 0xFFFFFFFF else (0xFFFF, 0xFFFF)
    armed = (
        split_u32(armed_start_utc)
        if armed_start_utc != 0xFFFFFFFF
        else (0xFFFF, 0xFFFF)
    )
    return (
        time_status,
        0,
        0,
        schedule_state,
        current[0],
        current[1],
        armed[0],
        armed[1],
    )


class P3BTimeParsingTests(unittest.TestCase):
    def test_epoch_and_timezone_aware_iso(self) -> None:
        self.assertEqual(parse_utc_seconds("0"), 0)
        self.assertEqual(parse_utc_seconds("epoch:123"), 123)
        self.assertEqual(parse_utc_seconds("0x12345678"), 0x12345678)
        self.assertEqual(
            parse_utc_seconds("2026-09-20T12:00:00Z"),
            parse_utc_seconds("2026-09-20T20:00:00+08:00"),
        )

    def test_day_boundary_and_maximum_legal_value(self) -> None:
        before_midnight = parse_utc_seconds("2026-09-20T23:59:59+08:00")
        after_midnight = parse_utc_seconds("2026-09-21T00:00:00+08:00")
        self.assertEqual(after_midnight - before_midnight, 1)
        self.assertEqual(
            parse_utc_seconds("2106-02-07T06:28:14Z"),
            MAX_UTC_SECONDS,
        )
        self.assertEqual(split_u32(MAX_UTC_SECONDS), (0xFFFF, 0xFFFE))

    def test_rejects_naive_and_invalid_values(self) -> None:
        for value in (
            "2026-09-20T12:00:00",
            "2026-09-20",
            "4294967295",
            "epoch:4294967295",
            "epoch:-1",
            "-1",
            "not-a-time",
        ):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    parse_utc_seconds(value)
        with self.assertRaises(ValueError):
            split_u32(0xFFFFFFFF)


class P3BCliTests(unittest.TestCase):
    def _run(self, argv: list[str], transport: ScriptedTransport):
        def factory(_args, _logger):
            return transport

        stdout = StringIO()
        stderr = StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = main(argv, factory)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_naive_cli_value_is_rejected_before_transport(self) -> None:
        calls = 0

        def factory(_args, _logger):
            nonlocal calls
            calls += 1
            raise AssertionError("factory must not be called")

        with self.assertRaises(SystemExit) as raised:
            with redirect_stderr(StringIO()):
                main(
                    [
                        "--port",
                        "/dev/fake",
                        "time-set",
                        "--utc",
                        "2026-09-20T12:00:00",
                    ],
                    factory,
                )
        self.assertEqual(raised.exception.code, 2)
        self.assertEqual(calls, 0)

    def test_protocol_1_does_not_send_new_commands(self) -> None:
        cases = (
            ["time-set", "--utc", "123"],
            ["time-status"],
            ["schedule", "--utc", "123"],
        )
        for command in cases:
            with self.subTest(command=command):
                transport = ScriptedTransport(
                    lambda request: read_response(request, identity_values(1))
                )
                code, stdout, stderr = self._run(
                    ["--port", "/dev/fake", "--json", *command],
                    transport,
                )
                self.assertEqual(code, 7)
                payload = json.loads(stdout)
                self.assertEqual(payload["error"]["kind"], "unsupported_protocol")
                self.assertIn("不支持协议 2/3 时间功能", payload["error"]["message"])
                self.assertEqual(stderr, "")
                self.assertEqual(len(transport.writes), 1)
                self.assertEqual(transport.writes[0][1], FUNCTION_READ_INPUT)

    def test_protocol_2_time_set_sequence_and_words(self) -> None:
        def handler(request: bytes) -> bytes:
            function = request[1]
            start = struct.unpack_from(">H", request, 2)[0]
            if function == FUNCTION_READ_INPUT and start == 0x0000:
                return read_response(request, identity_values(2))
            if function == FUNCTION_WRITE_MULTIPLE and start == 0x0020:
                return write_response(request)
            if function == FUNCTION_WRITE_SINGLE and start == 0x0040:
                return write_response(request)
            if function == FUNCTION_READ_INPUT and start == 0x0005:
                return read_response(request, observation_values(6, 1))
            if function == FUNCTION_READ_INPUT and start == 0x0016:
                return read_response(
                    request,
                    time_status_values(
                        current_utc=0x12345678,
                        armed_start_utc=0xFFFFFFFF,
                    ),
                )
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            [
                "--port",
                "/dev/fake",
                "--json",
                "time-set",
                "--utc",
                "epoch:305419896",
            ],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        payload = json.loads(stdout)
        self.assertEqual(payload["result"]["requested_utc_seconds"], 0x12345678)
        self.assertEqual(payload["result"]["pending_words"], [0x1234, 0x5678])
        self.assertEqual(
            payload["result"]["command"]["last_command"]["name"], "SET_TIME"
        )
        self.assertEqual(
            payload["result"]["time_status"]["current_utc_seconds"], 0x12345678
        )
        self.assertEqual(len(transport.writes), 5)
        self.assertEqual(transport.writes[1][1], FUNCTION_WRITE_MULTIPLE)
        self.assertEqual(transport.writes[1][2:4], b"\x00\x20")
        self.assertEqual(transport.writes[1][4:6], b"\x00\x02")
        self.assertEqual(transport.writes[2][4:6], b"\x00\x06")

    def test_protocol_2_schedule_sequence(self) -> None:
        target = parse_utc_seconds("2026-09-20T20:00:00+08:00")

        def handler(request: bytes) -> bytes:
            function = request[1]
            start = struct.unpack_from(">H", request, 2)[0]
            if function == FUNCTION_READ_INPUT and start == 0x0000:
                return read_response(request, identity_values(2))
            if function == FUNCTION_WRITE_MULTIPLE and start == 0x0022:
                return write_response(request)
            if function == FUNCTION_WRITE_SINGLE and start == 0x0040:
                return write_response(request)
            if function == FUNCTION_READ_INPUT and start == 0x0005:
                return read_response(request, observation_values(7, 1))
            if function == FUNCTION_READ_INPUT and start == 0x0016:
                return read_response(
                    request,
                    time_status_values(
                        schedule_state=1,
                        armed_start_utc=target,
                    ),
                )
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            [
                "--port",
                "/dev/fake",
                "--json",
                "schedule",
                "--utc",
                "2026-09-20T20:00:00+08:00",
            ],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        payload = json.loads(stdout)
        self.assertEqual(
            payload["result"]["command"]["last_command"]["name"], "ARM_START"
        )
        self.assertEqual(
            payload["result"]["time_status"]["schedule_state_name"], "ARMED"
        )
        self.assertEqual(
            transport.writes[1][2:4],
            b"\x00\x22",
        )
        expected_words = split_u32(target)
        self.assertEqual(
            struct.unpack(">HH", transport.writes[1][7:11]),
            expected_words,
        )

    def test_time_status_protocol_2_decodes_sentinel(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            if calls == 1:
                return read_response(request, identity_values(2))
            return read_response(
                request,
                time_status_values(
                    time_status=0,
                    current_utc=0xFFFFFFFF,
                    armed_start_utc=0xFFFFFFFF,
                ),
            )

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "time-status"],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        result = json.loads(stdout)["result"]
        self.assertEqual(result["time_status_name"], "UNCALIBRATED")
        self.assertIsNone(result["current_utc_seconds"])
        self.assertFalse(result["current_utc_valid"])
        self.assertIsNone(result["armed_start_utc_seconds"])
        self.assertTrue(
            all(request[1] == FUNCTION_READ_INPUT for request in transport.writes)
        )

    def test_set_time_armed_rejection_keeps_observations_and_no_retry(self) -> None:
        def handler(request: bytes) -> bytes:
            function = request[1]
            start = struct.unpack_from(">H", request, 2)[0]
            if function == FUNCTION_READ_INPUT and start == 0x0000:
                return read_response(request, identity_values(2))
            if function == FUNCTION_WRITE_MULTIPLE:
                return write_response(request)
            if function == FUNCTION_WRITE_SINGLE and start == 0x0040:
                return exception_response(request, 0x04)
            if function == FUNCTION_READ_INPUT and start == 0x0005:
                return read_response(request, observation_values(6, 4))
            if function == FUNCTION_READ_INPUT and start == 0x0016:
                return read_response(
                    request,
                    time_status_values(
                        schedule_state=1,
                        armed_start_utc=0x12345678,
                    ),
                )
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "time-set", "--utc", "123"],
            transport,
        )
        self.assertEqual(code, 5)
        payload = json.loads(stdout)
        self.assertEqual(payload["error"]["exception_code"], 0x04)
        self.assertEqual(
            payload["error"]["observation"]["last_command"]["result"],
            "REJECTED",
        )
        self.assertEqual(
            payload["error"]["time_status"]["schedule_state_name"], "ARMED"
        )
        self.assertEqual(len(transport.writes), 5)

    def test_schedule_invalid_target_reports_0x03(self) -> None:
        def handler(request: bytes) -> bytes:
            function = request[1]
            start = struct.unpack_from(">H", request, 2)[0]
            if function == FUNCTION_READ_INPUT and start == 0x0000:
                return read_response(request, identity_values(2))
            if function == FUNCTION_WRITE_MULTIPLE:
                return write_response(request)
            if function == FUNCTION_WRITE_SINGLE and start == 0x0040:
                return exception_response(request, 0x03)
            if function == FUNCTION_READ_INPUT and start == 0x0005:
                return read_response(request, observation_values(7, 4))
            if function == FUNCTION_READ_INPUT and start == 0x0016:
                return read_response(request, time_status_values())
            raise AssertionError(f"unexpected request {request.hex(' ')}")

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "schedule", "--utc", "1"],
            transport,
        )
        self.assertEqual(code, 5)
        payload = json.loads(stdout)
        self.assertEqual(payload["error"]["exception_code"], 0x03)
        self.assertEqual(len(transport.writes), 5)

    def test_time_status_text_output(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            if calls == 1:
                return read_response(request, identity_values(2))
            return read_response(request, time_status_values(schedule_state=1))

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "time-status"],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        self.assertIn("time-status:", stdout)
        self.assertIn("schedule_state=ARMED", stdout)


if __name__ == "__main__":
    unittest.main()
