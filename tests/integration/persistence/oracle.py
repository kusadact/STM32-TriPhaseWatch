"""Independent CSV schema, payload, and storage-count oracle."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import PurePosixPath
import re
from typing import Any, Iterable, Mapping, Sequence


CSV_COLUMNS = (
    "schema",
    "session",
    "seq",
    "trigger",
    "planned_ms",
    "actual_ms",
    "utc_valid",
    "utc_s",
    "config_version",
    "period_s",
    "mask",
    "sample_count",
    "source",
    "v0",
    "v1",
    "v2",
    "v3",
    "u0",
    "u1",
    "u2",
    "u3",
    "q0",
    "q1",
    "q2",
    "q3",
    "file_id",
    "file_date",
    "reserved",
)
CSV_HEADER = ",".join(CSV_COLUMNS)

U16_FIELDS = {
    "schema",
    "trigger",
    "utc_valid",
    "period_s",
    "mask",
    "sample_count",
    "source",
    "v0",
    "v1",
    "v2",
    "v3",
    "u0",
    "u1",
    "u2",
    "u3",
    "q0",
    "q1",
    "q2",
    "q3",
    "reserved",
}
U32_FIELDS = {
    "session",
    "seq",
    "utc_s",
    "config_version",
    "file_id",
    "file_date",
}
U64_FIELDS = {"planned_ms", "actual_ms"}

MASK_CHANNELS = 0x000F
PERIOD_MIN_SEC = 10
PERIOD_MAX_SEC = 3600
U32_MODULUS = 1 << 32

STORAGE_START = 0x0080
STORAGE_COUNT = 0x0030
TIME_STATUS_START = 0x0016
TIME_STATUS_COUNT = 0x0008


class CsvContractError(ValueError):
    def __init__(self, code: str, message: str, *, offset: int | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.offset = offset


@dataclass(frozen=True)
class CsvRecord:
    schema: int
    session: int
    seq: int
    trigger: int
    planned_ms: int
    actual_ms: int
    utc_valid: int
    utc_s: int
    config_version: int
    period_s: int
    mask: int
    sample_count: int
    source: int
    v0: int
    v1: int
    v2: int
    v3: int
    u0: int
    u1: int
    u2: int
    u3: int
    q0: int
    q1: int
    q2: int
    q3: int
    file_id: int
    file_date: int
    reserved: int

    @property
    def values(self) -> tuple[int, ...]:
        return tuple(getattr(self, name) for name in CSV_COLUMNS)

    @property
    def payload_key(self) -> tuple[Any, ...]:
        return (
            self.session,
            self.seq,
            self.trigger,
            self.planned_ms,
            self.actual_ms,
            self.utc_valid,
            self.utc_s,
            self.config_version,
            self.period_s,
            self.mask,
            self.sample_count,
            self.source,
            self.v0,
            self.v1,
            self.v2,
            self.v3,
            self.u0,
            self.u1,
            self.u2,
            self.u3,
            self.q0,
            self.q1,
            self.q2,
            self.q3,
        )


@dataclass(frozen=True)
class ParsedCsv:
    relative_path: str
    header_line: bytes
    records: tuple[CsvRecord, ...]
    byte_count: int
    sha256: str


def is_ascii_decimal(value: bytes) -> bool:
    return bool(value) and all(0x30 <= byte <= 0x39 for byte in value)


def _parse_unsigned(
    raw: bytes,
    field: str,
    *,
    line_number: int,
    offset: int,
) -> int:
    if not is_ascii_decimal(raw):
        raise CsvContractError(
            "non_decimal_field",
            f"line {line_number} field {field!r} is not unsigned decimal",
            offset=offset,
        )
    value = int(raw)
    if field in U16_FIELDS and value > 0xFFFF:
        raise CsvContractError(
            "field_width",
            f"line {line_number} field {field!r} exceeds u16",
            offset=offset,
        )
    if field in U32_FIELDS and value > 0xFFFFFFFF:
        raise CsvContractError(
            "field_width",
            f"line {line_number} field {field!r} exceeds u32",
            offset=offset,
        )
    if field in U64_FIELDS and value > 0xFFFFFFFFFFFFFFFF:
        raise CsvContractError(
            "field_width",
            f"line {line_number} field {field!r} exceeds u64",
            offset=offset,
        )
    return value


def parse_csv_bytes(
    data: bytes,
    relative_path: str,
    *,
    expected_sha256: str | None = None,
) -> ParsedCsv:
    if not data:
        raise CsvContractError("empty_file", "CSV file is empty")
    if b"\r" in data:
        raise CsvContractError("non_lf_line_end", "CSV contains CR bytes")
    if not data.endswith(b"\n"):
        raise CsvContractError(
            "truncated_tail",
            "CSV does not end with LF and may be truncated",
            offset=len(data),
        )
    try:
        data.decode("ascii")
    except UnicodeDecodeError as exc:
        raise CsvContractError(
            "non_ascii",
            "CSV contains non-ASCII bytes",
            offset=exc.start,
        ) from exc

    lines = data[:-1].split(b"\n")
    if not lines:
        raise CsvContractError("empty_file", "CSV file is empty")
    if lines[0] != CSV_HEADER.encode("ascii"):
        raise CsvContractError(
            "header_mismatch",
            "CSV header does not match schema 1 exactly",
            offset=0,
        )

    records: list[CsvRecord] = []
    byte_offset = len(lines[0]) + 1
    for line_number, line in enumerate(lines[1:], start=2):
        if line == CSV_HEADER.encode("ascii"):
            raise CsvContractError(
                "duplicate_header",
                f"duplicate header at line {line_number}",
                offset=byte_offset,
            )
        fields = line.split(b",")
        if len(fields) != len(CSV_COLUMNS):
            raise CsvContractError(
                "column_count",
                (
                    f"line {line_number} has {len(fields)} columns, "
                    f"expected {len(CSV_COLUMNS)}"
                ),
                offset=byte_offset,
            )
        values: dict[str, int] = {}
        local_offset = byte_offset
        for field, raw in zip(CSV_COLUMNS, fields):
            values[field] = _parse_unsigned(
                raw,
                field,
                line_number=line_number,
                offset=local_offset,
            )
            local_offset += len(raw) + 1
        records.append(CsvRecord(**values))
        byte_offset += len(line) + 1

    if expected_sha256 is not None:
        import hashlib

        actual = hashlib.sha256(data).hexdigest()
        if actual != expected_sha256:
            raise CsvContractError(
                "file_hash",
                f"file SHA-256 is {actual}, expected {expected_sha256}",
            )
    import hashlib

    return ParsedCsv(
        relative_path=relative_path,
        header_line=lines[0],
        records=tuple(records),
        byte_count=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
    )


def validate_file_path(relative_path: str, records: Sequence[CsvRecord]) -> list[dict[str, Any]]:
    issues: list[dict[str, Any]] = []
    path = PurePosixPath(relative_path)
    if path.is_absolute() or ".." in path.parts:
        return [
            {
                "code": "unsafe_file_path",
                "message": f"unsafe relative path {relative_path!r}",
            }
        ]
    if len(path.parts) != 3 or path.parts[0] != "LOG":
        return [
            {
                "code": "file_path_shape",
                "message": f"path {relative_path!r} is not LOG/<date>/<id>.CSV",
            }
        ]
    path_date_text, filename = path.parts[1], path.parts[2]
    if path_date_text == "UNSET":
        path_date = 0
    elif re.fullmatch(r"[0-9]{8}", path_date_text):
        path_date = int(path_date_text)
    else:
        return [
            {
                "code": "file_path_date",
                "message": f"invalid date directory {path_date_text!r}",
            }
        ]
    match = re.fullmatch(r"([0-9A-F]{8})\.CSV", filename)
    if not match:
        return [
            {
                "code": "file_path_name",
                "message": f"invalid CSV filename {filename!r}",
            }
        ]
    path_id = int(match.group(1), 16)
    for index, record in enumerate(records):
        if record.file_id != path_id:
            issues.append(
                {
                    "code": "file_id_mismatch",
                    "message": (
                        f"record {index} file_id={record.file_id} does not match "
                        f"path id={path_id}"
                    ),
                }
            )
        if record.file_date != path_date:
            issues.append(
                {
                    "code": "file_date_mismatch",
                    "message": (
                        f"record {index} file_date={record.file_date} does not match "
                        f"path date={path_date}"
                    ),
                }
            )
    return issues


def config_is_valid(period_s: int, mask: int, sample_count: int) -> bool:
    return (
        PERIOD_MIN_SEC <= period_s <= PERIOD_MAX_SEC
        and 1 <= mask <= MASK_CHANNELS
        and 0 <= sample_count <= 0xFFFF
    )


def expected_test_value(seq: int, channel_index: int) -> int:
    return ((seq * 10) + channel_index) & 0xFFFF


def utc_date_text(utc_s: int) -> str:
    return datetime.fromtimestamp(utc_s, timezone.utc).strftime("%Y%m%d")


def validate_record_contract(
    record: CsvRecord,
    *,
    config_versions: Mapping[int, Mapping[str, int]] | None,
) -> list[dict[str, Any]]:
    issues: list[dict[str, Any]] = []

    def issue(code: str, message: str) -> None:
        issues.append({"code": code, "message": message})

    if record.schema != 1:
        issue("schema", f"schema={record.schema}, expected 1")
    if record.reserved != 0:
        issue("reserved", f"reserved={record.reserved}, expected 0")
    if record.trigger not in (1, 2):
        issue("trigger", f"trigger={record.trigger}, expected 1 or 2")
    if record.source != 1:
        issue("source", f"source={record.source}, expected TEST=1")
    if record.actual_ms < record.planned_ms:
        issue(
            "timestamp_order",
            f"actual_ms={record.actual_ms} is earlier than planned_ms={record.planned_ms}",
        )
    if record.utc_valid not in (0, 1):
        issue("utc_valid", f"utc_valid={record.utc_valid}, expected 0 or 1")
    elif record.utc_valid == 0:
        if record.utc_s != 0:
            issue(
                "utc_contradiction",
                f"utc_valid=0 requires utc_s=0, got {record.utc_s}",
            )
        if record.file_date != 0:
            issue(
                "unset_file_date",
                f"utc_valid=0 requires file_date=0, got {record.file_date}",
            )
    else:
        if record.utc_s == 0xFFFFFFFF:
            issue(
                "utc_contradiction",
                "utc_valid=1 cannot use the reserved UTC sentinel",
            )
        else:
            expected_date = int(utc_date_text(record.utc_s))
            if record.file_date != expected_date:
                issue(
                    "utc_file_date",
                    (
                        f"UTC {record.utc_s} belongs to {expected_date}, "
                        f"file_date={record.file_date}"
                    ),
                )

    if not config_is_valid(record.period_s, record.mask, record.sample_count):
        issue(
            "config_range",
            (
                f"invalid config period={record.period_s} mask={record.mask} "
                f"sample_count={record.sample_count}"
            ),
        )

    expected_config = None
    if config_versions is not None:
        expected_config = config_versions.get(record.config_version)
        if expected_config is None:
            issues.append(
                {
                    "code": "config_version_evidence",
                    "message": (
                        f"config_version={record.config_version} has no successful "
                        "apply evidence"
                    ),
                    "inconclusive": True,
                }
            )
        else:
            expected_period = expected_config["period_s"]
            expected_mask = expected_config["mask"]
            expected_count = expected_config["sample_count"]
            if (
                record.period_s,
                record.mask,
                record.sample_count,
            ) != (expected_period, expected_mask, expected_count):
                issue(
                    "config_mismatch",
                    (
                        f"config_version={record.config_version} maps to "
                        f"period={expected_period} mask={expected_mask} "
                        f"sample_count={expected_count}, file has "
                        f"period={record.period_s} mask={record.mask} "
                        f"sample_count={record.sample_count}"
                    ),
                )

    values = (record.v0, record.v1, record.v2, record.v3)
    units = (record.u0, record.u1, record.u2, record.u3)
    qualities = (record.q0, record.q1, record.q2, record.q3)
    for index in range(4):
        enabled = bool(record.mask & (1 << index))
        if units[index] != 1:
            issue(
                "unit",
                f"u{index}={units[index]}, expected count unit 1",
            )
        if enabled:
            expected_value = expected_test_value(record.seq, index)
            if values[index] != expected_value:
                issue(
                    "channel_value",
                    (
                        f"v{index}={values[index]}, expected "
                        f"{expected_value} for seq={record.seq}"
                    ),
                )
            if qualities[index] != 1:
                issue(
                    "channel_quality",
                    f"q{index}={qualities[index]}, expected TEST_VALID=1",
                )
        else:
            if values[index] != 0:
                issue(
                    "disabled_channel_value",
                    f"disabled v{index}={values[index]}, expected 0",
                )
            if qualities[index] != 0:
                issue(
                    "disabled_channel_quality",
                    f"disabled q{index}={qualities[index]}, expected 0",
                )
    return issues


def validate_sampling_timeline(
    records: Sequence[CsvRecord],
) -> list[dict[str, Any]]:
    """Independent per-record sampling-time semantics for one file."""

    issues: list[dict[str, Any]] = []

    def issue(code: str, message: str) -> None:
        issues.append({"code": code, "message": message})

    previous: CsvRecord | None = None
    for index, record in enumerate(records):
        if record.trigger == 2 and record.planned_ms != record.actual_ms:
            issue(
                "single_time",
                (
                    f"record {index}: single trigger requires planned_ms == "
                    f"actual_ms, got {record.planned_ms} != {record.actual_ms}"
                ),
            )
        if previous is not None:
            if record.actual_ms < previous.actual_ms:
                issue(
                    "actual_order",
                    (
                        f"record {index}: actual_ms={record.actual_ms} is earlier "
                        f"than the previous actual_ms={previous.actual_ms}"
                    ),
                )
            if record.trigger == 1 and previous.trigger == 1:
                step = record.planned_ms - previous.actual_ms
                expected = record.period_s * 1000
                previous_expected = previous.period_s * 1000
                if step not in (expected, previous_expected):
                    issue(
                        "planned_step",
                        (
                            f"record {index}: planned_ms={record.planned_ms} is "
                            f"{step} ms after the previous actual_ms="
                            f"{previous.actual_ms}, expected {expected} ms"
                        ),
                    )
        previous = record
    return issues


def _word32(high: int, low: int) -> int:
    return ((high & 0xFFFF) << 16) | (low & 0xFFFF)


def decode_storage_block(values: Sequence[int]) -> dict[str, int]:
    if len(values) != STORAGE_COUNT:
        raise ValueError(f"storage block requires {STORAGE_COUNT} registers")
    words = tuple(int(value) & 0xFFFF for value in values)
    decoded = {
        "contract_revision": words[0],
        "save_state": words[1],
        "save_command_id": _word32(words[2], words[3]),
        "save_config_version": _word32(words[4], words[5]),
        "save_error": words[6],
        "config_load_state": words[7],
        "storage_state": words[8],
        "storage_error": words[9],
        "queued": words[10],
        "queue_high_water": words[11],
        "generated": _word32(words[12], words[13]),
        "synced": _word32(words[14], words[15]),
        "dropped": _word32(words[16], words[17]),
        "uncertain": _word32(words[18], words[19]),
        "in_flight": words[20],
        "drain_state": words[21],
        "last_synced_seq": _word32(words[22], words[23]),
        "last_synced_file": _word32(words[24], words[25]),
        "last_synced_date": _word32(words[26], words[27]),
        "active_config_version": _word32(words[28], words[29]),
        "drain_generation": _word32(words[30], words[31]),
        "storage_errors": _word32(words[32], words[33]),
        "load_sequence": _word32(words[34], words[35]),
        "captured_period": _word32(words[36], words[37]),
        "captured_mask": words[38],
        "captured_count": words[39],
        "reserved": tuple(words[40:48]),
    }
    return decoded


def decode_time_status_block(values: Sequence[int]) -> dict[str, Any]:
    if len(values) != TIME_STATUS_COUNT:
        raise ValueError(f"time status requires {TIME_STATUS_COUNT} registers")
    current_utc = _word32(values[4], values[5])
    armed_utc = _word32(values[6], values[7])
    return {
        "time_status": values[0],
        "time_status_name": (
            "VALID"
            if values[0] == 1
            else "UNCALIBRATED"
            if values[0] == 0
            else f"UNKNOWN({values[0]})"
        ),
        "schedule_state": values[3],
        "current_utc_seconds": (
            None if current_utc == 0xFFFFFFFF else current_utc
        ),
        "current_utc_valid": current_utc != 0xFFFFFFFF,
        "armed_start_utc_seconds": None if armed_utc == 0xFFFFFFFF else armed_utc,
        "armed_start_utc_valid": armed_utc != 0xFFFFFFFF,
        "raw_words": list(values),
    }


def u32_delta(new_value: int, old_value: int) -> int:
    return (new_value - old_value) % U32_MODULUS


def storage_conservation_holds(storage: Mapping[str, int]) -> bool:
    return (
        storage["synced"]
        + storage["dropped"]
        + storage["uncertain"]
        + storage["queued"]
        + storage["in_flight"]
    ) % U32_MODULUS == storage["generated"] % U32_MODULUS


def validate_storage_block(storage: Mapping[str, int]) -> list[dict[str, Any]]:
    issues: list[dict[str, Any]] = []

    def issue(code: str, message: str) -> None:
        issues.append({"code": code, "message": message})

    if storage["contract_revision"] != 1:
        issue(
            "contract_revision",
            f"contract_revision={storage['contract_revision']}, expected 1",
        )
    if storage["save_state"] not in (0, 1, 2, 3):
        issue("save_state", f"invalid save_state={storage['save_state']}")
    if storage["config_load_state"] not in (0, 1, 2, 3):
        issue(
            "config_load_state",
            f"invalid config_load_state={storage['config_load_state']}",
        )
    if storage["storage_state"] not in (0, 1, 2, 3, 4, 5, 6):
        issue("storage_state", f"invalid storage_state={storage['storage_state']}")
    if storage["storage_error"] not in range(0, 10):
        issue("storage_error", f"invalid storage_error={storage['storage_error']}")
    if storage["save_error"] not in range(0, 10):
        issue("save_error", f"invalid save_error={storage['save_error']}")
    if storage["save_state"] in (1, 2, 3):
        if not PERIOD_MIN_SEC <= storage["captured_period"] <= PERIOD_MAX_SEC:
            issue(
                "captured_period_range",
                (
                    f"captured_period={storage['captured_period']} is outside "
                    f"{PERIOD_MIN_SEC}..{PERIOD_MAX_SEC} under frozen layout"
                ),
            )
        if not 1 <= storage["captured_mask"] <= MASK_CHANNELS:
            issue(
                "captured_mask_range",
                f"captured_mask={storage['captured_mask']} is outside 1..15",
            )
    if not 0 <= storage["queued"] <= 32:
        issue("queue_range", f"queued={storage['queued']} exceeds 32")
    if not 0 <= storage["queue_high_water"] <= 32:
        issue(
            "queue_high_water",
            f"queue_high_water={storage['queue_high_water']} exceeds 32",
        )
    if storage["in_flight"] not in (0, 1):
        issue("in_flight", f"in_flight={storage['in_flight']} is not 0 or 1")
    if storage["drain_state"] not in (0, 1, 2, 3):
        issue("drain_state", f"invalid drain_state={storage['drain_state']}")
    if tuple(storage["reserved"]) != (0,) * 8:
        issue("reserved_storage", "storage reserved words are not all zero")
    if not storage_conservation_holds(storage):
        issue(
            "conservation",
            (
                "generated != synced+dropped+uncertain+queued+in_flight "
                f"({storage['generated']} != "
                f"{(storage['synced'] + storage['dropped'] + storage['uncertain'] + storage['queued'] + storage['in_flight']) % U32_MODULUS})"
            ),
        )
    if storage["synced"] == 0:
        if storage["last_synced_seq"] != 0 or storage["last_synced_file"] != 0:
            issue(
                "last_synced_invalid",
                "last synced identity is non-zero while synced=0",
            )
    else:
        if storage["last_synced_seq"] == 0 or storage["last_synced_file"] == 0:
            issue(
                "last_synced_invalid",
                "synced>0 requires a non-zero last synced sequence and file id",
            )
    return issues


def records_are_contiguous(
    records: Iterable[CsvRecord],
) -> list[dict[str, Any]]:
    issues: list[dict[str, Any]] = []
    previous: CsvRecord | None = None
    for current in records:
        if previous is None:
            previous = current
            continue
        expected = (previous.seq + 1) % U32_MODULUS
        if current.seq != expected:
            code = "duplicate_seq" if current.seq == previous.seq else "sequence_order"
            issues.append(
                {
                    "code": code,
                    "message": (
                        f"sequence {previous.seq} -> {current.seq}, expected {expected}"
                    ),
                }
            )
        previous = current
    return issues


def duplicate_identity_issues(records: Sequence[CsvRecord]) -> list[dict[str, Any]]:
    first: dict[tuple[int, int], CsvRecord] = {}
    issues: list[dict[str, Any]] = []
    for record in records:
        key = (record.session, record.seq)
        existing = first.get(key)
        if existing is None:
            first[key] = record
            continue
        if existing.payload_key == record.payload_key:
            issues.append(
                {
                    "code": "duplicate_record",
                    "message": f"session={record.session} seq={record.seq} is duplicated",
                }
            )
        else:
            issues.append(
                {
                    "code": "same_seq_payload_mismatch",
                    "message": (
                        f"session={record.session} seq={record.seq} has different payloads"
                    ),
                }
            )
    return issues
