from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
import os
import tempfile
import unittest

from tools.modbus_cli import main
from tools.modbus_client.protocol import (
    FUNCTION_WRITE_SINGLE,
    append_crc,
)
from tools.modbus_client.errors import TransportError
from fake_transport import ScriptedTransport


def read_response(request: bytes, values: tuple[int, ...]) -> bytes:
    payload = bytes((request[0], request[1], len(values) * 2))
    payload += b"".join(value.to_bytes(2, "big") for value in values)
    return append_crc(payload)


def write_response(request: bytes) -> bytes:
    return append_crc(request[:6])


def exception_response(request: bytes, code: int) -> bytes:
    return append_crc(bytes((request[0], request[1] | 0x80, code)))


def observation_values(
    *,
    run_state: int = 0,
    period: int = 10,
    mask: int = 1,
    record_count: int = 0,
    version: int = 0,
    command: int = 0,
    result: int = 0,
    command_id: int = 0,
) -> tuple[int, ...]:
    return (
        run_state,
        1,
        period,
        mask,
        record_count,
        (version >> 16) & 0xFFFF,
        version & 0xFFFF,
        command,
        result,
        (command_id >> 16) & 0xFFFF,
        command_id & 0xFFFF,
    )


def snapshot_values(sequence: int = 1) -> tuple[int, ...]:
    return (
        1,
        (sequence >> 16) & 0xFFFF,
        sequence & 0xFFFF,
        1,
        1,
        (sequence * 10) & 0xFFFF,
        0,
        0,
        0,
        1,
        0,
        0,
        0,
        2,
        1,
    )


