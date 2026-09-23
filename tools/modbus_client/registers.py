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
    8: "ACK_ALARM",
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
    3: "REAL_DS18B20",
}

DS18B20_QUALITY_NAMES = {
    1: "OK",
    2: "TIMEOUT",
    3: "CRC_ERROR",
    4: "RANGE_ERROR",
    5: "STALE",
    6: "NOT_PRESENT",
}

DS18B20_ERROR_NAMES = {
    0: "NONE",
    1: "RESET_TIMEOUT",
    2: "BUS_STUCK_LOW",
    3: "ROM_CRC",
    4: "ROM_FAMILY",
    5: "SEARCH",
    6: "SCRATCHPAD_CRC",
    7: "RANGE",
    255: "DRIVER",
}

THERMAL_ALARM_CONTRACT_REVISION = 1
THERMAL_ALARM_INPUT_START = 0x00C8
THERMAL_ALARM_INPUT_COUNT = 29
THERMAL_STATE_INPUT_START = 0x00B0
THERMAL_STATE_INPUT_COUNT = 53
THERMAL_ALARM_CONFIG_START = 0x0050
THERMAL_ALARM_CONFIG_COUNT = 16
THERMAL_ALARM_CONFIG_REVISION = 1

ALARM_LEVEL_NAMES = {
    0: "NORMAL",
    1: "NOTICE",
    2: "WARNING",
    3: "CRITICAL",
    4: "SENSOR_FAULT",
    5: "UNKNOWN",
}

ALARM_REASON_NAMES = {
    0: "NONE",
    1: "PHASE_TEMPERATURE_HIGH",
    2: "PHASE_DELTA_HIGH",
    3: "RISE_RATE_HIGH",
    4: "SENSOR_NOT_PRESENT",
    5: "SENSOR_TIMEOUT",
    6: "SENSOR_CRC_ERROR",
    7: "SENSOR_RANGE_ERROR",
    8: "INSUFFICIENT_VALID_PHASES",
    9: "CONFIG_INVALID",
}

ALARM_PHASE_NAMES = {
    0: "NONE",
    1: "A",
    2: "B",
    3: "C",
}

ALARM_FLAG_VALID = 1 << 0
ALARM_FLAG_LATCHED = 1 << 1
ALARM_FLAG_ACKNOWLEDGED = 1 << 2
ALARM_FLAG_BUZZER_ACTIVE = 1 << 3
ALARM_FLAG_MASK = (
    ALARM_FLAG_VALID
    | ALARM_FLAG_LATCHED
    | ALARM_FLAG_ACKNOWLEDGED
    | ALARM_FLAG_BUZZER_ACTIVE
)

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


def _i16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def _require_length(values: Sequence[int], expected: int, name: str) -> None:
    if len(values) != expected:
        raise ValueError(f"{name} requires {expected} registers")


def _require_range(value: int, minimum: int, maximum: int, name: str) -> None:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be an integer")
    if not minimum <= value <= maximum:
        raise ValueError(f"{name} must be in {minimum}..{maximum}")


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


def decode_ds18b20_snapshot(values: Sequence[int]) -> dict[str, Any]:
    """Decode the 24-register DS18B20 extension block at input 0x00B0."""

    _require_length(values, 24, "DS18B20 snapshot")
    source = values[1]
    valid_mask = values[2]
    sensors = []
    for index in range(3):
        quality = values[8 + index]
        error = values[11 + index]
        sample_time_offset = 14 + (index * 2)
        sensors.append(
            {
                "sensor_id": index,
                "sensor_type": "DS18B20",
                "valid": bool(valid_mask & (1 << index)),
                "temperature_x16": _i16(values[5 + index]),
                "temperature_unit": "1/16degC",
                "quality_code": quality,
                "quality": DS18B20_QUALITY_NAMES.get(
                    quality,
                    f"UNKNOWN({quality})",
                ),
                "error_code": error,
                "error": DS18B20_ERROR_NAMES.get(
                    error,
                    f"UNKNOWN({error})",
                ),
                "sample_time_ms": _word32(
                    values[sample_time_offset],
                    values[sample_time_offset + 1],
                ),
                "rom_short": values[21 + index],
            }
        )
    return {
        "contract_revision": values[0],
        "source_type": source,
        "source": DATA_SOURCE_NAMES.get(source, f"UNKNOWN({source})"),
        "valid_mask": valid_mask,
        "sample_id": _word32(values[3], values[4]),
        "sensors": sensors,
        "sensor_type_code": values[20],
    }


