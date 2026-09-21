"""Board A register decoding kept separate from protocol transport."""

from __future__ import annotations

from typing import Any, Sequence

COMMAND_NAMES = {
    0: "NONE",
    1: "APPLY_CONFIG",
    2: "SAVE_CONFIG",
    3: "START",
    4: "STOP",
    5: "SINGLE",
    6: "SET_TIME",
    7: "ARM_START",
}

COMMAND_RESULT_NAMES = {
    0: "NONE",
    1: "ACCEPTED",
    2: "DUPLICATE",
    3: "UNSUPPORTED",
    4: "REJECTED",
}

RUN_STATE_NAMES = {
    0: "STOPPED",
    1: "RUNNING",
}

TRIGGER_NAMES = {
    0: "NONE",
    1: "PERIODIC",
    2: "SINGLE",
}

QUALITY_NAMES = {
    0: "UNAVAILABLE",
    1: "TEST_VALID",
}

DATA_SOURCE_NAMES = {
    0: "NONE",
    1: "TEST",
    2: "REAL_DHT11",
}

DHT11_QUALITY_NAMES = {
    1: "OK",
    2: "TIMEOUT",
    3: "CHECKSUM_ERROR",
    4: "RANGE_ERROR",
    5: "STALE",
    6: "NOT_PRESENT",
}

DHT11_ERROR_NAMES = {
    0: "NONE",
    1: "TIMEOUT_RESPONSE",
    2: "TIMEOUT_BIT",
    3: "CHECKSUM",
    4: "RANGE",
    5: "TOO_SOON",
    255: "DRIVER",
}

SAVE_STATE_NAMES = {
    0: "IDLE",
    1: "PENDING",
    2: "SUCCESS",
    3: "FAILED",
}

SAVE_ERROR_NAMES = {
    0: "NONE",
    1: "NO_VALID_RECORD",
    2: "INVALID_DATA",
    3: "IO",
    4: "CONFLICT",
    5: "VERIFY",
    6: "NOT_READY",
    7: "BUSY",
    8: "TIMEOUT",
    9: "OTHER",
}

CONFIG_LOAD_STATE_NAMES = {
    0: "UNFINISHED",
    1: "SUCCESS",
    2: "DEFAULT_NO_RECORD",
    3: "DEFAULT_ERROR",
}

STORAGE_STATE_NAMES = {
    0: "INITIALIZING",
    1: "READY",
    2: "UNAVAILABLE",
    3: "FULL",
    4: "IO_ERROR",
    5: "DRAINING",
    6: "CLOSED",
}

STORAGE_ERROR_NAMES = {
    0: "NONE",
    1: "INIT",
    2: "MOUNT",
    3: "CREATE",
    4: "FULL",
    5: "WRITE",
    6: "SYNC",
    7: "CLOSE",
    8: "TIMEOUT",
    9: "NAME_EXHAUSTED",
}

DRAIN_STATE_NAMES = {
    0: "NONE",
    1: "PENDING",
    2: "DONE",
    3: "FAILED",
}

STATS_FIELDS = (
    "rx_frames",
    "crc_errors",
    "address_mismatch",
    "overlong_frames",
    "malformed_frames",
    "tx_responses",
    "broadcast_writes",
    "broadcast_reads",
    "exception_responses",
    "scheduler_missed",
    "storage_dropped",
    "persistence_errors",
    "device_faults",
)


def _word32(high: int, low: int) -> int:
    return (high << 16) | low


def _require_length(values: Sequence[int], expected: int, name: str) -> None:
    if len(values) != expected:
        raise ValueError(f"{name} requires {expected} registers")


def decode_identity(values: Sequence[int]) -> dict[str, Any]:
    _require_length(values, 5, "identity")
    return {
        "device_type": values[0],
        "reported_version": f"{values[1]}.{values[2]}.{values[3]}",
        "firmware_words": [values[1], values[2], values[3]],
        "protocol_version": values[4],
    }


