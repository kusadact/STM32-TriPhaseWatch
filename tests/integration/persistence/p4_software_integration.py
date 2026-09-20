#!/usr/bin/env python3
"""P5-05 software integration driver for a fixed P4 candidate tree."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
from typing import Any


if __package__ in (None, ""):
    repository_root = Path(__file__).resolve().parents[3]
    if str(repository_root) not in sys.path:
        sys.path.insert(0, str(repository_root))
    persistence_parent = Path(__file__).resolve().parents[1]
    if str(persistence_parent) not in sys.path:
        sys.path.insert(0, str(persistence_parent))
    host_driver = repository_root / "tests" / "host" / "persistence_verifier"
    if str(host_driver) not in sys.path:
        sys.path.insert(0, str(host_driver))

from persistence.capture import CapturingClient
from persistence.manifest import (
    load_identity,
    sha256_file,
    write_json_atomic,
)
from persistence.run import Connection, RunConfig, run_scenario
from persistence.verify import verify_run
from p4_adapter_transport import SubprocessAdapterTransport

_CANDIDATE_CLIENT: Any = None
_CANDIDATE_SERVICE: Any = None


def _bind_candidate_cli(
    integration_root: Path,
    identity: dict[str, Any],
) -> None:
    global _CANDIDATE_CLIENT, _CANDIDATE_SERVICE

    root = integration_root.resolve()
    service_path = root / "tools" / "modbus_client" / "service.py"
    if not service_path.is_file():
        print(f"integration root has no candidate CLI: {service_path}", file=sys.stderr)
        raise SystemExit(2)
    expected = str(identity["cli"]["source_sha256"]).lower()
    actual = sha256_file(service_path)
    if actual != expected:
        print(
            "integration root CLI hash mismatch: "
            f"expected {expected}, actual {actual}",
            file=sys.stderr,
        )
        raise SystemExit(2)
    sys.path.insert(0, str(root))
    for name in [
        name
        for name in sys.modules
        if name == "tools" or name.startswith("tools.")
    ]:
        del sys.modules[name]
    from tools.modbus_client.client import ModbusClient
    from tools.modbus_client.service import ModbusService

    _CANDIDATE_CLIENT = ModbusClient
    _CANDIDATE_SERVICE = ModbusService


def _read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    return [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]


def _build_file_manifest(
    run_dir: Path,
    storage_root: Path,
    *,
    run_id: str,
    boot_epoch: str,
) -> list[dict[str, Any]]:
    files: list[dict[str, Any]] = []
    for path in sorted(storage_root.rglob("*.CSV")):
        relative = path.relative_to(storage_root).as_posix()
        if not relative.startswith("LOG/"):
            continue
        files.append(
            {
                "path": relative,
                "source_path": path.as_posix(),
                "copy_path": path.as_posix(),
                "size": path.stat().st_size,
                "sha256": sha256_file(path),
                "run_id": run_id,
                "boot_epoch": boot_epoch,
                "evidence_layer": "synthetic-host-storage-substitute",
            }
        )
    write_json_atomic(
        run_dir / "file_manifest.json",
        {
            "format": "p5-persistence-file-manifest/1",
            "status": "PASS" if files else "FAIL",
            "scope": "P5-05 synthetic host storage substitute",
            "files": files,
        },
    )
    return files


def _save_observation(run_dir: Path) -> dict[str, Any] | None:
    for row in reversed(_read_jsonl(run_dir / "observations.jsonl")):
        if row.get("kind") == "save":
            value = row.get("value")
            if isinstance(value, dict):
                return value
    return None


def _restart_check(
    *,
    adapter: Path,
    storage_root: Path,
    config_image: Path,
    expected_config: dict[str, int],
    time_step_us: int,
) -> dict[str, Any]:
    transport = SubprocessAdapterTransport(
        adapter,
        storage_root,
        config_image,
        time_step_us=time_step_us,
        save_delay_calls=0,
    )
    client = _CANDIDATE_CLIENT(
        transport,
        timeout_seconds=1.0,
        recovery_seconds=0.01,
        recovery_quiet_seconds=0.001,
        recovery_limit_seconds=0.05,
    )
    client.open()
    try:
        service = _CANDIDATE_SERVICE(client)
        status = service.status()
        active = status["active_config"]
        actual = {
            "period_sec": int(active["period_sec"]),
            "channel_mask": int(active["channel_mask"]),
            "record_count": int(active["record_count"]),
        }
        return {
            "status": "PASS" if actual == expected_config else "FAIL",
            "actual": actual,
            "expected": expected_config,
        }
    finally:
        client.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run the P5 runner against the real-C P4 adapter, then build the "
            "synthetic host file manifest and run the independent verifier."
        )
    )
    parser.add_argument("--adapter", required=True, type=Path)
    parser.add_argument("--integration-root", required=True, type=Path)
    parser.add_argument("--identity", required=True, type=Path)
    parser.add_argument("--storage-root", required=True, type=Path)
    parser.add_argument("--config-image", required=True, type=Path)
    parser.add_argument("--run-output", required=True, type=Path)
    parser.add_argument("--verify-output", required=True, type=Path)
    parser.add_argument("--matrix", required=True, type=Path)
    parser.add_argument("--scenario", choices=("short", "baseline"), default="baseline")
    parser.add_argument("--duration-s", type=float, default=1.0)
    parser.add_argument("--period-s", type=int, default=10)
    parser.add_argument("--mask", type=int, default=15)
    parser.add_argument("--count", type=int, default=0)
    parser.add_argument("--save-at-s", type=float, default=0.0)
    parser.add_argument("--time-step-us", type=int, default=10_000_000)
    parser.add_argument("--save-delay-calls", type=int, default=2)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    identity = load_identity(args.identity)
    _bind_candidate_cli(args.integration_root, identity)
    if not args.adapter.is_file():
        raise SystemExit(f"adapter does not exist: {args.adapter}")
    if args.run_output.exists():
        raise SystemExit(f"run output already exists: {args.run_output}")
    if args.verify_output.exists():
        raise SystemExit(f"verify output already exists: {args.verify_output}")
    if args.storage_root.exists() and any(args.storage_root.iterdir()):
        raise SystemExit(f"storage root is not empty: {args.storage_root}")
    args.storage_root.mkdir(parents=True, exist_ok=True)
    args.config_image.parent.mkdir(parents=True, exist_ok=True)

    adapter = args.adapter.resolve()
    storage_root = args.storage_root.resolve()
    config_image = args.config_image.resolve()

    def factory(config: RunConfig, recorder):
        transport = SubprocessAdapterTransport(
            adapter,
            storage_root,
            config_image,
            time_step_us=args.time_step_us,
            save_delay_calls=args.save_delay_calls,
        )
        client = _CANDIDATE_CLIENT(
            transport,
            timeout_seconds=1.0,
            recovery_seconds=0.01,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.05,
        )
        service = _CANDIDATE_SERVICE(CapturingClient(client, recorder))
        client.open()
        return Connection(service=service, client=client, close=client.close)

    run_code = run_scenario(
        RunConfig(
            port="p4-core-adapter",
            scenario=args.scenario,
            output=args.run_output,
            identity_path=args.identity,
            duration_s=args.duration_s,
            period_s=args.period_s,
            poll_s=1.0,
            address=1,
            timeout_s=1.0,
            mask=args.mask,
            sample_count=args.count,
            save_at_s=args.save_at_s if args.scenario == "baseline" else None,
            set_time_utc=1_709_164_800,
        ),
        identity,
        connection_factory=factory,
    )
    manifest = _read_json(args.run_output / "manifest.json")
    files = _build_file_manifest(
        args.run_output,
        storage_root,
        run_id=str(manifest["run_id"]),
        boot_epoch=str(manifest["boot_epoch"]),
    )
    report = verify_run(args.run_output, storage_root, args.verify_output)
    save = _save_observation(args.run_output)
    expected_config = {
        "period_sec": args.period_s,
        "channel_mask": args.mask,
        "record_count": args.count,
    }
    restart = _restart_check(
        adapter=adapter,
        storage_root=storage_root,
        config_image=config_image,
        expected_config=expected_config,
        time_step_us=args.time_step_us,
    )
    matrix_rows = []
    for line in args.matrix.read_text(encoding="utf-8").splitlines():
        if line.startswith(("E0", "S0", "R0")):
            matrix_rows.append(line.split("\t", 2)[:2])
    deviation_codes = [
        issue["code"]
        for issue in report.issues
        if issue["code"] in {"captured_period_range", "captured_mask_range"}
    ]
    summary = {
        "synthetic_layer": "host-modbus+EEPROM-file+SD-file-substitute",
        "run_exit": run_code,
        "verify_exit": report.exit_code,
        "verify_overall": report.overall,
        "capture": manifest.get("capture"),
        "end_to_end": manifest.get("end_to_end"),
        "files": files,
        "save_observation": save,
        "restart_check": restart,
        "known_p4_contract_deviation": {
            "status": "FAIL" if deviation_codes else "NOT_OBSERVED",
            "codes": deviation_codes,
            "input": "SAVE then one read of 0x0080..0x00AF",
            "expected": "0x00A4..0x00A5 captured_period u32; 0x00A6 mask; 0x00A7 count",
            "actual_candidate": "0x00A4 period u16; 0x00A5 mask; 0x00A6 count",
        },
        "p4_matrix": matrix_rows,
        "checked_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    if run_code != 3:
        return 1
    if report.exit_code != 0:
        return report.exit_code
    if restart["status"] != "PASS":
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