def _quality_is_displayable(quality: int) -> bool:
    return quality in (1, 5)


def _quality_is_fault(quality: int) -> bool:
    return quality in (2, 3, 4, 6)


def _alarm_phase_quality_name(quality: int) -> str:
    return DS18B20_QUALITY_NAMES.get(quality, f"UNKNOWN({quality})")


def decode_thermal_alarm(values: Sequence[int]) -> dict[str, Any]:
    """Decode and validate the fixed 29-register thermal alarm block."""

    _require_length(values, THERMAL_ALARM_INPUT_COUNT, "thermal alarm")
    revision = values[0]
    if revision != THERMAL_ALARM_CONTRACT_REVISION:
        raise ValueError(
            "thermal alarm contract revision must be "
            f"{THERMAL_ALARM_CONTRACT_REVISION}"
        )

    level = values[1]
    reason = values[2]
    flags = values[3]
    trigger_phase = values[4]
    delta_valid_raw = values[5]
    maximum_delta_x16 = _i16(values[6])
    hottest_temperature_x16 = _i16(values[7])
    hottest_phase = values[8]
    temperatures = tuple(_i16(value) for value in values[9:12])
    qualities = tuple(values[12:15])

    if flags & ~ALARM_FLAG_MASK:
        raise ValueError("thermal alarm flags contain reserved bits")
    if level not in ALARM_LEVEL_NAMES:
        raise ValueError(f"thermal alarm level is invalid: {level}")
    if reason not in ALARM_REASON_NAMES:
        raise ValueError(f"thermal alarm reason is invalid: {reason}")
    if trigger_phase not in ALARM_PHASE_NAMES:
        raise ValueError(
            f"thermal alarm trigger phase is invalid: {trigger_phase}"
        )
    if hottest_phase not in ALARM_PHASE_NAMES:
        raise ValueError(
            f"thermal alarm hottest phase is invalid: {hottest_phase}"
        )
    if delta_valid_raw not in (0, 1):
        raise ValueError("thermal alarm delta_valid must be 0 or 1")
    for index, quality in enumerate(qualities):
        if quality != 0 and quality not in DS18B20_QUALITY_NAMES:
            raise ValueError(
                f"thermal alarm phase {index} quality is invalid: {quality}"
            )

    alarm_valid = bool(flags & ALARM_FLAG_VALID)
    delta_valid = bool(delta_valid_raw)
    if not alarm_valid:
        if level != 5 or reason != 0 or trigger_phase != 0:
            raise ValueError("invalid thermal alarm must be UNKNOWN/NONE/NONE")
        if delta_valid or maximum_delta_x16 != 0:
            raise ValueError("invalid thermal alarm cannot claim a delta")
        hottest_phase = 0
        hottest_temperature_x16 = 0

    displayable = tuple(
        alarm_valid and _quality_is_displayable(quality)
        for quality in qualities
    )
    displayable_count = sum(displayable)
    fault_phases = tuple(
        index
        for index, quality in enumerate(qualities)
        if _quality_is_fault(quality)
    )

    if alarm_valid:
        for index, quality in enumerate(qualities):
            if quality == 0:
                raise ValueError(
                    f"valid thermal alarm phase {index} has no quality"
                )
        if level == 0 and reason != 0:
            raise ValueError("NORMAL thermal alarm must have reason NONE")
        if level in (1, 2, 3) and reason not in (1, 2, 3):
            raise ValueError("metric thermal alarm has an invalid reason")
        if level == 5 and reason != 0:
            raise ValueError("UNKNOWN thermal alarm must have reason NONE")
        if fault_phases and level != 4:
            raise ValueError("sensor fault quality requires SENSOR_FAULT level")

    if level == 4:
        if reason not in (4, 5, 6, 7, 8, 9):
            raise ValueError("SENSOR_FAULT has an invalid reason")
        if delta_valid or maximum_delta_x16 != 0:
            raise ValueError("SENSOR_FAULT cannot publish a temperature delta")
        if reason in (4, 5, 6, 7):
            if trigger_phase not in (1, 2, 3):
                raise ValueError("sensor fault requires an A/B/C trigger phase")
            phase_index = trigger_phase - 1
            expected_quality = {
                4: 6,
                5: 2,
                6: 3,
                7: 4,
            }[reason]
            if qualities[phase_index] != expected_quality:
                raise ValueError(
                    "sensor fault reason does not match trigger phase quality"
                )
        elif trigger_phase != 0:
            raise ValueError("non-sensor fault reason requires NONE phase")
        if reason == 8 and displayable_count >= 2:
            raise ValueError(
                "INSUFFICIENT_VALID_PHASES requires fewer than two valid phases"
            )

    if reason == 1:
        if trigger_phase not in (1, 2, 3):
            raise ValueError("temperature alarm requires an A/B/C trigger phase")
        if not displayable[trigger_phase - 1]:
            raise ValueError("temperature alarm trigger phase is not valid")
    elif reason == 2:
        if trigger_phase not in (1, 2, 3):
            raise ValueError("delta alarm requires an A/B/C trigger phase")
        if trigger_phase != hottest_phase:
            raise ValueError("delta alarm trigger phase must be hottest phase")
    elif reason == 3 and trigger_phase not in (1, 2, 3):
        raise ValueError("rise-rate alarm requires an A/B/C trigger phase")

    if delta_valid:
        if displayable_count < 2:
            raise ValueError("thermal alarm delta requires at least two phases")
        valid_temperatures = [
            temperatures[index]
            for index in range(3)
            if displayable[index]
        ]
        expected_delta = max(valid_temperatures) - min(valid_temperatures)
        if maximum_delta_x16 != expected_delta:
            raise ValueError("thermal alarm maximum delta is inconsistent")
        if hottest_phase not in (1, 2, 3):
            raise ValueError("delta alarm requires a hottest phase")
        hottest_index = hottest_phase - 1
        if not displayable[hottest_index]:
            raise ValueError("thermal alarm hottest phase is not valid")
        if hottest_temperature_x16 != temperatures[hottest_index]:
            raise ValueError("thermal alarm hottest temperature is inconsistent")
    elif maximum_delta_x16 != 0:
        raise ValueError("thermal alarm delta is invalid but nonzero")

    if hottest_phase not in (1, 2, 3):
        hottest_temperature = None
    else:
        hottest_index = hottest_phase - 1
        if not displayable[hottest_index]:
            raise ValueError("thermal alarm hottest phase is not valid")
        if hottest_temperature_x16 != temperatures[hottest_index]:
            raise ValueError("thermal alarm hottest temperature is inconsistent")
        hottest_temperature = hottest_temperature_x16

    phases = []
    for index, phase in enumerate(("A", "B", "C")):
        quality = qualities[index]
        phases.append(
            {
                "phase": phase,
                "phase_code": index + 1,
                "valid": displayable[index],
                "temperature_x16": (
                    temperatures[index] if displayable[index] else None
                ),
                "temperature_unit": "1/16degC",
                "quality_code": quality,
                "quality": _alarm_phase_quality_name(quality),
            }
        )

    return {
        "contract_revision": revision,
        "valid": alarm_valid,
        "level_code": level,
        "level": ALARM_LEVEL_NAMES[level],
        "reason_code": reason,
        "reason": ALARM_REASON_NAMES[reason],
        "flags": {
            "valid": alarm_valid,
            "latched": bool(flags & ALARM_FLAG_LATCHED),
            "acknowledged": bool(flags & ALARM_FLAG_ACKNOWLEDGED),
            "buzzer_active": bool(flags & ALARM_FLAG_BUZZER_ACTIVE),
        },
        "latched": bool(flags & ALARM_FLAG_LATCHED),
        "acknowledged": bool(flags & ALARM_FLAG_ACKNOWLEDGED),
        "buzzer_active": bool(flags & ALARM_FLAG_BUZZER_ACTIVE),
        "trigger_phase_code": trigger_phase,
        "trigger_phase": ALARM_PHASE_NAMES[trigger_phase],
        "delta_valid": delta_valid,
        "maximum_delta_x16": maximum_delta_x16 if delta_valid else None,
        "hottest_temperature_x16": hottest_temperature,
        "hottest_phase_code": hottest_phase,
        "hottest_phase": ALARM_PHASE_NAMES[hottest_phase],
        "phases": phases,
        "event_id": _word32(values[15], values[16]),
        "alarm_sample_id": _word32(values[17], values[18]),
        "duration_sec": _word32(values[19], values[20]),
        "notice_count": _word32(values[21], values[22]),
        "warning_count": _word32(values[23], values[24]),
        "critical_count": _word32(values[25], values[26]),
        "sensor_fault_count": _word32(values[27], values[28]),
    }