def decode_status(values: Sequence[int]) -> dict[str, Any]:
    _require_length(values, 25, "status")
    run_state = values[5]
    last_command = values[12]
    command_result = values[13]
    return {
        "identity": decode_identity(values[:5]),
        "run_state": RUN_STATE_NAMES.get(run_state, f"UNKNOWN({run_state})"),
        "run_state_code": run_state,
        "active_config": {
            "valid": bool(values[6]),
            "period_sec": values[7],
            "channel_mask": values[8],
            "record_count": values[9],
            "version": _word32(values[10], values[11]),
        },
        "last_command": {
            "code": last_command,
            "name": COMMAND_NAMES.get(last_command, f"UNKNOWN({last_command})"),
            "result_code": command_result,
            "result": COMMAND_RESULT_NAMES.get(
                command_result,
                f"UNKNOWN({command_result})",
            ),
            "id": _word32(values[14], values[15]),
        },
        "data_source_type": values[16],
        "session_id": _word32(values[17], values[18]),
        "persistence_status": values[19],
        "storage_status": values[20],
        "rtos_status": values[21],
        "time_status": values[22],
        "records_this_run": _word32(values[23], values[24]),
    }


def decode_snapshot(values: Sequence[int]) -> dict[str, Any]:
    _require_length(values, 15, "snapshot")
    trigger = values[13]
    channels = []
    for index in range(4):
        quality = values[9 + index]
        channels.append(
            {
                "index": index,
                "value": values[5 + index],
                "quality_code": quality,
                "quality": QUALITY_NAMES.get(
                    quality,
                    f"UNKNOWN({quality})",
                ),
            }
        )
    return {
        "valid": bool(values[0]),
        "sequence": _word32(values[1], values[2]),
        "source_type": values[3],
        "source": DATA_SOURCE_NAMES.get(
            values[3],
            f"UNKNOWN({values[3]})",
        ),
        "channel_count": values[4],
        "channels": channels,
        "trigger_code": trigger,
        "trigger": TRIGGER_NAMES.get(trigger, f"UNKNOWN({trigger})"),
        "unit_code": values[14],
        "unit": "count" if values[14] == 1 else f"UNKNOWN({values[14]})",
    }


def decode_dht11_snapshot(values: Sequence[int]) -> dict[str, Any]:
    """Decode the 24-register DHT11 extension block at input 0x00B0."""

    _require_length(values, 24, "DHT11 snapshot")
    source = values[1]
    valid_mask = values[2]
    sensors = []
    for index in range(3):
        quality = values[11 + index]
        error = values[14 + index]
        sample_time_offset = 17 + (index * 2)
        sensors.append(
            {
                "sensor_id": index,
                "sensor_type": "DHT11",
                "valid": bool(valid_mask & (1 << index)),
                "temperature_x10": values[5 + index],
                "temperature_unit": "0.1degC",
                "humidity_x10": values[8 + index],
                "humidity_unit": "0.1%RH",
                "quality_code": quality,
                "quality": DHT11_QUALITY_NAMES.get(
                    quality,
                    f"UNKNOWN({quality})",
                ),
                "error_code": error,
                "error": DHT11_ERROR_NAMES.get(
                    error,
                    f"UNKNOWN({error})",
                ),
                "sample_time_ms": _word32(
                    values[sample_time_offset],
                    values[sample_time_offset + 1],
                ),
            }
        )
    return {
        "contract_revision": values[0],
        "source_type": source,
        "source": DATA_SOURCE_NAMES.get(source, f"UNKNOWN({source})"),
        "valid_mask": valid_mask,
        "sample_id": _word32(values[3], values[4]),
        "sensors": sensors,
        "sensor_type_code": values[23],
    }


def decode_stats(values: Sequence[int]) -> dict[str, Any]:
    _require_length(values, 26, "stats")
    return {
        name: _word32(values[index * 2], values[(index * 2) + 1])
        for index, name in enumerate(STATS_FIELDS)
    }


def decode_command_observation(values: Sequence[int]) -> dict[str, Any]:
    _require_length(values, 11, "command observation")
    run_state = values[0]
    command = values[7]
    result = values[8]
    return {
        "run_state": RUN_STATE_NAMES.get(run_state, f"UNKNOWN({run_state})"),
        "run_state_code": run_state,
        "active_config": {
            "valid": bool(values[1]),
            "period_sec": values[2],
            "channel_mask": values[3],
            "record_count": values[4],
            "version": _word32(values[5], values[6]),
        },
        "last_command": {
            "code": command,
            "name": COMMAND_NAMES.get(command, f"UNKNOWN({command})"),
            "result_code": result,
            "result": COMMAND_RESULT_NAMES.get(
                result,
                f"UNKNOWN({result})",
            ),
            "id": _word32(values[9], values[10]),
        },
    }


