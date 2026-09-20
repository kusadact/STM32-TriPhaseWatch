"""Synthetic run fixtures used only to test the verifier itself."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
from typing import Any, Iterable, Mapping


INTEGRATION_ROOT = Path(__file__).resolve().parents[2] / "integration"
if str(INTEGRATION_ROOT) not in sys.path:
    sys.path.insert(0, str(INTEGRATION_ROOT))

from persistence import oracle
from persistence.manifest import (
    RUN_MANIFEST_FORMAT,
    identity_sha256,
    validate_identity,
)


BASE_UTC = int(
    datetime(2026, 9, 20, 12, 0, tzinfo=timezone.utc).timestamp()
)


def valid_identity() -> dict[str, Any]:
    return validate_identity(
        {
            "baseline_sha": "8" * 40,
            "candidate_sha": "9" * 40,
            "candidate_diff_sha256": "a" * 64,
            "firmware": {
                "candidate_sha": "9" * 40,
                "elf_sha256": "b" * 64,
                "bin_sha256": "c" * 64,
                "build_config": "synthetic-test-build",
            },
            "cli": {
                "candidate_sha": "9" * 40,
                "source_sha256": "d" * 64,
            },
            "contract": {
                "protocol_version": 3,
                "contract_revision": 1,
                "csv_schema": 1,
            },
        }
    )


def storage_words(
    *,
    generated: int = 0,
    synced: int = 0,
    dropped: int = 0,
    uncertain: int = 0,
    queued: int = 0,
    in_flight: int = 0,
    drain_state: int = 2,
    last_synced_seq: int = 0,
    last_synced_file: int = 0,
    last_synced_date: int = 0,
    active_config_version: int = 1,
    storage_state: int = 1,
) -> list[int]:
    words = [0] * oracle.STORAGE_COUNT

    def put32(offset: int, value: int) -> None:
        words[offset] = (value >> 16) & 0xFFFF
        words[offset + 1] = value & 0xFFFF

    words[0] = 1
    words[7] = 1
    words[8] = storage_state
    words[10] = queued
    words[11] = queued
    put32(12, generated)
    put32(14, synced)
    put32(16, dropped)
    put32(18, uncertain)
    words[20] = in_flight
    words[21] = drain_state
    put32(22, last_synced_seq)
    put32(24, last_synced_file)
    put32(26, last_synced_date)
    put32(28, active_config_version)
    put32(30, 1)
    return words


def record_values(
    *,
    seq: int,
    session: int,
    trigger: int,
    period_s: int,
    mask: int,
    sample_count: int,
    config_version: int,
    utc_valid: int,
    utc_s: int,
    file_id: int,
    file_date: int,
    planned_ms: int | None = None,
    actual_ms: int | None = None,
) -> dict[str, int]:
    planned = planned_ms if planned_ms is not None else 1_000_000 + ((seq - 1) * 10_000)
    actual = actual_ms if actual_ms is not None else planned + 5
    values = {
        "schema": 1,
        "session": session,
        "seq": seq,
        "trigger": trigger,
        "planned_ms": planned,
        "actual_ms": actual,
        "utc_valid": utc_valid,
        "utc_s": utc_s,
        "config_version": config_version,
        "period_s": period_s,
        "mask": mask,
        "sample_count": sample_count,
        "source": 1,
        "file_id": file_id,
        "file_date": file_date,
        "reserved": 0,
    }
    for index in range(4):
        enabled = bool(mask & (1 << index))
        values[f"v{index}"] = (
            oracle.expected_test_value(seq, index) if enabled else 0
        )
        values[f"u{index}"] = 1
        values[f"q{index}"] = 1 if enabled else 0
    return values


def csv_bytes(records: Iterable[Mapping[str, int]]) -> bytes:
    lines = [oracle.CSV_HEADER]
    for record in records:
        lines.append(",".join(str(int(record[name])) for name in oracle.CSV_COLUMNS))
    return ("\n".join(lines) + "\n").encode("ascii")


def write_json(path: Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )


def write_jsonl(path: Path, rows: Iterable[Mapping[str, Any]]) -> None:
    with path.open("w", encoding="utf-8") as stream:
        for row in rows:
            stream.write(
                json.dumps(
                    row,
                    ensure_ascii=True,
                    sort_keys=True,
                    separators=(",", ":"),
                )
                + "\n"
            )


def make_valid_fixture(
    root: Path,
    *,
    record_count: int = 7,
    seq_start: int = 1,
    session: int = 1,
    period_s: int = 10,
    mask: int = 15,
    sample_count: int = 0,
    config_version: int = 1,
    utc_valid: int = 1,
    start_generated: int = 0,
    end_generated: int | None = None,
    end_synced: int | None = None,
    scenario: str = "short",
    expected_duration_s: float = 9.0,
    observed_duration_s: float = 9.0,
    completion_reason: str = "finite_count_completed",
    include_repeated_poll: bool = True,
) -> tuple[Path, Path]:
    run_dir = root / "run"
    files_dir = root / "files"
    run_dir.mkdir()
    file_path = files_dir / (
        "LOG/20260920/00000001.CSV" if utc_valid else "LOG/UNSET/00000001.CSV"
    )
    file_path.parent.mkdir(parents=True)
    if end_generated is None:
        end_generated = (start_generated + record_count) % (1 << 32)
    if end_synced is None:
        end_synced = end_generated

    records: list[dict[str, int]] = []
    for offset in range(record_count):
        seq = (seq_start + offset) % (1 << 32)
        utc_s = BASE_UTC + (offset * period_s) if utc_valid else 0
        file_date = int(oracle.utc_date_text(utc_s)) if utc_valid else 0
        planned_ms = 1_000_000 + (offset * ((period_s * 1000) + 5))
        actual_ms = planned_ms + 5
        records.append(
            record_values(
                seq=seq,
                session=session,
                trigger=1,
                period_s=period_s,
                mask=mask,
                sample_count=sample_count,
                config_version=config_version,
                utc_valid=utc_valid,
                utc_s=utc_s,
                file_id=1,
                file_date=file_date,
                planned_ms=planned_ms,
                actual_ms=actual_ms,
            )
        )
    data = csv_bytes(records)
    file_path.write_bytes(data)
    digest = hashlib.sha256(data).hexdigest()

    identity = valid_identity()
    started = "2026-09-20T04:00:00+00:00"
    ended = "2026-09-20T04:00:09+00:00"
    write_json(
        run_dir / "manifest.json",
        {
            "format": RUN_MANIFEST_FORMAT,
            "run_id": "synthetic-run-1",
            "boot_epoch": "synthetic-boot-1",
            "phase": "COMPLETE",
            "capture": "PASS",
            "end_to_end": "NOT_RUN",
            "scenario": scenario,
            "identity": identity,
            "identity_sha256": identity_sha256(identity),
            "parameters": {
                "period_s": period_s,
                "mask": mask,
                "sample_count": sample_count,
                "poll_s": 1.0,
            },
            "environment": {
                "kind": "synthetic",
                "warning": "not hardware evidence",
            },
            "started_utc": started,
            "ended_utc": ended,
            "expected_duration_s": expected_duration_s,
            "observed_duration_s": observed_duration_s,
            "completion_reason": completion_reason,
            "monotonic_origin_s": 100.0,
            "monotonic_end_s": 100.0 + observed_duration_s,
        },
    )
    write_json(
        run_dir / "file_manifest.json",
        {
            "format": "p5-persistence-file-manifest/1",
            "status": "PASS",
            "files": [
                {
                    "path": file_path.relative_to(files_dir).as_posix(),
                    "source_path": file_path.as_posix(),
                    "size": len(data),
                    "sha256": digest,
                    "run_id": "synthetic-run-1",
                    "boot_epoch": "synthetic-boot-1",
                }
            ],
        },
    )
    write_jsonl(
        run_dir / "commands.jsonl",
        [
            {
                "command_id": index + 1,
                "operation": operation,
                "status": "PASS",
                "transaction_ids": [index + 1],
            }
            for index, operation in enumerate(
                ("config", "apply", "time-set", "start", "stop")
            )
        ],
    )
    write_jsonl(run_dir / "transactions.jsonl", [])
    write_jsonl(run_dir / "frames.jsonl", [])

    start_last_seq = (seq_start - 1) % (1 << 32)
    first_storage = oracle.decode_storage_block(
        storage_words(
            generated=start_generated,
            synced=start_generated,
            last_synced_seq=start_last_seq if start_generated else 0,
            last_synced_file=1 if start_generated else 0,
            last_synced_date=(
                int(oracle.utc_date_text(BASE_UTC)) if start_generated else 0
            ),
            active_config_version=config_version,
        )
    )
    final_storage = oracle.decode_storage_block(
        storage_words(
            generated=end_generated,
            synced=end_synced,
            last_synced_seq=(seq_start + record_count - 1) % (1 << 32),
            last_synced_file=1 if end_synced else 0,
            last_synced_date=(
                int(oracle.utc_date_text(BASE_UTC))
                if end_synced and utc_valid
                else 0
            ),
            active_config_version=config_version,
        )
    )
    observations: list[dict[str, Any]] = [
        {
            "kind": "config_apply",
            "value": {
                "version": config_version,
                "period_s": period_s,
                "mask": mask,
                "sample_count": sample_count,
            },
        },
        {"kind": "storage", "value": first_storage},
    ]
    if utc_valid:
        observations.append(
            {
                "kind": "command",
                "value": {
                    "requested_utc_seconds": BASE_UTC,
                    "command": 6,
                    "result": 1,
                },
                "observed_utc": datetime.fromtimestamp(
                    BASE_UTC, tz=timezone.utc
                ).isoformat(),
                "observed_monotonic_s": 100.0,
            }
        )
    for index, record in enumerate(records):
        snapshot_observation: dict[str, Any] = {
            "kind": "snapshot",
            "value": {
                "session_id": session,
                "sequence": record["seq"],
                "trigger_code": record["trigger"],
                "channel_values": [
                    record[f"v{channel}"] for channel in range(4)
                ],
                "channel_qualities": [
                    record[f"q{channel}"] for channel in range(4)
                ],
            },
        }
        if utc_valid:
            snapshot_observation["observed_utc"] = datetime.fromtimestamp(
                record["utc_s"], tz=timezone.utc
            ).isoformat()
            snapshot_observation["observed_monotonic_s"] = (
                100.0 + (index * period_s)
            )
        observations.append(snapshot_observation)
        if index == 0 and include_repeated_poll:
            repeated = dict(snapshot_observation)
            repeated["value"] = dict(snapshot_observation["value"])
            observations.append(repeated)
    observations.append({"kind": "storage", "value": final_storage})
    write_jsonl(run_dir / "observations.jsonl", observations)
    write_jsonl(
        run_dir / "checkpoints.jsonl",
        [
            {
                "checkpoint": "start",
                "value": {"storage": first_storage},
            },
            {
                "checkpoint": "end",
                "value": {"storage": final_storage},
            },
        ],
    )
    write_json(
        run_dir / "result.json",
        {
            "format": "p5-persistence-run-result/1",
            "capture": "PASS",
            "end_to_end": "NOT_RUN",
            "exit_code": 3,
        },
    )
    return run_dir, files_dir


def rewrite_csv(
    files_dir: Path,
    relative_path: str,
    records: Iterable[Mapping[str, int]],
) -> str:
    data = csv_bytes(records)
    (files_dir / relative_path).write_bytes(data)
    return hashlib.sha256(data).hexdigest()