def decode_thermal_alarm_config(values: Sequence[int]) -> dict[str, Any]:
    """Decode the P7A alarm configuration block at holding 0x0050."""

    _require_length(values, THERMAL_ALARM_CONFIG_COUNT, "thermal alarm config")
    if values[0] != THERMAL_ALARM_CONFIG_REVISION:
        raise ValueError(
            f"unsupported thermal alarm config revision {values[0]}"
        )
    phase_notice = _i16(values[1])
    phase_warning = _i16(values[2])
    phase_critical = _i16(values[3])
    delta_notice = _i16(values[4])
    delta_warning = _i16(values[5])
    delta_critical = _i16(values[6])
    rise_notice = _i16(values[7])
    rise_warning = _i16(values[8])
    rise_critical = _i16(values[9])
    assert_samples = values[10]
    clear_samples = values[11]
    hysteresis = _i16(values[12])
    buzzer_enable = values[13]

    _require_range(phase_notice, -880, 2000, "phase_notice_x16")
    _require_range(phase_warning, -880, 2000, "phase_warning_x16")
    _require_range(phase_critical, -880, 2000, "phase_critical_x16")
    if not phase_notice < phase_warning < phase_critical:
        raise ValueError("phase thresholds must be notice < warning < critical")
    if not 0 <= delta_notice < delta_warning < delta_critical:
        raise ValueError("delta thresholds must be 0 <= notice < warning < critical")
    if not 0 <= rise_notice < rise_warning < rise_critical:
        raise ValueError("rise thresholds must be 0 <= notice < warning < critical")
    _require_range(assert_samples, 1, 100, "assert_samples")
    _require_range(clear_samples, 1, 255, "clear_samples")
    _require_range(hysteresis, 0, 0x7FFF, "hysteresis_x16")
    if phase_notice - hysteresis < -880:
        raise ValueError("phase recovery threshold is below the supported range")
    if delta_notice - hysteresis < 0:
        raise ValueError("delta recovery threshold cannot be negative")
    if rise_notice - hysteresis < 0:
        raise ValueError("rise recovery threshold cannot be negative")
    _require_range(buzzer_enable, 0, 1, "buzzer_enable")
    if values[14] != 0 or values[15] != 0:
        raise ValueError("thermal alarm config reserved registers must be 0")

    return {
        "contract_revision": values[0],
        "phase_notice_x16": phase_notice,
        "phase_warning_x16": phase_warning,
        "phase_critical_x16": phase_critical,
        "delta_notice_x16": delta_notice,
        "delta_warning_x16": delta_warning,
        "delta_critical_x16": delta_critical,
        "rise_notice_x16_per_min": rise_notice,
        "rise_warning_x16_per_min": rise_warning,
        "rise_critical_x16_per_min": rise_critical,
        "assert_samples": assert_samples,
        "clear_samples": clear_samples,
        "hysteresis_x16": hysteresis,
        "buzzer_enable": bool(buzzer_enable),
    }


def decode_alarm_config(values: Sequence[int]) -> dict[str, Any]:
    """Compatibility alias for the fixed alarm configuration decoder."""

    return decode_thermal_alarm_config(values)


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
        "event_dropped": _word32(values[0x28], values[0x29]),
    }


def command_result_name(value: int) -> str:
    return COMMAND_RESULT_NAMES.get(value, f"UNKNOWN({value})")
