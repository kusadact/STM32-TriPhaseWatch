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
        "source": "TEST" if values[3] == 1 else f"UNKNOWN({values[3]})",
        "channel_count": values[4],
        "channels": channels,
        "trigger_code": trigger,
        "trigger": TRIGGER_NAMES.get(trigger, f"UNKNOWN({trigger})"),
        "unit_code": values[14],
        "unit": "count" if values[14] == 1 else f"UNKNOWN({values[14]})",
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


def command_result_name(value: int) -> str:
    return COMMAND_RESULT_NAMES.get(value, f"UNKNOWN({value})")