class CliTests(unittest.TestCase):
    def _run(self, argv: list[str], transport: ScriptedTransport):
        def factory(_args, _logger):
            return transport

        stdout = StringIO()
        stderr = StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = main(argv, factory)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_help_does_not_open_transport(self) -> None:
        calls = 0

        def factory(_args, _logger):
            nonlocal calls
            calls += 1
            raise AssertionError("factory must not be called")

        with self.assertRaises(SystemExit) as raised:
            with redirect_stdout(StringIO()):
                main(["--help"], factory)
        self.assertEqual(raised.exception.code, 0)
        self.assertEqual(calls, 0)

    def test_identity_json(self) -> None:
        transport = ScriptedTransport(
            lambda request: read_response(request, (1, 1, 0, 0, 1))
        )
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "identity"],
            transport,
        )
        self.assertEqual(code, 0)
        self.assertEqual(stderr, "")
        payload = json.loads(stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["result"]["reported_version"], "1.0.0")

    def test_frame_log_is_exclusive_jsonl(self) -> None:
        transport = ScriptedTransport(
            lambda request: read_response(request, (1, 1, 0, 0, 1))
        )
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "frames.jsonl")
            code, stdout, stderr = self._run(
                ["--port", "/dev/fake", "--json", "--frame-log", path, "identity"],
                transport,
            )
            self.assertEqual(code, 0, stderr)
            self.assertTrue(os.path.exists(path))
            with open(path, encoding="utf-8") as stream:
                events = [json.loads(line) for line in stream]
            self.assertEqual(events[0]["direction"], "tx")
            self.assertEqual(events[1]["direction"], "rx")

            code, stdout, stderr = self._run(
                ["--port", "/dev/fake", "--json", "--frame-log", path, "identity"],
                ScriptedTransport(
                    lambda request: read_response(request, (1, 1, 0, 0, 1))
                ),
            )
            self.assertEqual(code, 3)
            self.assertIn("frame_log", json.loads(stdout)["error"]["kind"])

    def test_config_readback_mismatch_is_exit_7(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            if calls == 1:
                return write_response(request)
            return read_response(request, (10, 2, 0))

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            [
                "--port",
                "/dev/fake",
                "--json",
                "config",
                "--period",
                "10",
                "--channels",
                "1",
                "--count",
                "0",
            ],
            transport,
        )
        self.assertEqual(code, 7)
        payload = json.loads(stdout)
        self.assertEqual(payload["error"]["kind"], "state_mismatch")
        self.assertEqual(stderr, "")

    def test_save_reports_exception_4_and_observation(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            if calls == 1:
                return exception_response(request, 4)
            return read_response(
                request,
                observation_values(command=2, result=3),
            )

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "save"],
            transport,
        )
        self.assertEqual(code, 5)
        payload = json.loads(stdout)
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["error"]["exception_code"], 4)
        self.assertEqual(
            payload["error"]["observation"]["last_command"]["result"],
            "UNSUPPORTED",
        )

    def test_start_with_finite_one_may_be_stopped_on_observation(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            if calls == 1:
                return write_response(request)
            return read_response(
                request,
                observation_values(run_state=0, command=3, result=1),
            )

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "start"],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        payload = json.loads(stdout)
        self.assertEqual(payload["result"]["last_command"]["result"], "ACCEPTED")

    def test_address_overflow_is_rejected_before_transport(self) -> None:
        calls = 0

        def factory(_args, _logger):
            nonlocal calls
            calls += 1
            raise AssertionError("factory must not be called")

        stdout = StringIO()
        with redirect_stdout(stdout):
            code = main(
                [
                    "--port",
                    "/dev/fake",
                    "--json",
                    "read-holding",
                    "--start",
                    "65535",
                    "--count",
                    "2",
                ],
                factory,
            )
        self.assertEqual(code, 2)
        self.assertEqual(calls, 0)
        self.assertEqual(json.loads(stdout.getvalue())["error"]["kind"], "argument")

    def test_raw_and_business_command_dispatch(self) -> None:
        cases = [
            (
                ["status"],
                lambda request: read_response(request, tuple(range(25))),
            ),
            (
                ["snapshot"],
                lambda request: read_response(request, snapshot_values(3)),
            ),
            (
                ["stats"],
                lambda request: read_response(request, tuple(range(26))),
            ),
            (
                ["read-holding", "--start", "0", "--count", "2"],
                lambda request: read_response(request, (10, 20)),
            ),
            (
                ["read-input", "--start", "32", "--count", "2"],
                lambda request: read_response(request, (1, 2)),
            ),
            (
                ["write-single", "--register", "64", "--value", "3"],
                write_response,
            ),
            (
                ["write-multiple", "--start", "0", "--values", "10", "3"],
                write_response,
            ),
            (
                ["apply"],
                lambda request: (
                    write_response(request)
                    if request[1] == FUNCTION_WRITE_SINGLE
                    else read_response(
                        request,
                        observation_values(command=1, result=1),
                    )
                ),
            ),
            (
                ["stop"],
                lambda request: (
                    write_response(request)
                    if request[1] == FUNCTION_WRITE_SINGLE
                    else read_response(
                        request,
                        observation_values(command=4, result=1),
                    )
                ),
            ),
        ]
        for arguments, handler in cases:
            with self.subTest(arguments=arguments):
                transport = ScriptedTransport(handler)
                code, stdout, stderr = self._run(
                    ["--port", "/dev/fake", "--json", *arguments],
                    transport,
                )
                self.assertEqual(code, 0, stderr)
                self.assertTrue(json.loads(stdout)["ok"])

    def test_single_duplicate_is_success(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            if calls == 1:
                return write_response(request)
            if calls == 2:
                return read_response(
                    request,
                    observation_values(command=5, result=2, command_id=1001),
                )
            return read_response(request, snapshot_values(7))

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "single", "--id", "1001"],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        payload = json.loads(stdout)
        self.assertTrue(payload["result"]["duplicate"])

    def test_observation_failure_keeps_write_acknowledged(self) -> None:
        calls = 0

        def handler(request: bytes) -> bytes:
            nonlocal calls
            calls += 1
            if calls == 1:
                return write_response(request)
            raise TransportError("observation disconnected")

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "apply"],
            transport,
        )
        self.assertEqual(code, 3)
        payload = json.loads(stdout)
        self.assertFalse(payload["ok"])
        self.assertTrue(payload["result"]["write_acknowledged"])

    def test_watch_json_is_jsonl_and_bounded(self) -> None:
        transport = ScriptedTransport(
            lambda request: read_response(request, snapshot_values(1))
        )
        code, stdout, stderr = self._run(
            [
                "--port",
                "/dev/fake",
                "--json",
                "watch",
                "--interval",
                "1",
                "--samples",
                "1",
            ],
            transport,
        )
        self.assertEqual(code, 0, stderr)
        lines = stdout.strip().splitlines()
        self.assertEqual(len(lines), 1)
        self.assertEqual(json.loads(lines[0])["operation"], "watch")

    def test_keyboard_interrupt_is_130(self) -> None:
        def handler(_request: bytes) -> bytes:
            raise KeyboardInterrupt

        transport = ScriptedTransport(handler)
        code, stdout, stderr = self._run(
            ["--port", "/dev/fake", "--json", "identity"],
            transport,
        )
        self.assertEqual(code, 130)
        self.assertEqual(json.loads(stdout)["error"]["kind"], "interrupted")


if __name__ == "__main__":
    unittest.main()
