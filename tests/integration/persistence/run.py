#!/usr/bin/env python3
"""Run short or baseline persistence capture through the production client."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import inspect
import json
from pathlib import Path
import sys
import time
from typing import Any, Callable, Mapping, Sequence
import uuid


if __package__ in (None, ""):
    repository_root = Path(__file__).resolve().parents[3]
    if str(repository_root) not in sys.path:
        sys.path.insert(0, str(repository_root))
    package_parent = Path(__file__).resolve().parents[1]
    if str(package_parent) not in sys.path:
        sys.path.insert(0, str(package_parent))

from persistence import oracle
from persistence.capture import (
    CapturingClient,
    EvidenceRecorder,
)
from persistence.manifest import (
    ManifestError,
    RunArtifacts,
    build_initial_manifest,
    load_identity,
    utc_now_iso,
    validate_identity,
    write_json_atomic,
)

from tools.modbus_client.client import ModbusClient
from tools.modbus_client.errors import ModbusClientError, StateError
from tools.modbus_client.service import ModbusService
from tools.modbus_client.transport import MacOSTTYTransport
from tools.modbus_client.timeparse import split_u32


EXIT_CAPTURE_PASS_END_TO_END_NOT_RUN = 3
EXIT_CAPTURE_FAIL = 1
EXIT_INPUT_ENVIRONMENT = 2

HOLDING_PENDING_UTC_START = 0x0020
HOLDING_COMMAND = 0x0040
COMMAND_SET_TIME = 6
INPUT_COMMAND_OBSERVATION_START = 0x0005
INPUT_COMMAND_OBSERVATION_COUNT = 11


class RunnerInputError(ValueError):
    pass


@dataclass(frozen=True)
class RunConfig:
    port: str
    scenario: str
    output: Path
    identity_path: Path
    duration_s: float
    period_s: int
    poll_s: float
    address: int
    timeout_s: float
    mask: int
    sample_count: int
    save_at_s: float | None
    set_time_utc: int | None

    def parameters(self) -> dict[str, Any]:
        return {
            "port": self.port,
            "scenario": self.scenario,
            "duration_s": self.duration_s,
            "period_s": self.period_s,
            "poll_s": self.poll_s,
            "address": self.address,
            "timeout_s": self.timeout_s,
            "mask": self.mask,
            "sample_count": self.sample_count,
            "save_at_s": self.save_at_s,
            "set_time_utc": self.set_time_utc,
        }


@dataclass
class Connection:
    service: Any
    client: Any
    close: Callable[[], None]


@dataclass(frozen=True)
class CaptureOutcome:
    observed_duration_s: float
    completion_reason: str
    details: Mapping[str, Any]


def _service_call(
    recorder: EvidenceRecorder,
    operation: str,
    parameters: Mapping[str, Any],
    callback: Callable[[], Any],
    *,
    observation_kind: str | None = None,
) -> Any:
    with recorder.command(operation, parameters) as command_id:
        result = callback()
        if observation_kind is not None:
            recorder.record_observation(
                observation_kind,
                result if isinstance(result, Mapping) else {"result": result},
                command_id=command_id,
            )
        return result


class ScenarioRunner:
    def __init__(
        self,
        *,
        config: RunConfig,
        service: Any,
        recorder: EvidenceRecorder,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
    ) -> None:
        self.config = config
        self.service = service
        self.recorder = recorder
        self.clock = clock
        self.sleep = sleep
        self._saved = False
        self._initial_session: int | None = None

    def _read_identity(self) -> dict[str, Any]:
        result = _service_call(
            self.recorder,
            "identity",
            {},
            self.service.identity,
            observation_kind="identity",
        )
        if result.get("protocol_version") != 3:
            raise RunnerInputError(
                "formal persistence verification requires protocol version 3"
            )
        return result

    def _read_status(self, label: str = "status") -> dict[str, Any]:
        result = _service_call(
            self.recorder,
            label,
            {},
            self.service.status,
            observation_kind="status",
        )
        session = int(result.get("session_id", 0))
        if self._initial_session is None:
            self._initial_session = session
        elif session != self._initial_session:
            raise RuntimeError(
                f"device session changed from {self._initial_session} to {session}"
            )
        return result

    def _read_snapshot(self) -> dict[str, Any]:
        return _service_call(
            self.recorder,
            "snapshot",
            {},
            self.service.snapshot,
            observation_kind="snapshot",
        )

    def _read_stats(self) -> dict[str, Any]:
        return _service_call(
            self.recorder,
            "stats",
            {},
            self.service.stats,
            observation_kind="stats",
        )

    def _read_time(self) -> dict[str, Any]:
        def callback() -> dict[str, Any]:
            values = self.service.read_input(
                oracle.TIME_STATUS_START,
                oracle.TIME_STATUS_COUNT,
            )
            return oracle.decode_time_status_block(values)

        return _service_call(
            self.recorder,
            "time-status",
            {},
            callback,
            observation_kind="time",
        )

    def _read_storage(self) -> dict[str, Any]:
        def callback() -> dict[str, Any]:
            values = self.service.read_input(
                oracle.STORAGE_START,
                oracle.STORAGE_COUNT,
            )
            return oracle.decode_storage_block(values)

        return _service_call(
            self.recorder,
            "storage-status",
            {},
            callback,
            observation_kind="storage",
        )

    def _stop_once(self) -> dict[str, Any]:
        return _service_call(
            self.recorder,
            "stop",
            {},
            self.service.stop,
            observation_kind="command",
        )

    def _wait_for_drain(self, timeout_s: float = 5.0) -> None:
        deadline = self.clock() + timeout_s
        while True:
            storage = self._read_storage()
            if storage["drain_state"] == 3:
                raise StateError("storage drain failed")
            if (
                storage["drain_state"] == 2
                and storage["queued"] == 0
                and storage["in_flight"] == 0
            ):
                return
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise TimeoutError(
                    "storage did not drain within the bounded stop window"
                )
            self.sleep(min(0.1, remaining))

    def _stop_and_wait(self, *, always_write: bool = True) -> None:
        status = self._read_status("status-before-stop")
        if always_write or status.get("run_state_code") != 0:
            self._stop_once()
        self._wait_for_drain()

    def _configure_and_apply(self) -> dict[str, Any]:
        _service_call(
            self.recorder,
            "config",
            {
                "period_s": self.config.period_s,
                "mask": self.config.mask,
                "sample_count": self.config.sample_count,
            },
            lambda: self.service.config(
                self.config.period_s,
                self.config.mask,
                self.config.sample_count,
            ),
            observation_kind="config",
        )
        apply_result = _service_call(
            self.recorder,
            "apply",
            {},
            self.service.apply,
            observation_kind="command",
        )
        status = self._read_status("status-after-apply")
        active = status.get("active_config", {})
        config_observation = {
            "version": int(active.get("version", 0)),
            "period_s": int(active.get("period_sec", 0)),
            "mask": int(active.get("channel_mask", 0)),
            "sample_count": int(active.get("record_count", 0)),
        }
        self.recorder.record_observation("config_apply", config_observation)
        expected_active = (
            self.config.period_s,
            self.config.mask,
            self.config.sample_count,
        )
        actual_active = (
            config_observation["period_s"],
            config_observation["mask"],
            config_observation["sample_count"],
        )
        if actual_active != expected_active:
            raise StateError(
                "applied configuration does not match requested configuration",
                expected=expected_active,
                actual=actual_active,
            )
        return {"apply": apply_result, "status": status}

    def _start(self) -> dict[str, Any]:
        return _service_call(
            self.recorder,
            "start",
            {},
            self.service.start,
            observation_kind="command",
        )

    def _set_time(self, utc_seconds: int) -> None:
        if not 0 <= utc_seconds < 0xFFFFFFFF:
            raise RunnerInputError("set_time_utc is outside the legal UTC range")
        words = split_u32(utc_seconds)

        def callback() -> dict[str, Any]:
            self.service.write_multiple(HOLDING_PENDING_UTC_START, words)
            self.service.write_single(HOLDING_COMMAND, COMMAND_SET_TIME)
            observation = self.service.read_input(
                INPUT_COMMAND_OBSERVATION_START,
                INPUT_COMMAND_OBSERVATION_COUNT,
            )
            if observation[7] != COMMAND_SET_TIME or observation[8] != 1:
                raise StateError(
                    "SET_TIME command was not accepted",
                    command=observation[7],
                    result=observation[8],
                )
            return {
                "requested_utc_seconds": utc_seconds,
                "pending_words": list(words),
                "command": observation[7],
                "result": observation[8],
            }

        _service_call(
            self.recorder,
            "time-set",
            {"utc_seconds": utc_seconds},
            callback,
            observation_kind="command",
        )

    def _save(self) -> None:
        method = getattr(self.service, "save", None)
        if method is None:
            raise StateError("production service does not expose save()")
        signature = inspect.signature(method)
        accepts_command_id = (
            "command_id" in signature.parameters
            or "save_command_id" in signature.parameters
        )
        command_id = int(self.clock() * 1000.0) & 0xFFFFFFFF

        def callback() -> dict[str, Any]:
            if accepts_command_id:
                if "command_id" in signature.parameters:
                    result = method(command_id=command_id)
                else:
                    result = method(save_command_id=command_id)
            else:
                result = method()
            if not isinstance(result, Mapping):
                return {"result": result, "command_id": command_id}
            output = dict(result)
            output.setdefault("command_id", command_id)
            return output

        _service_call(
            self.recorder,
            "save",
            {"command_id": command_id},
            callback,
            observation_kind="save",
        )
        self._saved = True

    def _checkpoint(
        self,
        label: str,
        *,
        status: Mapping[str, Any] | None = None,
        storage: Mapping[str, Any] | None = None,
        time_status: Mapping[str, Any] | None = None,
    ) -> None:
        value: dict[str, Any] = {}
        if status is not None:
            value["status"] = dict(status)
        if storage is not None:
            value["storage"] = dict(storage)
        if time_status is not None:
            value["time"] = dict(time_status)
        self.recorder.checkpoint(label, value)

    def run_short(self) -> CaptureOutcome:
        start = self.clock()
        identity = self._read_identity()
        status = self._read_status("initial-status")
        time_status = self._read_time()
        storage = self._read_storage()
        self._checkpoint(
            "discovery",
            status=status,
            storage=storage,
            time_status=time_status,
        )
        self._stop_and_wait(always_write=True)
        self._configure_and_apply()
        short_start_storage = self._read_storage()
        self._checkpoint("start", storage=short_start_storage)
        self._start()

        deadline = self.clock() + self.config.duration_s
        complete = False
        while self.clock() <= deadline:
            status = self._read_status("short-poll")
            self._read_snapshot()
            storage = self._read_storage()
            if (
                status.get("run_state_code") == 0
                and storage["queued"] == 0
                and storage["in_flight"] == 0
                and storage["drain_state"] in (0, 2)
            ):
                complete = True
                break
            remaining = deadline - self.clock()
            if remaining <= 0:
                break
            self.sleep(min(self.config.poll_s, remaining))
        if not complete:
            raise TimeoutError(
                "short finite run did not stop and drain within --duration-s"
            )
        final_status = self._read_status("final-status")
        final_storage = self._read_storage()
        final_time = self._read_time()
        generated = (
            final_storage["generated"] - short_start_storage["generated"]
        ) % (1 << 32)
        if generated != self.config.sample_count:
            raise StateError(
                "short run generated count does not match requested finite count",
                generated=generated,
                requested=self.config.sample_count,
            )
        if final_status.get("records_this_run") != self.config.sample_count:
            raise StateError(
                "short run status count does not match requested finite count",
                status_count=final_status.get("records_this_run"),
                requested=self.config.sample_count,
            )
        self._checkpoint(
            "end",
            status=final_status,
            storage=final_storage,
            time_status=final_time,
        )
        return CaptureOutcome(
            observed_duration_s=self.clock() - start,
            completion_reason="finite_count_completed",
            details={
                "identity": identity,
                "final_status": final_status,
                "final_storage": final_storage,
            },
        )

    def run_baseline(self) -> CaptureOutcome:
        identity = self._read_identity()
        initial_status = self._read_status("initial-status")
        initial_time = self._read_time()
        initial_stats = self._read_stats()
        initial_storage = self._read_storage()
        self._checkpoint(
            "discovery",
            status=initial_status,
            storage=initial_storage,
            time_status=initial_time,
        )
        self._stop_and_wait(always_write=True)
        set_time_utc = self.config.set_time_utc
        if set_time_utc is None:
            set_time_utc = int(datetime.now(timezone.utc).timestamp())
        self._set_time(set_time_utc)
        self._configure_and_apply()

        start_status = self._read_status("pre-start-status")
        start_storage = self._read_storage()
        if start_storage["queued"] != 0 or start_storage["in_flight"] != 0:
            raise StateError(
                "baseline start requires an empty queue and no in-flight record",
                queued=start_storage["queued"],
                in_flight=start_storage["in_flight"],
            )
        self._checkpoint(
            "baseline_start",
            status=start_status,
            storage=start_storage,
        )
        start_checkpoint = self._start()
        run_started = self.clock()
        next_poll = run_started
        next_slow = run_started + 60.0
        save_at = self.config.save_at_s
        rounds = 0

        while True:
            now = self.clock()
            if now - run_started >= self.config.duration_s:
                break
            if rounds:
                next_poll += self.config.poll_s
            else:
                next_poll = run_started
            wait_s = next_poll - self.clock()
            if wait_s > 0:
                self.sleep(wait_s)
            self._read_status("baseline-poll")
            self._read_snapshot()
            storage = self._read_storage()
            now = self.clock()
            if now >= next_slow:
                self._read_stats()
                self._read_time()
                next_slow = now + 60.0
            if (
                save_at is not None
                and not self._saved
                and now - run_started >= save_at
                and save_at <= self.config.duration_s
            ):
                self._save()
            rounds += 1
            if rounds > int(self.config.duration_s / self.config.poll_s) + 10:
                raise RuntimeError("baseline poll loop did not make progress")

        observed = self.clock() - run_started
        self._stop_once()
        self._wait_for_drain()
        final_status = self._read_status("final-status")
        final_time = self._read_time()
        final_stats = self._read_stats()
        final_storage = self._read_storage()
        if final_status.get("run_state_code") != 0:
            raise StateError("baseline STOP did not leave the device stopped")
        self._checkpoint(
            "baseline_end",
            status=final_status,
            storage=final_storage,
            time_status=final_time,
        )
        return CaptureOutcome(
            observed_duration_s=observed,
            completion_reason="duration_elapsed",
            details={
                "identity": identity,
                "start_command": start_checkpoint,
                "start_storage": start_storage,
                "final_status": final_status,
                "final_storage": final_storage,
                "final_stats": final_stats,
                "poll_rounds": rounds,
                "observed_duration_s": observed,
            },
        )

    def run(self) -> CaptureOutcome:
        if self.config.scenario == "short":
            return self.run_short()
        if self.config.scenario == "baseline":
            return self.run_baseline()
        raise RunnerInputError(f"unsupported scenario {self.config.scenario!r}")


def _production_connection_factory(
    config: RunConfig,
    recorder: EvidenceRecorder,
) -> Connection:
    transport = MacOSTTYTransport(config.port, logger=recorder)
    client = ModbusClient(
        transport,
        timeout_seconds=config.timeout_s,
        logger=recorder,
    )
    capturing_client = CapturingClient(client, recorder)
    service = ModbusService(capturing_client, address=config.address)
    client.open()

    def close() -> None:
        client.close()

    return Connection(service=service, client=client, close=close)


def _best_effort_stop(service: Any, recorder: EvidenceRecorder) -> str | None:
    try:
        service.stop()
    except BaseException as exc:
        return f"{type(exc).__name__}: {exc}"
    recorder.record_observation("best_effort_stop", {"accepted": True})
    return None


def _write_final(
    artifacts: RunArtifacts,
    manifest: dict[str, Any],
    *,
    capture: str,
    reason: str,
    exit_code: int,
    observed_duration_s: float | None,
    completion_reason: str | None,
    details: Mapping[str, Any] | None = None,
) -> None:
    manifest.update(
        {
            "phase": (
                "COMPLETE"
                if capture == "PASS"
                else "PARTIAL"
                if capture == "PARTIAL"
                else "FAILED"
            ),
            "capture": capture,
            "end_to_end": "NOT_RUN",
            "ended_utc": utc_now_iso(),
            "observed_duration_s": observed_duration_s,
            "completion_reason": completion_reason,
            "monotonic_end_s": time.monotonic(),
        }
    )
    write_json_atomic(artifacts.manifest_path, manifest)
    write_json_atomic(
        artifacts.result_path,
        {
            "format": "p5-persistence-run-result/1",
            "capture": capture,
            "end_to_end": "NOT_RUN",
            "exit_code": exit_code,
            "reason": reason,
            "details": dict(details or {}),
            "next_step": (
                "copy the run-scoped LOG files read-only, build file_manifest.json, "
                "then run verify.py"
            ),
        },
    )


def run_scenario(
    config: RunConfig,
    identity: Mapping[str, Any] | dict[str, Any],
    *,
    connection_factory: Callable[
        [RunConfig, EvidenceRecorder], Connection
    ] = _production_connection_factory,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    try:
        validated_identity = validate_identity(identity)
    except ManifestError as exc:
        print(f"run: {exc}", file=sys.stderr)
        return EXIT_INPUT_ENVIRONMENT
    run_id = str(uuid.uuid4())
    artifacts = RunArtifacts.paths(config.output)
    initial = build_initial_manifest(
        run_id=run_id,
        scenario=config.scenario,
        identity=validated_identity,
        parameters=config.parameters(),
        expected_duration_s=config.duration_s,
    )
    initial["monotonic_origin_s"] = clock()
    try:
        artifacts.create(initial)
    except ManifestError as exc:
        print(f"run: {exc}", file=sys.stderr)
        return EXIT_INPUT_ENVIRONMENT

    recorder = EvidenceRecorder(artifacts)
    connection: Connection | None = None
    outcome: CaptureOutcome | None = None
    capture = "FAIL"
    exit_code = EXIT_CAPTURE_FAIL
    reason = "capture failure"
    observed_duration_s: float | None = None
    completion_reason: str | None = "capture_failure"
    details: dict[str, Any] = {}

    try:
        try:
            connection = connection_factory(config, recorder)
        except (ModbusClientError, OSError, RunnerInputError) as exc:
            capture = "NOT_RUN"
            exit_code = EXIT_INPUT_ENVIRONMENT
            reason = f"environment/input failure: {type(exc).__name__}: {exc}"
            completion_reason = "environment_failure"
            details = {"error": repr(exc)}
        else:
            try:
                runner = ScenarioRunner(
                    config=config,
                    service=connection.service,
                    recorder=recorder,
                    clock=clock,
                    sleep=sleep,
                )
                outcome = runner.run()
            except KeyboardInterrupt:
                stop_error = _best_effort_stop(connection.service, recorder)
                capture = "PARTIAL"
                exit_code = EXIT_CAPTURE_PASS_END_TO_END_NOT_RUN
                reason = "run interrupted by Ctrl-C"
                completion_reason = "interrupted"
                details = {"best_effort_stop_error": stop_error}
            except RunnerInputError as exc:
                capture = "NOT_RUN"
                exit_code = EXIT_INPUT_ENVIRONMENT
                reason = f"input failure: {type(exc).__name__}: {exc}"
                completion_reason = "environment_failure"
                details = {"error": repr(exc)}
            except BaseException as exc:
                stop_error = _best_effort_stop(connection.service, recorder)
                capture = "FAIL"
                exit_code = EXIT_CAPTURE_FAIL
                reason = f"capture failure: {type(exc).__name__}: {exc}"
                completion_reason = "capture_failure"
                details = {
                    "error": repr(exc),
                    "best_effort_stop_error": stop_error,
                }
            else:
                capture = "PASS"
                exit_code = EXIT_CAPTURE_PASS_END_TO_END_NOT_RUN
                reason = (
                    "run capture completed; copied SD files were not available "
                    "to this command, so end_to_end remains NOT_RUN"
                )
                observed_duration_s = outcome.observed_duration_s
                completion_reason = outcome.completion_reason
                details = dict(outcome.details)
    finally:
        close_error = None
        if connection is not None:
            try:
                connection.close()
            except BaseException as exc:
                close_error = f"{type(exc).__name__}: {exc}"
        if close_error is not None:
            recorder.record_observation(
                "connection_close_error",
                {"error": close_error},
            )
            if capture == "PASS":
                capture = "FAIL"
                exit_code = EXIT_CAPTURE_FAIL
                reason = f"connection close failed: {close_error}"
                completion_reason = "capture_failure"
                details["connection_close_error"] = close_error
        recorder.close()

    _write_final(
        artifacts,
        initial,
        capture=capture,
        reason=reason,
        exit_code=exit_code,
        observed_duration_s=observed_duration_s,
        completion_reason=completion_reason,
        details=details,
    )
    return exit_code


def _positive_float(text: str) -> float:
    value = float(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be > 0")
    return value


def _nonnegative_float(text: str) -> float:
    value = float(text)
    if value < 0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return value


def _u16(text: str) -> int:
    value = int(text, 0)
    if not 0 <= value <= 0xFFFF:
        raise argparse.ArgumentTypeError("must be in 0..65535")
    return value


def _u31(text: str) -> int:
    value = int(text, 0)
    if not 0 <= value < 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("must be in 0..0xFFFFFFFE")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Capture a short finite run or the 7200 s persistence baseline "
            "through one production Modbus client connection."
        )
    )
    parser.add_argument("--port", required=True, help="explicit serial port path")
    parser.add_argument(
        "--scenario",
        required=True,
        choices=("short", "baseline"),
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="new run output directory; must not exist",
    )
    parser.add_argument(
        "--identity",
        required=True,
        type=Path,
        help="frozen identity JSON; missing fields are rejected",
    )
    parser.add_argument(
        "--duration-s",
        type=_positive_float,
        help="short wait bound or baseline duration; defaults short=60, baseline=7200",
    )
    parser.add_argument("--period-s", type=int, default=10)
    parser.add_argument("--poll-s", type=_positive_float, default=1.0)
    parser.add_argument("--address", type=int, default=1)
    parser.add_argument("--timeout-s", type=_positive_float, default=3.0)
    parser.add_argument("--mask", type=_u16, default=15)
    parser.add_argument("--count", type=_u16, default=None)
    parser.add_argument(
        "--save-at-s",
        type=_nonnegative_float,
        default=None,
        help="baseline SAVE time after START; defaults to 60, short never saves",
    )
    parser.add_argument(
        "--set-time-utc",
        type=_u31,
        default=None,
        help="baseline SET_TIME value; defaults to the Mac UTC second",
    )
    return parser


def config_from_args(args: argparse.Namespace) -> RunConfig:
    duration = args.duration_s
    if duration is None:
        duration = 7200.0 if args.scenario == "baseline" else 60.0
    sample_count = args.count
    if sample_count is None:
        sample_count = 0 if args.scenario == "baseline" else 3
    save_at = args.save_at_s
    if save_at is None and args.scenario == "baseline":
        save_at = 60.0
    if not 10 <= args.period_s <= 3600:
        raise RunnerInputError("--period-s must be in 10..3600")
    if not 1 <= args.mask <= 15:
        raise RunnerInputError("--mask must be in 1..15")
    if not 0 <= args.address <= 247:
        raise RunnerInputError("--address must be in 0..247")
    return RunConfig(
        port=args.port,
        scenario=args.scenario,
        output=args.output,
        identity_path=args.identity,
        duration_s=duration,
        period_s=args.period_s,
        poll_s=args.poll_s,
        address=args.address,
        timeout_s=args.timeout_s,
        mask=args.mask,
        sample_count=sample_count,
        save_at_s=save_at,
        set_time_utc=args.set_time_utc,
    )


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        config = config_from_args(args)
        identity = load_identity(config.identity_path)
    except (ManifestError, RunnerInputError, OSError, ValueError) as exc:
        print(f"run: {exc}", file=sys.stderr)
        return EXIT_INPUT_ENVIRONMENT
    return run_scenario(config, identity)


if __name__ == "__main__":
    raise SystemExit(main())
