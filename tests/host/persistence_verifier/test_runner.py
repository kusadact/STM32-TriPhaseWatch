from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
from pathlib import Path
import tempfile
import unittest

from fake_device import FakeDeviceTransport, VirtualClock
from fixture_factory import valid_identity
from persistence.capture import CapturingClient
from persistence.run import (
    Connection,
    RunConfig,
    build_parser,
    main,
    run_scenario,
)
from tools.modbus_client.client import ModbusClient
from tools.modbus_client.service import ModbusService


class TimeoutTransport:
    def __init__(self) -> None:
        self.description = "synthetic-timeout"
        self.open_count = 0
        self.close_count = 0
        self.writes: list[bytes] = []
        self.closed = True

    def open(self) -> None:
        self.open_count += 1
        self.closed = False

    def close(self) -> None:
        self.close_count += 1
        self.closed = True

    def write_all(self, data: bytes, deadline: float) -> list[bytes]:
        self.writes.append(data)
        return [data]

    def read(self, max_bytes: int, deadline: float) -> bytes | None:
        return None

    def read_available(self, max_bytes: int = 512) -> bytes:
        return b""

    def recover(
        self,
        duration_seconds: float,
        quiet_seconds: float,
        deadline: float,
    ) -> bool:
        return True


class RunnerTests(unittest.TestCase):
    def _config(
        self,
        root: Path,
        *,
        scenario: str = "short",
        duration_s: float = 10.0,
        sample_count: int = 3,
        save_at_s: float | None = None,
    ) -> RunConfig:
        return RunConfig(
            port="/dev/synthetic",
            scenario=scenario,
            output=root / "run",
            identity_path=root / "identity.json",
            duration_s=duration_s,
            period_s=10,
            poll_s=1.0,
            address=1,
            timeout_s=0.05,
            mask=15,
            sample_count=sample_count,
            save_at_s=save_at_s,
            set_time_utc=1_700_000_000,
        )

    def _factory_for(
        self,
        transport,
        *,
        timeout_s: float = 0.05,
    ):
        def factory(_config: RunConfig, recorder):
            client = ModbusClient(
                transport,
                timeout_seconds=timeout_s,
                recovery_seconds=0.001,
                recovery_quiet_seconds=0.001,
                recovery_limit_seconds=0.01,
            )
            service = ModbusService(CapturingClient(client, recorder))
            client.open()
            return Connection(service=service, client=client, close=client.close)

        return factory

    def test_help_is_side_effect_free(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            with redirect_stdout(StringIO()):
                build_parser().parse_args(["--help"])
        self.assertEqual(raised.exception.code, 0)

    def test_short_run_uses_one_connection_and_finite_count(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = self._config(root)
            transport = FakeDeviceTransport(finite_count=3)
            clock = VirtualClock()
            code = run_scenario(
                config,
                valid_identity(),
                connection_factory=self._factory_for(transport),
                clock=clock.monotonic,
                sleep=clock.sleep,
            )
            self.assertEqual(code, 3)
            self.assertEqual(transport.open_count, 1)
            self.assertEqual(transport.close_count, 1)
            self.assertEqual(transport.sequence, 3)
            self.assertEqual(transport.command_writes.count(3), 1)
            self.assertEqual(transport.command_writes.count(4), 1)
            manifest = json.loads(
                (config.output / "manifest.json").read_text(encoding="utf-8")
            )
            result = json.loads(
                (config.output / "result.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["capture"], "PASS")
            self.assertEqual(manifest["end_to_end"], "NOT_RUN")
            self.assertEqual(result["exit_code"], 3)

    def test_baseline_run_uses_virtual_clock_and_stops(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = self._config(
                root,
                scenario="baseline",
                duration_s=3.0,
                sample_count=0,
                save_at_s=None,
            )
            transport = FakeDeviceTransport(finite_count=0)
            clock = VirtualClock()
            code = run_scenario(
                config,
                valid_identity(),
                connection_factory=self._factory_for(transport),
                clock=clock.monotonic,
                sleep=clock.sleep,
            )
            self.assertEqual(code, 3)
            self.assertEqual(transport.open_count, 1)
            self.assertEqual(transport.close_count, 1)
            self.assertGreaterEqual(transport.command_writes.count(4), 2)
            self.assertIn(6, transport.command_writes)
            manifest = json.loads(
                (config.output / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["completion_reason"], "duration_elapsed")
            self.assertGreaterEqual(manifest["observed_duration_s"], 3.0)

    def test_timeout_is_recorded_without_blind_retry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = self._config(root)
            transport = TimeoutTransport()
            clock = VirtualClock()
            code = run_scenario(
                config,
                valid_identity(),
                connection_factory=self._factory_for(transport, timeout_s=0.01),
                clock=clock.monotonic,
                sleep=clock.sleep,
            )
            self.assertEqual(code, 1)
            self.assertEqual(transport.open_count, 1)
            self.assertEqual(transport.close_count, 1)
            self.assertEqual(len(transport.writes), 2)
            result = json.loads(
                (config.output / "result.json").read_text(encoding="utf-8")
            )
            self.assertEqual(result["capture"], "FAIL")
            self.assertEqual(result["end_to_end"], "NOT_RUN")

    def test_missing_formal_identity_rejects_before_transport(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "run"
            stdout = StringIO()
            stderr = StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                code = main(
                    [
                        "--port",
                        "/dev/never-open",
                        "--scenario",
                        "short",
                        "--output",
                        str(output),
                        "--identity",
                        str(root / "missing.json"),
                    ]
                )
            self.assertEqual(code, 2)
            self.assertFalse(output.exists())
            self.assertIn("cannot read JSON", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
