"""Run identity, output ownership, and artifact manifest helpers."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
from typing import Any, Mapping
import uuid


FORMAL_IDENTITY_FORMAT = "p5-persistence-identity/1"
RUN_MANIFEST_FORMAT = "p5-persistence-run/1"


class ManifestError(ValueError):
    """Input identity or output ownership is not suitable for a formal run."""


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("ascii")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as exc:
        raise ManifestError(f"cannot read JSON {path}: {exc}") from exc


def write_json_atomic(path: Path, value: Any) -> None:
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(
                value,
                stream,
                ensure_ascii=True,
                sort_keys=True,
                indent=2,
            )
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _require_mapping(value: Any, name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ManifestError(f"{name} must be a JSON object")
    return value


def _require_nonempty_text(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ManifestError(f"{name} must be a non-empty string")
    return value.strip()


def _require_sha(value: Any, name: str, *, length: int) -> str:
    text = _require_nonempty_text(value, name)
    if len(text) != length or not re.fullmatch(r"[0-9a-fA-F]+", text):
        raise ManifestError(
            f"{name} must be exactly {length} hexadecimal characters"
        )
    return text.lower()


def _require_candidate(container: Mapping[str, Any], name: str) -> None:
    sha = container.get("candidate_sha")
    diff = container.get("candidate_diff_sha256")
    if sha is None and diff is None:
        raise ManifestError(
            f"{name} must provide candidate_sha or candidate_diff_sha256"
        )
    if sha is not None:
        _require_sha(sha, f"{name}.candidate_sha", length=40)
    if diff is not None:
        _require_sha(diff, f"{name}.candidate_diff_sha256", length=64)


def validate_identity(value: Any) -> dict[str, Any]:
    """Validate the minimum frozen product identity used by a formal run."""

    identity = dict(_require_mapping(value, "identity"))
    if identity.get("format") not in (None, FORMAL_IDENTITY_FORMAT):
        raise ManifestError(
            f"identity.format must be {FORMAL_IDENTITY_FORMAT!r} when present"
        )
    identity["format"] = FORMAL_IDENTITY_FORMAT
    identity["baseline_sha"] = _require_sha(
        identity.get("baseline_sha"),
        "identity.baseline_sha",
        length=40,
    )
    _require_candidate(identity, "identity")

    firmware = dict(
        _require_mapping(identity.get("firmware"), "identity.firmware")
    )
    firmware["elf_sha256"] = _require_sha(
        firmware.get("elf_sha256"),
        "identity.firmware.elf_sha256",
        length=64,
    )
    firmware["bin_sha256"] = _require_sha(
        firmware.get("bin_sha256"),
        "identity.firmware.bin_sha256",
        length=64,
    )
    firmware["build_config"] = _require_nonempty_text(
        firmware.get("build_config"),
        "identity.firmware.build_config",
    )
    if "candidate_sha" in firmware or "candidate_diff_sha256" in firmware:
        _require_candidate(firmware, "identity.firmware")
    identity["firmware"] = firmware

    cli = dict(_require_mapping(identity.get("cli"), "identity.cli"))
    cli["source_sha256"] = _require_sha(
        cli.get("source_sha256"),
        "identity.cli.source_sha256",
        length=64,
    )
    if "candidate_sha" in cli:
        cli["candidate_sha"] = _require_sha(
            cli.get("candidate_sha"),
            "identity.cli.candidate_sha",
            length=40,
        )
    identity["cli"] = cli

    contract = dict(
        _require_mapping(identity.get("contract"), "identity.contract")
    )
    if contract.get("protocol_version") != 3:
        raise ManifestError("identity.contract.protocol_version must be 3")
    if contract.get("contract_revision") != 1:
        raise ManifestError("identity.contract.contract_revision must be 1")
    if contract.get("csv_schema") != 1:
        raise ManifestError("identity.contract.csv_schema must be 1")
    identity["contract"] = {
        "protocol_version": 3,
        "contract_revision": 1,
        "csv_schema": 1,
    }
    return identity


def load_identity(path: Path) -> dict[str, Any]:
    return validate_identity(load_json(path))


def identity_sha256(identity: Mapping[str, Any]) -> str:
    return sha256_bytes(canonical_json_bytes(identity))


@dataclass(frozen=True)
class RunArtifacts:
    output_dir: Path
    manifest_path: Path
    commands_path: Path
    transactions_path: Path
    frames_path: Path
    observations_path: Path
    checkpoints_path: Path
    file_manifest_path: Path
    result_path: Path

    @classmethod
    def paths(cls, output_dir: Path) -> "RunArtifacts":
        return cls(
            output_dir=output_dir,
            manifest_path=output_dir / "manifest.json",
            commands_path=output_dir / "commands.jsonl",
            transactions_path=output_dir / "transactions.jsonl",
            frames_path=output_dir / "frames.jsonl",
            observations_path=output_dir / "observations.jsonl",
            checkpoints_path=output_dir / "checkpoints.jsonl",
            file_manifest_path=output_dir / "file_manifest.json",
            result_path=output_dir / "result.json",
        )

    def create(self, initial_manifest: Mapping[str, Any]) -> None:
        parent = self.output_dir.parent
        parent.mkdir(parents=True, exist_ok=True)
        try:
            self.output_dir.mkdir()
        except FileExistsError as exc:
            raise ManifestError(
                f"output directory already exists: {self.output_dir}"
            ) from exc
        write_json_atomic(self.manifest_path, dict(initial_manifest))
        for path in (
            self.commands_path,
            self.transactions_path,
            self.frames_path,
            self.observations_path,
            self.checkpoints_path,
        ):
            try:
                with path.open("x", encoding="utf-8"):
                    pass
            except OSError as exc:
                raise ManifestError(f"cannot create evidence file {path}: {exc}") from exc
        write_json_atomic(
            self.file_manifest_path,
            {
                "format": "p5-persistence-file-manifest/1",
                "status": "NOT_RUN",
                "reason": "run phase does not read or copy SD files",
                "files": [],
            },
        )
        write_json_atomic(
            self.result_path,
            {
                "format": "p5-persistence-run-result/1",
                "capture": "RUNNING",
                "end_to_end": "NOT_RUN",
                "exit_code": None,
                "reason": "run is still in progress",
            },
        )


def build_initial_manifest(
    *,
    run_id: str,
    scenario: str,
    identity: Mapping[str, Any],
    parameters: Mapping[str, Any],
    expected_duration_s: float,
    started_utc: str | None = None,
) -> dict[str, Any]:
    return {
        "format": RUN_MANIFEST_FORMAT,
        "run_id": run_id,
        "boot_epoch": run_id,
        "phase": "RUNNING",
        "capture": "RUNNING",
        "end_to_end": "NOT_RUN",
        "scenario": scenario,
        "identity": dict(identity),
        "identity_sha256": identity_sha256(identity),
        "parameters": dict(parameters),
        "environment": {
            "python": os.sys.version,
            "platform": os.uname().sysname,
            "machine": os.uname().machine,
        },
        "started_utc": started_utc or utc_now_iso(),
        "ended_utc": None,
        "expected_duration_s": expected_duration_s,
        "observed_duration_s": None,
        "completion_reason": None,
        "monotonic_origin_s": None,
        "monotonic_end_s": None,
    }
