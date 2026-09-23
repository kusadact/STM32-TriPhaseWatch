"""Independent CSV schema, payload, and storage-count oracle."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import PurePosixPath
import re
from typing import Any, Iterable, Mapping, Sequence


CSV_SCHEMA1_COLUMNS = (
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
CSV_DHT11_COLUMNS = (
    "dht_valid_mask",
    "dht_sample_id",
    "dht0_temp_x10",
    "dht1_temp_x10",
    "dht2_temp_x10",
    "dht0_humidity_x10",
    "dht1_humidity_x10",
    "dht2_humidity_x10",
    "dht0_quality",
    "dht1_quality",
    "dht2_quality",
    "dht0_error",
    "dht1_error",
    "dht2_error",
    "dht0_sample_ms",
    "dht1_sample_ms",
    "dht2_sample_ms",
)
CSV_DS18B20_COLUMNS = (
    "ds18b20_valid_mask",
    "ds18b20_sample_id",
    "ds18b20_0_temp_x16",
    "ds18b20_1_temp_x16",
    "ds18b20_2_temp_x16",
    "ds18b20_0_quality",
    "ds18b20_1_quality",
    "ds18b20_2_quality",
    "ds18b20_0_error",
    "ds18b20_1_error",
    "ds18b20_2_error",
    "ds18b20_0_rom_short",
    "ds18b20_1_rom_short",
    "ds18b20_2_rom_short",
    "ds18b20_0_sample_ms",
    "ds18b20_1_sample_ms",
    "ds18b20_2_sample_ms",
)
CSV_EVENT_COLUMNS = (
    "event_id",
    "event_phase",
    "event_level",
    "event_reason",
    "event_trigger_phase",
    "event_max_delta_x16",
    "event_delta_valid",
    "event_flags",
)
CSV_SCHEMA2_COLUMNS = CSV_SCHEMA1_COLUMNS + CSV_DHT11_COLUMNS
CSV_SCHEMA3_COLUMNS = CSV_SCHEMA1_COLUMNS + CSV_DS18B20_COLUMNS
CSV_SCHEMA4_COLUMNS = CSV_SCHEMA3_COLUMNS + CSV_EVENT_COLUMNS
CSV_COLUMNS = CSV_SCHEMA2_COLUMNS
CSV_SCHEMA1_HEADER = ",".join(CSV_SCHEMA1_COLUMNS)
CSV_SCHEMA2_HEADER = ",".join(CSV_SCHEMA2_COLUMNS)
CSV_SCHEMA3_HEADER = ",".join(CSV_SCHEMA3_COLUMNS)
CSV_SCHEMA4_HEADER = ",".join(CSV_SCHEMA4_COLUMNS)
CSV_HEADER = CSV_SCHEMA2_HEADER

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
    "dht_valid_mask",
    "dht0_temp_x10",
    "dht1_temp_x10",
    "dht2_temp_x10",
    "dht0_humidity_x10",
    "dht1_humidity_x10",
    "dht2_humidity_x10",
    "dht0_quality",
    "dht1_quality",
    "dht2_quality",
    "dht0_error",
    "dht1_error",
    "dht2_error",
    "ds18b20_valid_mask",
    "ds18b20_0_quality",
    "ds18b20_1_quality",
    "ds18b20_2_quality",
    "ds18b20_0_error",
    "ds18b20_1_error",
    "ds18b20_2_error",
    "ds18b20_0_rom_short",
    "ds18b20_1_rom_short",
    "ds18b20_2_rom_short",
    "event_phase",
    "event_level",
    "event_reason",
    "event_trigger_phase",
    "event_delta_valid",
    "event_flags",
}
U32_FIELDS = {
    "session",
    "seq",
    "utc_s",
    "config_version",
    "file_id",
    "file_date",
    "dht_sample_id",
    "dht0_sample_ms",
    "dht1_sample_ms",
    "dht2_sample_ms",
    "ds18b20_sample_id",
    "ds18b20_0_sample_ms",
    "ds18b20_1_sample_ms",
    "ds18b20_2_sample_ms",
    "event_id",
}
U64_FIELDS = {"planned_ms", "actual_ms"}
I16_FIELDS = {
    "ds18b20_0_temp_x16",
    "ds18b20_1_temp_x16",
    "ds18b20_2_temp_x16",
    "event_max_delta_x16",
}

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
    dht_valid_mask: int = 0
    dht_sample_id: int = 0
    dht0_temp_x10: int = 0
    dht1_temp_x10: int = 0
    dht2_temp_x10: int = 0
    dht0_humidity_x10: int = 0
    dht1_humidity_x10: int = 0
    dht2_humidity_x10: int = 0
    dht0_quality: int = 0
    dht1_quality: int = 0
    dht2_quality: int = 0
    dht0_error: int = 0
    dht1_error: int = 0
    dht2_error: int = 0
    dht0_sample_ms: int = 0
    dht1_sample_ms: int = 0
    dht2_sample_ms: int = 0
    ds18b20_valid_mask: int = 0
    ds18b20_sample_id: int = 0
    ds18b20_0_temp_x16: int = 0
    ds18b20_1_temp_x16: int = 0
    ds18b20_2_temp_x16: int = 0
    ds18b20_0_quality: int = 0
    ds18b20_1_quality: int = 0
    ds18b20_2_quality: int = 0
    ds18b20_0_error: int = 0
    ds18b20_1_error: int = 0
    ds18b20_2_error: int = 0
    ds18b20_0_rom_short: int = 0
    ds18b20_1_rom_short: int = 0
    ds18b20_2_rom_short: int = 0
    ds18b20_0_sample_ms: int = 0
    ds18b20_1_sample_ms: int = 0
    ds18b20_2_sample_ms: int = 0
    event_id: int = 0
    event_phase: int = 0
    event_level: int = 0
    event_reason: int = 0
    event_trigger_phase: int = 0
    event_max_delta_x16: int = 0
    event_delta_valid: int = 0
    event_flags: int = 0

    @property
    def values(self) -> tuple[int, ...]:
        columns = {
            1: CSV_SCHEMA1_COLUMNS,
            2: CSV_SCHEMA2_COLUMNS,
            3: CSV_SCHEMA3_COLUMNS,
            4: CSV_SCHEMA4_COLUMNS,
        }.get(self.schema, CSV_SCHEMA3_COLUMNS)
        return tuple(getattr(self, name) for name in columns)

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
            self.dht_valid_mask,
            self.dht_sample_id,
            self.dht0_temp_x10,
            self.dht1_temp_x10,
            self.dht2_temp_x10,
            self.dht0_humidity_x10,
            self.dht1_humidity_x10,
            self.dht2_humidity_x10,
            self.dht0_quality,
            self.dht1_quality,
            self.dht2_quality,
            self.dht0_error,
            self.dht1_error,
            self.dht2_error,
            self.dht0_sample_ms,
            self.dht1_sample_ms,
            self.dht2_sample_ms,
            self.ds18b20_valid_mask,
            self.ds18b20_sample_id,
            self.ds18b20_0_temp_x16,
            self.ds18b20_1_temp_x16,
            self.ds18b20_2_temp_x16,
            self.ds18b20_0_quality,
            self.ds18b20_1_quality,
            self.ds18b20_2_quality,
            self.ds18b20_0_error,
            self.ds18b20_1_error,
            self.ds18b20_2_error,
            self.ds18b20_0_rom_short,
            self.ds18b20_1_rom_short,
            self.ds18b20_2_rom_short,
            self.ds18b20_0_sample_ms,
            self.ds18b20_1_sample_ms,
            self.ds18b20_2_sample_ms,
            self.event_id,
            self.event_phase,
            self.event_level,
            self.event_reason,
            self.event_trigger_phase,
            self.event_max_delta_x16,
            self.event_delta_valid,
            self.event_flags,
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


def is_ascii_signed_decimal(value: bytes) -> bool:
    return bool(value) and (
        (value[0] != ord("-") and is_ascii_decimal(value))
        or (
            value[0] == ord("-")
            and len(value) > 1
            and is_ascii_decimal(value[1:])
        )
    )


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


def _parse_signed(
    raw: bytes,
    field: str,
    *,
    line_number: int,
    offset: int,
) -> int:
    if not is_ascii_signed_decimal(raw):
        raise CsvContractError(
            "non_decimal_field",
            f"line {line_number} field {field!r} is not signed decimal",
            offset=offset,
        )
    value = int(raw)
    if field in I16_FIELDS and not -0x8000 <= value <= 0x7FFF:
        raise CsvContractError(
            "field_width",
            f"line {line_number} field {field!r} exceeds i16",
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
    schema1_header = CSV_SCHEMA1_HEADER.encode("ascii")
    schema2_header = CSV_SCHEMA2_HEADER.encode("ascii")
    schema3_header = CSV_SCHEMA3_HEADER.encode("ascii")
    schema4_header = CSV_SCHEMA4_HEADER.encode("ascii")
    if lines[0] == schema1_header:
        columns = CSV_SCHEMA1_COLUMNS
    elif lines[0] == schema2_header:
        columns = CSV_SCHEMA2_COLUMNS
    elif lines[0] == schema3_header:
        columns = CSV_SCHEMA3_COLUMNS
    elif lines[0] == schema4_header:
        columns = CSV_SCHEMA4_COLUMNS
    else:
        raise CsvContractError(
            "header_mismatch",
            "CSV header does not match schema 1, 2, 3, or 4 exactly",
            offset=0,
        )

    records: list[CsvRecord] = []
    byte_offset = len(lines[0]) + 1
    for line_number, line in enumerate(lines[1:], start=2):
        if line in (schema1_header, schema2_header, schema3_header,
                    schema4_header):
            raise CsvContractError(
                "duplicate_header",
                f"duplicate header at line {line_number}",
                offset=byte_offset,
            )
        fields = line.split(b",")
        if len(fields) != len(columns):
            raise CsvContractError(
                "column_count",
                (
                    f"line {line_number} has {len(fields)} columns, "
                    f"expected {len(columns)}"
                ),
                offset=byte_offset,
            )
        values: dict[str, int] = {}
        local_offset = byte_offset
        for field, raw in zip(columns, fields):
            if field in I16_FIELDS:
                values[field] = _parse_signed(
                    raw,
                    field,
                    line_number=line_number,
                    offset=local_offset,
                )
            else:
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


def is_event_record(record: CsvRecord) -> bool:
    return record.schema == 4 and record.event_phase != 0


def validate_record_contract(
    record: CsvRecord,
    *,
    config_versions: Mapping[int, Mapping[str, int]] | None,
) -> list[dict[str, Any]]:
    issues: list[dict[str, Any]] = []

    def issue(code: str, message: str) -> None:
        issues.append({"code": code, "message": message})

    event_record = record.schema == 4 and record.event_phase != 0
    if record.schema not in (1, 2, 3, 4):
        issue("schema", f"schema={record.schema}, expected 1, 2, 3, or 4")
    if record.reserved != 0:
        issue("reserved", f"reserved={record.reserved}, expected 0")
    if event_record:
        if record.trigger != 0:
            issue("event_trigger", "schema-4 event rows require trigger=NONE")
    elif record.trigger not in (1, 2):
        issue("trigger", f"trigger={record.trigger}, expected 1 or 2")
    if record.source not in (1, 2, 3):
        issue(
            "source",
            f"source={record.source}, expected TEST=1, REAL_DHT11=2, or REAL_DS18B20=3",
        )
    if record.schema == 1 and record.source != 1:
        issue("schema_source", "CSV schema 1 requires TEST source")
    if record.schema == 2 and record.source not in (1, 2):
        issue("schema_source", "CSV schema 2 requires TEST or REAL_DHT11 source")
    if record.schema == 3 and record.source not in (1, 3):
        issue("schema_source", "CSV schema 3 requires TEST or REAL_DS18B20 source")
    if record.schema == 4 and record.source not in (1, 3):
        issue(
            "schema_source",
            "CSV schema 4 requires TEST or REAL_DS18B20 source",
        )
    if (
        record.schema == 4
        and not event_record
        and any(
            (
                record.event_id,
                record.event_phase,
                record.event_level,
                record.event_reason,
                record.event_trigger_phase,
                record.event_max_delta_x16,
                record.event_delta_valid,
                record.event_flags,
            )
        )
    ):
        issue("normal_event_fields", "schema-4 normal rows require zero event fields")
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

    if event_record:
        if record.source != 3:
            issue("event_source", "schema-4 event rows require REAL_DS18B20 source")
        if record.event_id == 0:
            issue("event_id", "schema-4 event rows require a non-zero event_id")
        if record.ds18b20_sample_id != record.seq:
            issue(
                "event_sample_id",
                (
                    f"event row seq={record.seq} does not match "
                    f"ds18b20_sample_id={record.ds18b20_sample_id}"
                ),
            )
        if any(
            (
                record.ds18b20_0_error,
                record.ds18b20_1_error,
                record.ds18b20_2_error,
                record.ds18b20_0_rom_short,
                record.ds18b20_1_rom_short,
                record.ds18b20_2_rom_short,
                record.ds18b20_0_sample_ms,
                record.ds18b20_1_sample_ms,
                record.ds18b20_2_sample_ms,
            )
        ):
            issue(
                "event_aux_fields",
                "event rows require error/rom_short/sample_ms fields at zero",
            )
        if record.planned_ms != record.actual_ms:
            issue(
                "event_time",
                "schema-4 event rows require planned_ms == actual_ms",
            )
        if record.event_phase not in (1, 2, 3, 4, 5):
            issue(
                "event_phase",
                f"event_phase={record.event_phase}, expected 1..5",
            )
        if record.event_level not in (0, 1, 2, 3, 4, 5):
            issue(
                "event_level",
                f"event_level={record.event_level}, expected 0..5",
            )
        if record.event_reason not in range(0, 10):
            issue(
                "event_reason",
                f"event_reason={record.event_reason}, expected 0..9",
            )
        if record.event_trigger_phase not in (0, 1, 2, 3):
            issue(
                "event_trigger_phase",
                (
                    f"event_trigger_phase={record.event_trigger_phase}, "
                    "expected 0..3"
                ),
            )
        if record.event_delta_valid not in (0, 1):
            issue(
                "event_delta_valid",
                f"event_delta_valid={record.event_delta_valid}, expected 0 or 1",
            )
        elif record.event_delta_valid == 0 and record.event_max_delta_x16 != 0:
            issue(
                "event_delta_value",
                "event_delta_valid=0 requires event_max_delta_x16=0",
            )
        if record.event_flags & ~0x03FF:
            issue(
                "event_flags",
                f"event_flags={record.event_flags} has unknown bits",
            )
        if any(
            (
                record.dht_valid_mask,
                record.dht_sample_id,
                record.dht0_temp_x10,
                record.dht1_temp_x10,
                record.dht2_temp_x10,
                record.dht0_humidity_x10,
                record.dht1_humidity_x10,
                record.dht2_humidity_x10,
                record.dht0_quality,
                record.dht1_quality,
                record.dht2_quality,
                record.dht0_error,
                record.dht1_error,
                record.dht2_error,
                record.dht0_sample_ms,
                record.dht1_sample_ms,
                record.dht2_sample_ms,
            )
        ):
            issue("event_dht11_fields", "event rows require DHT11 fields at zero")
        for index in range(4):
            if (
                (record.u0, record.u1, record.u2, record.u3)[index] != 2
                or (record.v0, record.v1, record.v2, record.v3)[index] != 0
                or (record.q0, record.q1, record.q2, record.q3)[index] != 0
            ):
                issue(
                    "event_legacy_channel",
                    (
                        f"event row requires legacy channel {index} to use "
                        "temperature unit, zero value and zero quality"
                    ),
                )
        if record.ds18b20_valid_mask & ~0x0007:
            issue(
                "ds18b20_valid_mask",
                f"invalid DS18B20 valid mask {record.ds18b20_valid_mask}",
            )
        event_temperatures = (
            record.ds18b20_0_temp_x16,
            record.ds18b20_1_temp_x16,
            record.ds18b20_2_temp_x16,
        )
        event_qualities = (
            record.ds18b20_0_quality,
            record.ds18b20_1_quality,
            record.ds18b20_2_quality,
        )
        for index, quality in enumerate(event_qualities):
            expected_valid = quality in (1, 5)
            if quality not in (0, 1, 2, 3, 4, 5, 6):
                issue(
                    "ds18b20_quality",
                    f"DS18B20-{index} has invalid quality={quality}",
                )
                continue
            if bool(record.ds18b20_valid_mask & (1 << index)) != expected_valid:
                issue(
                    "ds18b20_valid_mask",
                    (
                        f"DS18B20-{index} quality={quality} does not match "
                        "valid mask bit"
                    ),
                )
            if not expected_valid and event_temperatures[index] != 0:
                issue(
                    "ds18b20_invalid_value",
                    f"DS18B20-{index} reports a temperature without a valid sample",
                )
        return issues

    values = (record.v0, record.v1, record.v2, record.v3)
    units = (record.u0, record.u1, record.u2, record.u3)
    qualities = (record.q0, record.q1, record.q2, record.q3)
    if record.source == 1:
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
        if any(
            (
                record.dht_valid_mask,
                record.dht_sample_id,
                record.dht0_temp_x10,
                record.dht1_temp_x10,
                record.dht2_temp_x10,
                record.dht0_humidity_x10,
                record.dht1_humidity_x10,
                record.dht2_humidity_x10,
                record.dht0_quality,
                record.dht1_quality,
                record.dht2_quality,
                record.dht0_error,
                record.dht1_error,
                record.dht2_error,
                record.dht0_sample_ms,
                record.dht1_sample_ms,
                record.dht2_sample_ms,
            )
        ):
            issue("test_dht11_fields", "TEST source must leave DHT11 fields at zero")
        if any(
            (
                record.ds18b20_valid_mask,
                record.ds18b20_sample_id,
                record.ds18b20_0_temp_x16,
                record.ds18b20_1_temp_x16,
                record.ds18b20_2_temp_x16,
                record.ds18b20_0_quality,
                record.ds18b20_1_quality,
                record.ds18b20_2_quality,
                record.ds18b20_0_error,
                record.ds18b20_1_error,
                record.ds18b20_2_error,
                record.ds18b20_0_rom_short,
                record.ds18b20_1_rom_short,
                record.ds18b20_2_rom_short,
                record.ds18b20_0_sample_ms,
                record.ds18b20_1_sample_ms,
                record.ds18b20_2_sample_ms,
            )
        ):
            issue(
                "test_ds18b20_fields",
                "TEST source must leave DS18B20 fields at zero",
            )
    elif record.source == 2:
        if record.schema != 2:
            issue("dht11_schema", "REAL_DHT11 requires CSV schema 2")
        if record.dht_sample_id == 0:
            issue("dht11_sample_id", "REAL_DHT11 requires a non-zero sample id")
        for index in range(4):
            if (units[index] != 1) or (values[index] != 0) or (qualities[index] != 0):
                issue(
                    "real_legacy_channel",
                    (
                        f"REAL_DHT11 requires zero legacy channel {index}, "
                        "unit=1 and quality=0"
                    ),
                )
        temperatures = (
            record.dht0_temp_x10,
            record.dht1_temp_x10,
            record.dht2_temp_x10,
        )
        humidities = (
            record.dht0_humidity_x10,
            record.dht1_humidity_x10,
            record.dht2_humidity_x10,
        )
        dht_qualities = (
            record.dht0_quality,
            record.dht1_quality,
            record.dht2_quality,
        )
        dht_errors = (
            record.dht0_error,
            record.dht1_error,
            record.dht2_error,
        )
        for index, quality in enumerate(dht_qualities):
            expected_valid = quality in (1, 5)
            expected_errors = {
                1: {0},
                2: {1, 2},
                3: {3},
                4: {4},
                5: {1, 2, 3, 4, 5},
                6: {0, 1, 2, 3, 4, 255},
            }
            if bool(record.dht_valid_mask & (1 << index)) != expected_valid:
                issue(
                    "dht11_valid_mask",
                    (
                        f"DHT11-{index} quality={quality} does not match "
                        f"valid mask bit"
                    ),
                )
            if quality not in (1, 2, 3, 4, 5, 6):
                issue(
                    "dht11_quality",
                    f"DHT11-{index} has invalid quality={quality}",
                )
            if not expected_valid and (
                temperatures[index] != 0 or humidities[index] != 0
            ):
                issue(
                    "dht11_invalid_value",
                    f"DHT11-{index} reports values without a valid sample",
                )
            if quality == 1 and dht_errors[index] != 0:
                issue(
                    "dht11_ok_error",
                    f"DHT11-{index} is OK with error={dht_errors[index]}",
                )
            if quality == 5 and dht_errors[index] == 0:
                issue(
                    "dht11_stale_error",
                    f"DHT11-{index} is STALE without a failure reason",
                )
            if (
                quality in expected_errors
                and dht_errors[index] not in expected_errors[quality]
            ):
                issue(
                    "dht11_error_mismatch",
                    (
                        f"DHT11-{index} quality={quality} has incompatible "
                        f"error={dht_errors[index]}"
                    ),
                )
    else:
        if record.schema not in (3, 4):
            issue("ds18b20_schema", "REAL_DS18B20 requires CSV schema 3 or 4")
        if record.ds18b20_sample_id == 0:
            issue(
                "ds18b20_sample_id",
                "REAL_DS18B20 requires a non-zero sample id",
            )
        if record.ds18b20_valid_mask & ~0x0007:
            issue(
                "ds18b20_valid_mask",
                f"invalid DS18B20 valid mask {record.ds18b20_valid_mask}",
            )
        for index in range(4):
            if (
                units[index] != 2
                or values[index] != 0
                or qualities[index] != 0
            ):
                issue(
                    "real_legacy_channel",
                    (
                        f"REAL_DS18B20 requires zero legacy channel {index}, "
                        "unit=2 and quality=0"
                    ),
                )
        temperatures = (
            record.ds18b20_0_temp_x16,
            record.ds18b20_1_temp_x16,
            record.ds18b20_2_temp_x16,
        )
        ds_qualities = (
            record.ds18b20_0_quality,
            record.ds18b20_1_quality,
            record.ds18b20_2_quality,
        )
        ds_errors = (
            record.ds18b20_0_error,
            record.ds18b20_1_error,
            record.ds18b20_2_error,
        )
        expected_errors = {
            1: {0},
            2: {1, 2},
            3: {3, 6},
            4: {7},
            5: {1, 2, 3, 4, 5, 6, 7, 255},
            6: {0, 1, 2, 3, 4, 5, 6, 7, 255},
        }
        for index, quality in enumerate(ds_qualities):
            expected_valid = quality in (1, 5)
            if bool(record.ds18b20_valid_mask & (1 << index)) != expected_valid:
                issue(
                    "ds18b20_valid_mask",
                    (
                        f"DS18B20-{index} quality={quality} does not match "
                        "valid mask bit"
                    ),
                )
            if quality not in expected_errors:
                issue(
                    "ds18b20_quality",
                    f"DS18B20-{index} has invalid quality={quality}",
                )
                continue
            if not expected_valid and temperatures[index] != 0:
                issue(
                    "ds18b20_invalid_value",
                    f"DS18B20-{index} reports a temperature without a valid sample",
                )
            if ds_errors[index] not in expected_errors[quality]:
                issue(
                    "ds18b20_error_mismatch",
                    (
                        f"DS18B20-{index} quality={quality} has incompatible "
                        f"error={ds_errors[index]}"
                    ),
                )
    return issues


def validate_sampling_timeline(
    records: Sequence[CsvRecord],
) -> list[dict[str, Any]]:
    """Independent per-record sampling-time semantics for one file."""

    issues: list[dict[str, Any]] = []

    def issue(code: str, message: str) -> None:
        issues.append({"code": code, "message": message})

    previous_normal: CsvRecord | None = None
    event_id: int | None = None
    event_phase: int | None = None
    event_time_ms: int | None = None
    event_seen_trigger = False
    event_seen_active = False
    event_closed = False
    for index, record in enumerate(records):
        event_record = is_event_record(record)
        if event_record and record.planned_ms != record.actual_ms:
            issue(
                "event_time",
                (
                    f"record {index}: event rows require planned_ms == "
                    f"actual_ms, got {record.planned_ms} != {record.actual_ms}"
                ),
            )
        if event_record:
            if event_id != record.event_id:
                event_id = record.event_id
                event_phase = None
                event_time_ms = None
                event_seen_trigger = False
                event_seen_active = False
                event_closed = False

            if record.event_phase == 1:
                if event_seen_trigger or event_seen_active or event_closed:
                    issue(
                        "event_phase_order",
                        f"record {index}: PRE appears after the event started",
                    )
                if (
                    event_phase == 1
                    and event_time_ms is not None
                    and record.actual_ms < event_time_ms
                ):
                    issue(
                        "actual_order",
                        (
                            f"record {index}: PRE actual_ms={record.actual_ms} "
                            f"is earlier than previous PRE {event_time_ms}"
                        ),
                    )
            elif record.event_phase == 2:
                if event_seen_trigger or event_seen_active or event_closed:
                    issue(
                        "event_phase_order",
                        f"record {index}: duplicate or late TRIGGER",
                    )
                event_seen_trigger = True
            elif record.event_phase == 3:
                if not event_seen_trigger or event_closed:
                    issue(
                        "event_phase_order",
                        f"record {index}: ACTIVE without an open trigger",
                    )
                event_seen_active = True
            elif record.event_phase == 4:
                if not event_seen_trigger or event_closed:
                    issue(
                        "event_phase_order",
                        f"record {index}: POST without an open trigger",
                    )
                event_seen_active = True
            elif record.event_phase == 5:
                if not event_seen_trigger or event_closed:
                    issue(
                        "event_phase_order",
                        f"record {index}: duplicate or premature CLOSE",
                    )
                event_closed = True

            if record.event_phase != 1 or event_phase == 1:
                if (
                    event_time_ms is not None
                    and record.actual_ms < event_time_ms
                ):
                    issue(
                        "actual_order",
                        (
                            f"record {index}: event actual_ms={record.actual_ms} "
                            f"is earlier than previous event time={event_time_ms}"
                        ),
                    )
            if (
                previous_normal is not None
                and record.actual_ms < previous_normal.actual_ms
                and record.event_phase != 1
            ):
                issue(
                    "actual_order",
                    (
                        f"record {index}: event actual_ms={record.actual_ms} "
                        f"is earlier than previous normal "
                        f"actual_ms={previous_normal.actual_ms}"
                    ),
                )
            event_phase = record.event_phase
            event_time_ms = record.actual_ms
            continue

        if (
            record.trigger == 2
            and record.planned_ms != record.actual_ms
        ):
            issue(
                "single_time",
                (
                    f"record {index}: single trigger requires planned_ms == "
                    f"actual_ms, got {record.planned_ms} != {record.actual_ms}"
                ),
            )
        if previous_normal is not None:
            if record.actual_ms < previous_normal.actual_ms:
                issue(
                    "actual_order",
                    (
                        f"record {index}: actual_ms={record.actual_ms} is earlier "
                        f"than the previous normal actual_ms="
                        f"{previous_normal.actual_ms}"
                    ),
                )
        if (
            record.trigger == 1
            and previous_normal is not None
            and previous_normal.trigger == 1
        ):
            step = record.planned_ms - previous_normal.actual_ms
            expected = record.period_s * 1000
            previous_expected = previous_normal.period_s * 1000
            if step not in (expected, previous_expected):
                issue(
                    "planned_step",
                    (
                        f"record {index}: planned_ms={record.planned_ms} is "
                        f"{step} ms after the previous actual_ms="
                        f"{previous_normal.actual_ms}, expected {expected} ms"
                    ),
                )
        previous_normal = record
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

    if storage["contract_revision"] not in (1, 2):
        issue(
            "contract_revision",
            f"contract_revision={storage['contract_revision']}, expected 1 or 2",
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
        if is_event_record(current):
            continue
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
        if is_event_record(record):
            continue
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