def decode_time_status(values: Sequence[int]) -> dict[str, Any]:
    _require_length(values, 8, "time status")
    time_status = values[0]
    schedule_state = values[3]
    current_words = (values[4], values[5])
    armed_words = (values[6], values[7])
    current_raw = _word32(*current_words)
    armed_raw = _word32(*armed_words)
    return {
        "time_status": time_status,
        "time_status_name": (
            "VALID"
            if time_status == 1
            else "UNCALIBRATED"
            if time_status == 0
            else f"UNKNOWN({time_status})"
        ),
        "current_utc_seconds": (None if current_raw == 0xFFFFFFFF else current_raw),
        "current_utc_words": [current_words[0], current_words[1]],
        "current_utc_valid": current_raw != 0xFFFFFFFF,
        "schedule_state": schedule_state,
        "schedule_state_name": (
            "ARMED"
            if schedule_state == 1
            else "NONE"
            if schedule_state == 0
            else f"UNKNOWN({schedule_state})"
        ),
        "armed_start_utc_seconds": (None if armed_raw == 0xFFFFFFFF else armed_raw),
        "armed_start_utc_words": [armed_words[0], armed_words[1]],
        "armed_start_utc_valid": armed_raw != 0xFFFFFFFF,
    }


def decode_persistence_status(values: Sequence[int]) -> dict[str, Any]:
    _require_length(values, 48, "persistence status")
    save_state = values[1]
    save_error = values[6]
    config_load_state = values[7]
    storage_state = values[8]
    storage_error = values[9]
    drain_state = values[0x15]
    return {
        "contract_revision": values[0],
        "save_state": save_state,
        "save_state_name": SAVE_STATE_NAMES.get(
            save_state,
            f"UNKNOWN({save_state})",
        ),
        "save_command_id": _word32(values[2], values[3]),
        "save_config_version": _word32(values[4], values[5]),
        "save_error": save_error,
        "save_error_name": SAVE_ERROR_NAMES.get(
            save_error,
            f"UNKNOWN({save_error})",
        ),
        "config_load_state": config_load_state,
        "config_load_state_name": CONFIG_LOAD_STATE_NAMES.get(
            config_load_state,
            f"UNKNOWN({config_load_state})",
        ),
        "storage_state": storage_state,
        "storage_state_name": STORAGE_STATE_NAMES.get(
            storage_state,
            f"UNKNOWN({storage_state})",
        ),
        "storage_error": storage_error,
        "storage_error_name": STORAGE_ERROR_NAMES.get(
            storage_error,
            f"UNKNOWN({storage_error})",
        ),
        "queued": values[10],
        "queue_high_water": values[11],
        "generated": _word32(values[12], values[13]),
        "synced": _word32(values[14], values[15]),
        "dropped": _word32(values[16], values[17]),
        "uncertain": _word32(values[18], values[19]),
        "in_flight": values[20],
        "drain_state": drain_state,
        "drain_state_name": DRAIN_STATE_NAMES.get(
            drain_state,
            f"UNKNOWN({drain_state})",
        ),
        "last_synced_seq": _word32(values[0x16], values[0x17]),
        "last_synced_file": _word32(values[0x18], values[0x19]),
        "last_synced_date": _word32(values[0x1A], values[0x1B]),
        "active_config_version": _word32(values[0x1C], values[0x1D]),
        "drain_generation": _word32(values[0x1E], values[0x1F]),
        "storage_errors": _word32(values[0x20], values[0x21]),
        "load_sequence": _word32(values[0x22], values[0x23]),
        "captured_period_s": _word32(values[0x24], values[0x25]),
        "captured_mask": values[0x26],
        "captured_count": values[0x27],
    }


def command_result_name(value: int) -> str:
    return COMMAND_RESULT_NAMES.get(value, f"UNKNOWN({value})")
