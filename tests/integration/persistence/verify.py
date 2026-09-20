#!/usr/bin/env python3
"""Read-only verifier for P5 persistence run and copied SD evidence."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import sys
from typing import Any, Iterable, Mapping, Sequence


if __package__ in (None, ""):
    repository_root = Path(__file__).resolve().parents[3]
    if str(repository_root) not in sys.path:
        sys.path.insert(0, str(repository_root))
    package_parent = Path(__file__).resolve().parents[1]
    if str(package_parent) not in sys.path:
        sys.path.insert(0, str(package_parent))

from persistence import oracle
from persistence.manifest import (
    ManifestError,
    RUN_MANIFEST_FORMAT,
    identity_sha256,
    sha256_file,
    utc_now_iso,
    validate_identity,
    write_json_atomic,
)


EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_NOT_RUN = 2
EXIT_INCONCLUSIVE = 3

UTC_OBSERVATION_TOLERANCE_S = 3
SET_TIME_COMMAND = 6


class VerifyInputError(ValueError):
    pass


@dataclass
class Report:
    run_dir: str
    files_dir: str
    checks: list[dict[str, Any]] = field(default_factory=list)
    issues: list[dict[str, Any]] = field(default_factory=list)
    fatal: str | None = None

    def check(
        self,
        name: str,
        status: str,
        reason: str,
        **details: Any,
    ) -> None:
        self.checks.append(
            {
                "name": name,
                "status": status,
                "reason": reason,
                "details": details,
            }
        )

    def add_issue(
        self,
        code: str,
        message: str,
        *,
        status: str = "FAIL",
        path: str | None = None,
        **details: Any,
    ) -> None:
        issue = {
            "code": code,
            "status": status,
            "message": message,
        }
        if path is not None:
            issue["path"] = path
        issue.update(details)
        self.issues.append(issue)

    @property
    def overall(self) -> str:
        if self.fatal is not None:
            return "NOT_RUN"
        if any(issue["status"] == "FAIL" for issue in self.issues):
            return "FAIL"
        if any(issue["status"] == "INCONCLUSIVE" for issue in self.issues):
            return "INCONCLUSIVE"
        if any(check["status"] == "INCONCLUSIVE" for check in self.checks):
            return "INCONCLUSIVE"
        if any(check["status"] in ("FAIL", "NOT_RUN") for check in self.checks):
            return "FAIL"
        return "PASS"

    @property
    def exit_code(self) -> int:
        overall = self.overall
        if overall == "PASS":
            return EXIT_PASS
        if overall == "FAIL":
            return EXIT_FAIL
        if overall == "INCONCLUSIVE":
            return EXIT_INCONCLUSIVE
        return EXIT_NOT_RUN

    def as_result(self) -> dict[str, Any]:
        return {
            "format": "p5-persistence-verification-result/1",
            "verified_utc": utc_now_iso(),
            "run_dir": self.run_dir,
            "files_dir": self.files_dir,
            "overall": self.overall,
            "exit_code": self.exit_code,
            "fatal": self.fatal,
            "checks": self.checks,
            "issues": self.issues,
            "exit_code_semantics": {
                "0": "PASS",
                "1": "FAIL",
                "2": "input/environment NOT_RUN",
                "3": "PARTIAL/INCONCLUSIVE",
            },
        }


def _load_json(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as exc:
        raise VerifyInputError(f"cannot read {path}: {exc}") from exc


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        raise VerifyInputError(f"missing evidence file: {path}")
    rows: list[dict[str, Any]] = []
    try:
        with path.open("r", encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, start=1):
                if not line.strip():
                    continue
                try:
                    value = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise VerifyInputError(
                        f"{path}:{line_number}: invalid JSON: {exc}"
                    ) from exc
                if not isinstance(value, dict):
                    raise VerifyInputError(
                        f"{path}:{line_number}: JSONL row must be an object"
                    )
                rows.append(value)
    except OSError as exc:
        raise VerifyInputError(f"cannot read {path}: {exc}") from exc
    return rows


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _normalise_relative_path(value: Any) -> str:
    if not isinstance(value, str) or not value:
        raise VerifyInputError("file manifest entry has no path")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise VerifyInputError(f"unsafe file manifest path: {value!r}")
    return path.as_posix()


def _load_evidence(run_dir: Path) -> dict[str, Any]:
    if not run_dir.is_dir():
        raise VerifyInputError(f"run directory does not exist: {run_dir}")
    manifest = _load_json(run_dir / "manifest.json")
    if not isinstance(manifest, dict):
        raise VerifyInputError("manifest.json must contain an object")
    if manifest.get("format") != RUN_MANIFEST_FORMAT:
        raise VerifyInputError(
            f"manifest format must be {RUN_MANIFEST_FORMAT!r}"
        )
    file_manifest = _load_json(run_dir / "file_manifest.json")
    if not isinstance(file_manifest, dict):
        raise VerifyInputError("file_manifest.json must contain an object")
    return {
        "manifest": manifest,
        "file_manifest": file_manifest,
        "commands": _load_jsonl(run_dir / "commands.jsonl"),
        "transactions": _load_jsonl(run_dir / "transactions.jsonl"),
        "frames": _load_jsonl(run_dir / "frames.jsonl"),
        "observations": _load_jsonl(run_dir / "observations.jsonl"),
        "checkpoints": _load_jsonl(run_dir / "checkpoints.jsonl"),
    }


def _check_run_integrity(
    report: Report,
    manifest: Mapping[str, Any],
    checkpoints: Sequence[Mapping[str, Any]],
) -> None:
    scenario = manifest.get("scenario")
    phase = manifest.get("phase")
    capture = manifest.get("capture")
    expected = manifest.get("expected_duration_s")
    observed = manifest.get("observed_duration_s")
    reason = manifest.get("completion_reason")

    if phase != "COMPLETE" or capture != "PASS":
        report.check(
            "run_completion",
            "FAIL",
            f"phase={phase!r} capture={capture!r}; only a complete capture is verifiable",
        )
        report.add_issue(
            "half_run_as_complete",
            f"run is not marked COMPLETE/PASS: phase={phase!r} capture={capture!r}",
        )
    else:
        report.check("run_completion", "PASS", "capture is marked COMPLETE/PASS")

    if scenario not in ("short", "baseline"):
        report.check("scenario", "FAIL", f"unsupported scenario {scenario!r}")
        report.add_issue("scenario", f"unsupported scenario {scenario!r}")
    if not _is_number(expected) or expected <= 0:
        report.check("duration", "FAIL", "expected_duration_s is missing or invalid")
        report.add_issue(
            "duration_missing",
            "expected_duration_s must be a positive number",
        )
    elif not _is_number(observed):
        report.check("duration", "FAIL", "observed_duration_s is missing")
        report.add_issue(
            "half_run_as_complete",
            "observed_duration_s is missing but capture claims PASS",
        )
    elif scenario == "baseline" and observed + 0.001 < float(expected):
        report.check(
            "duration",
            "FAIL",
            f"baseline observed {observed}s is shorter than required {expected}s",
        )
        report.add_issue(
            "half_run_as_complete",
            f"observed {observed}s < expected {expected}s",
        )
    else:
        report.check(
            "duration",
            "PASS",
            f"observed {observed}s satisfies scenario {scenario}",
        )

    allowed_reasons = {"duration_elapsed", "finite_count_completed"}
    if reason not in allowed_reasons:
        report.check(
            "completion_reason",
            "FAIL",
            f"completion_reason={reason!r}",
        )
        report.add_issue(
            "half_run_as_complete",
            f"completion_reason={reason!r} does not prove normal completion",
        )
    if not manifest.get("boot_epoch"):
        report.check("boot_epoch", "FAIL", "boot_epoch is missing")
        report.add_issue("boot_epoch", "boot_epoch is missing")
    else:
        report.check("boot_epoch", "PASS", "run-scoped boot_epoch is present")

    labels = {
        str(row.get("checkpoint"))
        for row in checkpoints
        if isinstance(row, Mapping)
    }
    if not ({"start", "baseline_start"} & labels) or not (
        {"end", "final", "baseline_end"} & labels
    ):
        report.check(
            "checkpoints",
            "FAIL",
            f"required start/end checkpoints missing; saw {sorted(labels)}",
        )
        report.add_issue(
            "checkpoint_missing",
            "start and end checkpoints are required",
        )
    else:
        report.check("checkpoints", "PASS", "start and end checkpoints are present")

    try:
        identity = validate_identity(manifest.get("identity"))
    except ManifestError as exc:
        report.check("identity", "FAIL", str(exc))
        report.add_issue("identity", str(exc))
    else:
        expected_identity_hash = str(manifest.get("identity_sha256", "")).lower()
        if expected_identity_hash != identity_sha256(identity):
            report.check(
                "identity",
                "FAIL",
                "identity_sha256 does not match the identity object",
            )
            report.add_issue(
                "identity_hash",
                "identity_sha256 does not match the identity object",
            )
        else:
            report.check(
                "identity",
                "PASS",
                "frozen baseline/candidate/CLI/contract identity is present",
            )


def _manifest_file_entries(
    file_manifest: Mapping[str, Any],
) -> list[dict[str, Any]]:
    files = file_manifest.get("files")
    if not isinstance(files, list):
        raise VerifyInputError("file_manifest.json files must be a list")
    entries: list[dict[str, Any]] = []
    for index, item in enumerate(files):
        if not isinstance(item, dict):
            raise VerifyInputError(f"file manifest entry {index} must be an object")
        entry = dict(item)
        path_value = entry.get("path")
        if path_value is None:
            path_value = entry.get("relative_path")
        entry["path"] = _normalise_relative_path(path_value)
        entries.append(entry)
    return entries


def _scan_csv_files(files_dir: Path) -> set[str]:
    found: set[str] = set()
    for path in files_dir.rglob("*"):
        if not path.is_file() or path.suffix != ".CSV":
            continue
        relative = path.relative_to(files_dir).as_posix()
        if relative.startswith("LOG/"):
            found.add(relative)
    return found


def _verify_file_manifest(
    report: Report,
    files_dir: Path,
    entries: Sequence[Mapping[str, Any]],
) -> tuple[list[Mapping[str, Any]], list[oracle.ParsedCsv]]:
    issue_start = len(report.issues)
    listed_paths = [str(entry["path"]) for entry in entries]
    if len(set(listed_paths)) != len(listed_paths):
        report.check("file_manifest_unique", "FAIL", "duplicate manifest paths")
        report.add_issue(
            "file_manifest_duplicate",
            "file_manifest.json lists the same path more than once",
        )
    else:
        report.check("file_manifest_unique", "PASS", "manifest paths are unique")

    actual_paths = _scan_csv_files(files_dir)
    listed_set = set(listed_paths)
    missing_listing = sorted(actual_paths - listed_set)
    for path in missing_listing:
        report.add_issue(
            "file_manifest_missing_entry",
            f"CSV {path!r} exists under --files but is absent from the manifest",
            path=path,
        )
    missing_files = sorted(listed_set - actual_paths)
    for path in missing_files:
        report.add_issue(
            "file_missing",
            f"manifest file {path!r} is missing from --files",
            path=path,
        )

    verified_entries: list[Mapping[str, Any]] = []
    parsed_files: list[oracle.ParsedCsv] = []
    for entry in entries:
        relative = str(entry["path"])
        absolute = files_dir / relative
        if not absolute.is_file():
            continue
        actual_size = absolute.stat().st_size
        actual_hash = sha256_file(absolute)
        expected_size = entry.get("size")
        expected_hash = entry.get("sha256")
        if expected_size is not None and expected_size != actual_size:
            report.add_issue(
                "file_size",
                (
                    f"{relative}: size {actual_size} does not match "
                    f"manifest {expected_size}"
                ),
                path=relative,
            )
        if expected_hash is not None and str(expected_hash).lower() != actual_hash:
            report.add_issue(
                "file_hash",
                (
                    f"{relative}: SHA-256 {actual_hash} does not match "
                    f"manifest {expected_hash}"
                ),
                path=relative,
            )
        if expected_size is None or expected_hash is None:
            report.add_issue(
                "file_manifest_incomplete",
                f"{relative}: manifest must contain size and sha256",
                path=relative,
            )
        verified_entries.append(
            {
                **entry,
                "actual_size": actual_size,
                "actual_sha256": actual_hash,
            }
        )
        try:
            data = absolute.read_bytes()
            parsed = oracle.parse_csv_bytes(
                data,
                relative,
                expected_sha256=(
                    str(expected_hash).lower() if expected_hash is not None else None
                ),
            )
            parsed_files.append(parsed)
        except (OSError, oracle.CsvContractError) as exc:
            if isinstance(exc, oracle.CsvContractError):
                code = exc.code
                message = exc.message
                offset = exc.offset
            else:
                code = "file_read"
                message = str(exc)
                offset = None
            report.add_issue(
                code,
                f"{relative}: {message}",
                path=relative,
                offset=offset,
            )

    if len(report.issues) > issue_start:
        report.check(
            "file_manifest",
            "FAIL",
            f"{len(report.issues)} file-list/hash/format issue(s)",
        )
    else:
        report.check(
            "file_manifest",
            "PASS",
            f"{len(parsed_files)} listed CSV file(s) match size and SHA-256",
        )
    return verified_entries, parsed_files


def _config_versions_from_observations(
    report: Report,
    observations: Sequence[Mapping[str, Any]],
) -> dict[int, dict[str, int]]:
    versions: dict[int, dict[str, int]] = {}
    for observation in observations:
        if observation.get("kind") != "config_apply":
            continue
        value = observation.get("value")
        if not isinstance(value, Mapping):
            report.add_issue(
                "config_evidence",
                "config_apply observation has no object value",
            )
            continue
        config = value.get("config", value)
        if not isinstance(config, Mapping):
            report.add_issue(
                "config_evidence",
                "config_apply observation config is not an object",
            )
            continue
        try:
            version = int(config["version"])
            period = int(config["period_s"])
            mask = int(config["mask"])
            sample_count = int(config["sample_count"])
        except (KeyError, TypeError, ValueError):
            report.add_issue(
                "config_evidence",
                "config_apply observation is missing version/period/mask/count",
            )
            continue
        candidate = {
            "period_s": period,
            "mask": mask,
            "sample_count": sample_count,
        }
        existing = versions.get(version)
        if existing is not None and existing != candidate:
            report.add_issue(
                "config_version_evidence",
                f"config version {version} has conflicting evidence",
                version=version,
                previous=existing,
                current=candidate,
            )
        else:
            versions[version] = candidate
    if not versions:
        report.check(
            "config_mapping",
            "INCONCLUSIVE",
            "no successful config_apply observation is available",
        )
    else:
        report.check(
            "config_mapping",
            "PASS",
            f"recovered {len(versions)} config version mapping(s)",
        )
    return versions


def _storage_observations(
    observations: Sequence[Mapping[str, Any]],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for observation in observations:
        if observation.get("kind") != "storage":
            continue
        value = observation.get("value")
        if not isinstance(value, Mapping):
            continue
        if "generated" not in value or "synced" not in value:
            continue
        rows.append(dict(value))
    return rows


def _verify_storage(
    report: Report,
    observations: Sequence[Mapping[str, Any]],
    checkpoints: Sequence[Mapping[str, Any]],
) -> tuple[Mapping[str, int] | None, Mapping[str, int] | None]:
    storage_rows = _storage_observations(observations)
    if len(storage_rows) < 2:
        report.check(
            "storage",
            "INCONCLUSIVE",
            "at least two storage observations are required",
        )
        report.add_issue(
            "storage_evidence",
            "at least two same-response storage observations are required",
            status="INCONCLUSIVE",
        )
        return None, None
    storage_issue_codes: set[str] = set()
    for index, storage in enumerate(storage_rows):
        for issue in oracle.validate_storage_block(storage):
            storage_issue_codes.add(issue["code"])
            report.add_issue(
                issue["code"],
                f"storage observation {index}: {issue['message']}",
            )
    first = None
    for checkpoint in checkpoints:
        if checkpoint.get("checkpoint") not in ("start", "baseline_start"):
            continue
        value = checkpoint.get("value")
        if not isinstance(value, Mapping):
            continue
        storage = value.get("storage")
        if isinstance(storage, Mapping) and "generated" in storage:
            first = dict(storage)
            break
    if first is None:
        report.check(
            "storage_start_checkpoint",
            "FAIL",
            "start checkpoint has no storage block",
        )
        report.add_issue(
            "storage_start_checkpoint",
            "start checkpoint must contain the post-STOP/post-apply storage block",
        )
        first = storage_rows[0]
    else:
        report.check(
            "storage_start_checkpoint",
            "PASS",
            "start counters come from the post-STOP/post-apply checkpoint",
        )
        for issue in oracle.validate_storage_block(first):
            report.add_issue(
                issue["code"],
                f"storage start checkpoint: {issue['message']}",
            )
    last = storage_rows[-1]
    report.check(
        "storage",
        "PASS" if not storage_issue_codes else "FAIL",
        f"used {len(storage_rows)} storage observations",
    )
    return first, last


def _verify_records(
    report: Report,
    parsed_files: Sequence[oracle.ParsedCsv],
    config_versions: Mapping[int, Mapping[str, int]],
) -> list[oracle.CsvRecord]:
    issue_start = len(report.issues)
    all_records: list[oracle.CsvRecord] = []
    for parsed in parsed_files:
        for issue in oracle.validate_file_path(parsed.relative_path, parsed.records):
            report.add_issue(
                issue["code"],
                f"{parsed.relative_path}: {issue['message']}",
                path=parsed.relative_path,
            )
        for issue in oracle.records_are_contiguous(parsed.records):
            report.add_issue(
                issue["code"],
                f"{parsed.relative_path}: {issue['message']}",
                path=parsed.relative_path,
            )
        for record_index, record in enumerate(parsed.records):
            for issue in oracle.validate_record_contract(
                record,
                config_versions=config_versions,
            ):
                status = "INCONCLUSIVE" if issue.get("inconclusive") else "FAIL"
                report.add_issue(
                    issue["code"],
                    (
                        f"{parsed.relative_path}:{record_index + 2}: "
                        f"{issue['message']}"
                    ),
                    status=status,
                    path=parsed.relative_path,
                    record_index=record_index,
                )
        all_records.extend(parsed.records)
    for issue in oracle.duplicate_identity_issues(all_records):
        report.add_issue(issue["code"], issue["message"])
    if all_records:
        report.check(
            "records",
            "PASS" if len(report.issues) == issue_start else "FAIL",
            f"parsed and checked {len(all_records)} CSV record(s)",
        )
    else:
        report.check(
            "records",
            "INCONCLUSIVE",
            "the listed files contain no records",
        )
    return all_records


def _verify_timeline(
    report: Report,
    parsed_files: Sequence[oracle.ParsedCsv],
    observations: Sequence[Mapping[str, Any]],
) -> None:
    timeline_start = len(report.issues)
    record_count = 0
    for parsed in parsed_files:
        record_count += len(parsed.records)
        for issue in oracle.validate_sampling_timeline(parsed.records):
            report.add_issue(
                issue["code"],
                f"{parsed.relative_path}: {issue['message']}",
                path=parsed.relative_path,
            )
    timeline_issues = report.issues[timeline_start:]
    report.check(
        "sampling_timeline",
        "FAIL" if timeline_issues else "PASS",
        (
            f"checked {record_count} record(s) for planned/actual ordering, "
            "period steps and single semantics"
        ),
    )

    utc_start = len(report.issues)
    observed_monotonic_by_seq: dict[int, float] = {}
    for observation in observations:
        if observation.get("kind") != "snapshot":
            continue
        value = observation.get("value")
        if not isinstance(value, Mapping):
            continue
        sequence = value.get("sequence")
        monotonic = observation.get("observed_monotonic_s")
        if isinstance(sequence, int) and isinstance(monotonic, (int, float)):
            observed_monotonic_by_seq.setdefault(sequence, float(monotonic))

    # Every successful SET_TIME starts a new anchor segment: the device clock
    # is user-set, so records are compared against the anchor in force at
    # their observation time instead of the host wall clock.
    anchors: list[tuple[float, int]] = []
    for observation in observations:
        value = observation.get("value")
        if not isinstance(value, Mapping):
            continue
        if "requested_utc_seconds" not in value:
            continue
        if value.get("command") != SET_TIME_COMMAND or value.get("result") != 1:
            continue
        monotonic = observation.get("observed_monotonic_s")
        if not isinstance(monotonic, (int, float)):
            continue
        try:
            requested_utc = int(value["requested_utc_seconds"])
        except (TypeError, ValueError):
            continue
        anchors.append((float(monotonic), requested_utc))
    anchors.sort(key=lambda anchor: anchor[0])

    def active_anchor(monotonic: float) -> tuple[float, int] | None:
        selected: tuple[float, int] | None = None
        for anchor in anchors:
            if anchor[0] > monotonic:
                break
            selected = anchor
        return selected

    has_utc_records = any(
        record.utc_valid == 1 for parsed in parsed_files for record in parsed.records
    )

    checked = 0
    missing = 0
    unanchored = 0
    for parsed in parsed_files:
        previous: oracle.CsvRecord | None = None
        previous_anchor: tuple[float, int] | None = None
        for record_index, record in enumerate(parsed.records):
            if record.utc_valid != 1:
                previous = None
                previous_anchor = None
                continue
            monotonic = observed_monotonic_by_seq.get(record.seq)
            if monotonic is None:
                missing += 1
                previous = None
                previous_anchor = None
                continue
            anchor = active_anchor(monotonic)
            if anchor is None:
                unanchored += 1
                previous = None
                previous_anchor = None
                continue
            expected_utc = anchor[1] + int(monotonic - anchor[0])
            checked += 1
            if abs(record.utc_s - expected_utc) > UTC_OBSERVATION_TOLERANCE_S:
                report.add_issue(
                    "utc_host_mapping",
                    (
                        f"{parsed.relative_path}:{record_index + 2}: utc_s="
                        f"{record.utc_s} differs from the SET_TIME-anchored "
                        f"expectation {expected_utc} by more than "
                        f"{UTC_OBSERVATION_TOLERANCE_S} s"
                    ),
                    path=parsed.relative_path,
                    record_index=record_index,
                )
            # Device-internal consistency only holds inside one SET_TIME
            # segment; a legal re-set makes the step across the boundary
            # meaningless.
            if previous is not None and previous_anchor == anchor:
                utc_step = record.utc_s - previous.utc_s
                sample_step = (record.actual_ms - previous.actual_ms) // 1000
                if abs(utc_step - sample_step) > 1:
                    report.add_issue(
                        "utc_step",
                        (
                            f"{parsed.relative_path}:{record_index + 2}: UTC "
                            f"advanced {utc_step} s while the sampling clock "
                            f"advanced {sample_step} s"
                        ),
                        path=parsed.relative_path,
                        record_index=record_index,
                    )
            previous = record
            previous_anchor = anchor

    if has_utc_records and not anchors:
        report.add_issue(
            "utc_anchor_evidence",
            (
                "UTC-valid records exist but no successful SET_TIME anchor with "
                "a monotonic observation is available"
            ),
            status="INCONCLUSIVE",
        )
    elif unanchored:
        report.add_issue(
            "utc_anchor_evidence",
            (
                f"{unanchored} UTC-valid record(s) have no preceding SET_TIME "
                "anchor"
            ),
            status="INCONCLUSIVE",
        )
    if missing != 0:
        report.add_issue(
            "utc_mapping_evidence",
            (
                f"{missing} UTC-valid record(s) have no matching snapshot "
                "observation with a monotonic timestamp"
            ),
            status="INCONCLUSIVE",
        )
    utc_issues = report.issues[utc_start:]
    if any(issue.get("status") == "FAIL" for issue in utc_issues):
        utc_status = "FAIL"
    elif utc_issues:
        utc_status = "INCONCLUSIVE"
    else:
        utc_status = "PASS"
    report.check(
        "utc_mapping",
        utc_status,
        (
            f"cross-checked {checked} UTC-valid record(s) against "
            f"{len(anchors)} SET_TIME anchor(s); {missing} without matching "
            "evidence"
        ),
    )


def _snapshot_identity(
    observation: Mapping[str, Any],
) -> tuple[int | None, int | None]:
    value = observation.get("value")
    if not isinstance(value, Mapping):
        return None, None
    session_raw = value.get("session_id", value.get("session"))
    sequence_raw = value.get("sequence", value.get("seq"))
    try:
        session = int(session_raw) if session_raw is not None else None
        sequence = int(sequence_raw) if sequence_raw is not None else None
    except (TypeError, ValueError):
        return None, None
    return session, sequence


def _snapshot_payload(value: Mapping[str, Any]) -> dict[str, Any]:
    payload: dict[str, Any] = {}
    if "trigger_code" in value:
        payload["trigger"] = value.get("trigger_code")
    if isinstance(value.get("channel_values"), list):
        payload["values"] = value["channel_values"]
    if isinstance(value.get("channel_qualities"), list):
        payload["qualities"] = value["channel_qualities"]
    channels = value.get("channels")
    if isinstance(channels, list):
        values: list[int] = []
        qualities: list[int] = []
        for channel in channels:
            if isinstance(channel, Mapping):
                values.append(int(channel.get("value", 0)))
                qualities.append(int(channel.get("quality_code", 0)))
        if len(values) == 4:
            payload.setdefault("values", values)
            payload.setdefault("qualities", qualities)
    return payload


def _verify_observation_cross_checks(
    report: Report,
    observations: Sequence[Mapping[str, Any]],
    records: Sequence[oracle.CsvRecord],
) -> None:
    issue_start = len(report.issues)
    by_key = {(record.session, record.seq): record for record in records}
    sessions = {record.session for record in records}
    checked = 0
    for observation in observations:
        if observation.get("kind") != "snapshot":
            continue
        value = observation.get("value")
        if not isinstance(value, Mapping):
            report.add_issue(
                "snapshot_shape",
                "snapshot observation has no object value",
            )
            continue
        session, sequence = _snapshot_identity(observation)
        if sequence is None:
            report.add_issue(
                "snapshot_shape",
                "snapshot observation has no sequence",
            )
            continue
        record = by_key.get((session if session is not None else -1, sequence))
        if record is None and session is None and len(sessions) == 1:
            record = by_key.get((next(iter(sessions)), sequence))
        if record is None:
            report.add_issue(
                "snapshot_missing_file",
                (
                    f"snapshot session={session} seq={sequence} has no "
                    "corresponding CSV record"
                ),
            )
            continue
        payload = _snapshot_payload(value)
        if payload.get("trigger") is not None and int(payload["trigger"]) != record.trigger:
            report.add_issue(
                "snapshot_trigger",
                (
                    f"session={record.session} seq={record.seq}: snapshot trigger "
                    f"{payload['trigger']} != file {record.trigger}"
                ),
            )
        values = payload.get("values")
        qualities = payload.get("qualities")
        if values is not None:
            if list(values) != [record.v0, record.v1, record.v2, record.v3]:
                report.add_issue(
                    "snapshot_channel_value",
                    f"session={record.session} seq={record.seq}: channel values differ",
                    snapshot=list(values),
                    file=[record.v0, record.v1, record.v2, record.v3],
                )
        if qualities is not None:
            if list(qualities) != [record.q0, record.q1, record.q2, record.q3]:
                report.add_issue(
                    "snapshot_channel_quality",
                    f"session={record.session} seq={record.seq}: channel qualities differ",
                    snapshot=list(qualities),
                    file=[record.q0, record.q1, record.q2, record.q3],
                )
        checked += 1
    if len(report.issues) > issue_start:
        status = "FAIL"
    elif checked:
        status = "PASS"
    else:
        status = "INCONCLUSIVE"
    report.check(
        "snapshot_cross_check",
        status,
        f"cross-checked {checked} repeated snapshot observation(s)",
    )


def _verify_last_synced_identity(
    report: Report,
    last_storage: Mapping[str, int] | None,
    records: Sequence[oracle.CsvRecord],
) -> None:
    if last_storage is None or last_storage["synced"] == 0:
        report.check(
            "last_synced_identity",
            "PASS",
            "no successful sync is claimed",
        )
        return
    candidates = [
        record for record in records if record.seq == last_storage["last_synced_seq"]
    ]
    if not candidates:
        report.check(
            "last_synced_identity",
            "FAIL",
            "last_synced_seq has no matching file record",
        )
        report.add_issue(
            "last_synced_identity",
            (
                f"last_synced_seq={last_storage['last_synced_seq']} is absent "
                "from the file records"
            ),
        )
        return
    expected_file = last_storage["last_synced_file"]
    expected_date = last_storage["last_synced_date"]
    matching = [
        record
        for record in candidates
        if record.file_id == expected_file and record.file_date == expected_date
    ]
    if not matching:
        report.check(
            "last_synced_identity",
            "FAIL",
            "last synced file identity does not match the CSV path identity",
        )
        report.add_issue(
            "last_synced_identity",
            (
                f"last_synced_file/date={expected_file}/{expected_date} does "
                "not match the matching sequence record"
            ),
        )
    else:
        report.check(
            "last_synced_identity",
            "PASS",
            "last synced sequence and file identity match the CSV record",
        )


def _verify_counts(
    report: Report,
    *,
    scenario: str,
    first_storage: Mapping[str, int] | None,
    last_storage: Mapping[str, int] | None,
    records: Sequence[oracle.CsvRecord],
) -> None:
    if first_storage is None or last_storage is None:
        report.check("counts", "INCONCLUSIVE", "storage checkpoints are missing")
        return
    generated = oracle.u32_delta(last_storage["generated"], first_storage["generated"])
    synced = oracle.u32_delta(last_storage["synced"], first_storage["synced"])
    dropped = oracle.u32_delta(last_storage["dropped"], first_storage["dropped"])
    uncertain = oracle.u32_delta(
        last_storage["uncertain"],
        first_storage["uncertain"],
    )
    file_count = len(records)

    count_issues: list[dict[str, Any]] = []

    def count_issue(code: str, message: str) -> None:
        count_issues.append({"code": code, "message": message})
        report.add_issue(code, message)

    if generated != file_count:
        count_issue(
            "generated_file_count",
            f"generated delta {generated} != parsed file records {file_count}",
        )
    if synced != file_count:
        count_issue(
            "sync_file_count_mismatch",
            f"synced delta {synced} != parsed file records {file_count}",
        )

    normal_scenario = not scenario.startswith("fault")
    if normal_scenario and (dropped != 0 or uncertain != 0):
        count_issue(
            "normal_run_loss",
            f"normal run dropped={dropped} uncertain={uncertain}",
        )
    if normal_scenario and (
        last_storage["queued"] != 0
        or last_storage["in_flight"] != 0
        or last_storage["drain_state"] not in (0, 2)
    ):
        count_issue(
            "normal_run_not_drained",
            (
                f"queued={last_storage['queued']} "
                f"in_flight={last_storage['in_flight']} "
                f"drain_state={last_storage['drain_state']}"
            ),
        )
    if count_issues:
        report.check(
            "counts",
            "FAIL",
            (
                f"generated={generated} synced={synced} dropped={dropped} "
                f"uncertain={uncertain} files={file_count}"
            ),
        )
    else:
        report.check(
            "counts",
            "PASS",
            (
                f"generated=synced=files={file_count}; dropped=uncertain=0; "
                "queue drained"
            ),
        )


def verify_run(run_dir: Path, files_dir: Path, output_dir: Path) -> Report:
    if not files_dir.is_dir():
        raise VerifyInputError(f"files directory does not exist: {files_dir}")
    if output_dir.exists():
        raise VerifyInputError(f"output directory already exists: {output_dir}")
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    try:
        output_dir.mkdir()
    except FileExistsError as exc:
        raise VerifyInputError(f"output directory already exists: {output_dir}") from exc

    report = Report(run_dir=str(run_dir), files_dir=str(files_dir))
    try:
        evidence = _load_evidence(run_dir)
    except VerifyInputError as exc:
        report.fatal = str(exc)
        report.check("inputs", "NOT_RUN", str(exc))
        write_json_atomic(output_dir / "result.json", report.as_result())
        return report

    manifest = evidence["manifest"]
    _check_run_integrity(report, manifest, evidence["checkpoints"])
    try:
        entries = _manifest_file_entries(evidence["file_manifest"])
    except VerifyInputError as exc:
        report.fatal = str(exc)
        report.check("file_manifest", "NOT_RUN", str(exc))
        write_json_atomic(output_dir / "result.json", report.as_result())
        return report
    if not entries:
        report.fatal = "file_manifest.json contains no files"
        report.check("file_manifest", "NOT_RUN", report.fatal)
        write_json_atomic(output_dir / "result.json", report.as_result())
        return report

    _, parsed_files = _verify_file_manifest(report, files_dir, entries)
    config_versions = _config_versions_from_observations(
        report,
        evidence["observations"],
    )
    records = _verify_records(report, parsed_files, config_versions)
    _verify_timeline(report, parsed_files, evidence["observations"])
    _verify_observation_cross_checks(
        report,
        evidence["observations"],
        records,
    )
    first_storage, last_storage = _verify_storage(
        report,
        evidence["observations"],
        evidence["checkpoints"],
    )
    _verify_counts(
        report,
        scenario=str(manifest.get("scenario")),
        first_storage=first_storage,
        last_storage=last_storage,
        records=records,
    )
    _verify_last_synced_identity(report, last_storage, records)

    write_json_atomic(output_dir / "result.json", report.as_result())
    return report


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Read a completed P5 run directory and copied LOG CSV files, "
            "then independently verify schema 1, payloads, counters, and hashes."
        )
    )
    parser.add_argument(
        "--run",
        required=True,
        type=Path,
        help="completed run evidence directory containing manifest.json",
    )
    parser.add_argument(
        "--files",
        required=True,
        type=Path,
        help="read-only copy root containing LOG/... CSV files",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="new verification output directory; must not exist",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        report = verify_run(args.run, args.files, args.output)
    except (VerifyInputError, ManifestError) as exc:
        print(
            json.dumps(
                {
                    "format": "p5-persistence-verification-result/1",
                    "overall": "NOT_RUN",
                    "exit_code": EXIT_NOT_RUN,
                    "error": str(exc),
                },
                sort_keys=True,
            )
        )
        return EXIT_NOT_RUN
    print(json.dumps(report.as_result(), sort_keys=True))
    return report.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
