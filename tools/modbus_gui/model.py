"""Display models and pure state transitions for the Mac GUI."""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field, replace
from datetime import datetime, timedelta
from enum import Enum
from statistics import median
from types import MappingProxyType
from typing import Any, Mapping, Sequence


SENSOR_LAYOUT = (
    (0, "DS18B20-0", "PG9 / 1-Wire"),
    (1, "DS18B20-1", "PG9 / 1-Wire"),
    (2, "DS18B20-2", "PG9 / 1-Wire"),
)
TEMPERATURE_HISTORY_LIMIT = 120
PHASE_CARD_TITLES = (
    "A 相 / DS18B20-0",
    "B 相 / DS18B20-1",
    "C 相 / DS18B20-2",
)
ALLOWED_SENSOR_SOURCES = {"NONE", "DS18B20", "REAL_DS18B20"}


class SnapshotFormatError(ValueError):
    """Raised when a high-level service snapshot does not match the GUI contract."""


class ConnectionState(str, Enum):
    DISCONNECTED = "未连接"
    CONNECTING = "连接中"
    CONNECTED = "已连接"
    DEVICE_UNRESPONSIVE = "设备无响应"
    PROTOCOL_ERROR = "协议错误"


class AcquisitionPhase(str, Enum):
    STOPPED = "已停止"
    STARTING = "启动确认中"
    RUNNING = "运行中"
    STOPPING = "停止确认中"
    START_UNCONFIRMED = "启动未确认"
    STOP_UNCONFIRMED = "停止未确认"


class Quality(str, Enum):
    OK = "OK"
    TIMEOUT = "TIMEOUT"
    CRC_ERROR = "CRC_ERROR"
    RANGE_ERROR = "RANGE_ERROR"
    STALE = "STALE"
    NOT_PRESENT = "NOT_PRESENT"
    INTERFACE_UNAVAILABLE = "INTERFACE_UNAVAILABLE"
    UNKNOWN = "UNKNOWN"


QUALITY_ALIASES = {
    "OK": Quality.OK,
    "TIMEOUT": Quality.TIMEOUT,
    "CRC_ERROR": Quality.CRC_ERROR,
    "CHECKSUM_ERROR": Quality.CRC_ERROR,
    "RANGE_ERROR": Quality.RANGE_ERROR,
    "STALE": Quality.STALE,
    "NOT_PRESENT": Quality.NOT_PRESENT,
    "INTERFACE_UNAVAILABLE": Quality.INTERFACE_UNAVAILABLE,
}

QUALITY_LABELS = {
    Quality.OK: "OK",
    Quality.TIMEOUT: "超时",
    Quality.CRC_ERROR: "校验失败",
    Quality.RANGE_ERROR: "范围错误",
    Quality.STALE: "旧值",
    Quality.NOT_PRESENT: "未接入",
    Quality.INTERFACE_UNAVAILABLE: "接口未冻结",
    Quality.UNKNOWN: "未知",
}


class AlarmLevel(str, Enum):
    NORMAL = "NORMAL"
    NOTICE = "NOTICE"
    WARNING = "WARNING"
    CRITICAL = "CRITICAL"
    SENSOR_FAULT = "SENSOR_FAULT"
    UNKNOWN = "UNKNOWN"


class AlarmReason(str, Enum):
    NONE = "NONE"
    PHASE_TEMPERATURE_HIGH = "PHASE_TEMPERATURE_HIGH"
    PHASE_DELTA_HIGH = "PHASE_DELTA_HIGH"
    RISE_RATE_HIGH = "RISE_RATE_HIGH"
    SENSOR_NOT_PRESENT = "SENSOR_NOT_PRESENT"
    SENSOR_TIMEOUT = "SENSOR_TIMEOUT"
    SENSOR_CRC_ERROR = "SENSOR_CRC_ERROR"
    SENSOR_RANGE_ERROR = "SENSOR_RANGE_ERROR"
    INSUFFICIENT_VALID_PHASES = "INSUFFICIENT_VALID_PHASES"
    CONFIG_INVALID = "CONFIG_INVALID"


class Phase(str, Enum):
    NONE = "NONE"
    A = "A"
    B = "B"
    C = "C"


ALARM_LEVEL_ALIASES = {
    "NORMAL": AlarmLevel.NORMAL,
    "NOTICE": AlarmLevel.NOTICE,
    "WARNING": AlarmLevel.WARNING,
    "CRITICAL": AlarmLevel.CRITICAL,
    "SENSOR_FAULT": AlarmLevel.SENSOR_FAULT,
    "SENSOR FAULT": AlarmLevel.SENSOR_FAULT,
    "UNKNOWN": AlarmLevel.UNKNOWN,
}

ALARM_REASON_ALIASES = {
    "NONE": AlarmReason.NONE,
    "PHASE_TEMPERATURE_HIGH": AlarmReason.PHASE_TEMPERATURE_HIGH,
    "PHASE_DELTA_HIGH": AlarmReason.PHASE_DELTA_HIGH,
    "RISE_RATE_HIGH": AlarmReason.RISE_RATE_HIGH,
    "SENSOR_NOT_PRESENT": AlarmReason.SENSOR_NOT_PRESENT,
    "SENSOR_TIMEOUT": AlarmReason.SENSOR_TIMEOUT,
    "SENSOR_CRC_ERROR": AlarmReason.SENSOR_CRC_ERROR,
    "SENSOR_RANGE_ERROR": AlarmReason.SENSOR_RANGE_ERROR,
    "INSUFFICIENT_VALID_PHASES": AlarmReason.INSUFFICIENT_VALID_PHASES,
    "CONFIG_INVALID": AlarmReason.CONFIG_INVALID,
}

PHASE_ALIASES = {
    "NONE": Phase.NONE,
    "A": Phase.A,
    "B": Phase.B,
    "C": Phase.C,
    "PHASE_A": Phase.A,
    "PHASE_B": Phase.B,
    "PHASE_C": Phase.C,
}

ALARM_LEVEL_LABELS = {
    AlarmLevel.NORMAL: "正常",
    AlarmLevel.NOTICE: "提示",
    AlarmLevel.WARNING: "警告",
    AlarmLevel.CRITICAL: "严重",
    AlarmLevel.SENSOR_FAULT: "传感器故障",
    AlarmLevel.UNKNOWN: "未知",
}

ALARM_REASON_LABELS = {
    AlarmReason.NONE: "无",
    AlarmReason.PHASE_TEMPERATURE_HIGH: "相温过高",
    AlarmReason.PHASE_DELTA_HIGH: "相间温差过大",
    AlarmReason.RISE_RATE_HIGH: "温升速率过高",
    AlarmReason.SENSOR_NOT_PRESENT: "传感器未接入",
    AlarmReason.SENSOR_TIMEOUT: "传感器超时",
    AlarmReason.SENSOR_CRC_ERROR: "传感器校验失败",
    AlarmReason.SENSOR_RANGE_ERROR: "传感器范围错误",
    AlarmReason.INSUFFICIENT_VALID_PHASES: "有效相不足",
    AlarmReason.CONFIG_INVALID: "告警配置非法",
}

ALARM_COLORS = {
    AlarmLevel.NORMAL: "#15803d",
    AlarmLevel.NOTICE: "#eab308",
    AlarmLevel.WARNING: "#f97316",
    AlarmLevel.CRITICAL: "#dc2626",
    AlarmLevel.SENSOR_FAULT: "#6b7280",
    AlarmLevel.UNKNOWN: "#475569",
}

ALARM_FOREGROUNDS = {
    AlarmLevel.NOTICE: "#111827",
    AlarmLevel.WARNING: "#ffffff",
    AlarmLevel.CRITICAL: "#ffffff",
    AlarmLevel.SENSOR_FAULT: "#ffffff",
    AlarmLevel.NORMAL: "#ffffff",
    AlarmLevel.UNKNOWN: "#ffffff",
}


def _normalize_alarm_level(value: Any) -> AlarmLevel:
    if isinstance(value, AlarmLevel):
        return value
    if isinstance(value, int) and not isinstance(value, bool):
        levels = {
            0: AlarmLevel.NORMAL,
            1: AlarmLevel.NOTICE,
            2: AlarmLevel.WARNING,
            3: AlarmLevel.CRITICAL,
            4: AlarmLevel.SENSOR_FAULT,
            5: AlarmLevel.UNKNOWN,
        }
        if value not in levels:
            raise SnapshotFormatError(f"unsupported alarm level code {value}")
        return levels[value]
    if not isinstance(value, str):
        raise SnapshotFormatError("alarm level must be a string or integer")
    key = value.strip().upper()
    if key not in ALARM_LEVEL_ALIASES:
        raise SnapshotFormatError(f"unsupported alarm level {value!r}")
    return ALARM_LEVEL_ALIASES[key]


def _normalize_alarm_reason(value: Any) -> AlarmReason:
    if isinstance(value, AlarmReason):
        return value
    if isinstance(value, int) and not isinstance(value, bool):
        reasons = {
            0: AlarmReason.NONE,
            1: AlarmReason.PHASE_TEMPERATURE_HIGH,
            2: AlarmReason.PHASE_DELTA_HIGH,
            3: AlarmReason.RISE_RATE_HIGH,
            4: AlarmReason.SENSOR_NOT_PRESENT,
            5: AlarmReason.SENSOR_TIMEOUT,
            6: AlarmReason.SENSOR_CRC_ERROR,
            7: AlarmReason.SENSOR_RANGE_ERROR,
            8: AlarmReason.INSUFFICIENT_VALID_PHASES,
            9: AlarmReason.CONFIG_INVALID,
        }
        if value not in reasons:
            raise SnapshotFormatError(f"unsupported alarm reason code {value}")
        return reasons[value]
    if not isinstance(value, str):
        raise SnapshotFormatError("alarm reason must be a string or integer")
    key = value.strip().upper()
    if key not in ALARM_REASON_ALIASES:
        raise SnapshotFormatError(f"unsupported alarm reason {value!r}")
    return ALARM_REASON_ALIASES[key]


def _normalize_phase(value: Any) -> Phase:
    if isinstance(value, Phase):
        return value
    if isinstance(value, int) and not isinstance(value, bool):
        phases = {
            0: Phase.NONE,
            1: Phase.A,
            2: Phase.B,
            3: Phase.C,
        }
        if value not in phases:
            raise SnapshotFormatError(f"unsupported phase code {value}")
        return phases[value]
    if not isinstance(value, str):
        raise SnapshotFormatError("phase must be a string or integer")
    key = value.strip().upper()
    if key not in PHASE_ALIASES:
        raise SnapshotFormatError(f"unsupported phase {value!r}")
    return PHASE_ALIASES[key]


def _required_u32(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise SnapshotFormatError(f"{name} must be an integer")
    if not 0 <= value <= 0xFFFFFFFF:
        raise SnapshotFormatError(f"{name} must fit in 32 bits")
    return value


def _required_bool(value: Any, name: str) -> bool:
    if not isinstance(value, bool):
        raise SnapshotFormatError(f"{name} must be a boolean")
    return value


def normalize_quality(value: Any) -> Quality:
    if not isinstance(value, str):
        raise SnapshotFormatError("sensor quality must be a string")
    key = value.strip().upper()
    return QUALITY_ALIASES.get(key, Quality.UNKNOWN)


def _optional_x16(value: Any, name: str) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise SnapshotFormatError(f"{name} must be an integer or null")
    return value


def _optional_rom_short(value: Any) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise SnapshotFormatError("rom_short must be an integer or null")
    if not 0 <= value <= 0xFFFF:
        raise SnapshotFormatError("rom_short must fit in 16 bits")
    return value


def _optional_int(value: Any, name: str) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise SnapshotFormatError(f"{name} must be an integer or null")
    return value


@dataclass(frozen=True)
class SensorReading:
    sensor_id: int
    temperature_x16: int | None
    quality: Quality
    rom_short: int | None = None
    sample_time: Any = None
    source: str | None = None
    received_at: datetime | None = None

    @classmethod
    def from_payload(
        cls,
        payload: Mapping[str, Any],
        *,
        default_sensor_id: int,
        received_at: datetime | None = None,
    ) -> "SensorReading":
        sensor_id = payload.get("sensor_id", default_sensor_id)
        if isinstance(sensor_id, bool) or not isinstance(sensor_id, int):
            raise SnapshotFormatError("sensor_id must be an integer")
        quality = normalize_quality(payload.get("quality", "UNKNOWN"))
        temperature = _optional_x16(
            payload.get("temperature_x16"),
            "temperature_x16",
        )
        rom_short = _optional_rom_short(payload.get("rom_short"))

        if quality is Quality.OK and temperature is None:
            raise SnapshotFormatError(
                "sensor quality OK requires temperature_x16"
            )
        if quality not in (Quality.OK, Quality.STALE):
            temperature = None

        source = payload.get("source")
        if source is not None and not isinstance(source, str):
            raise SnapshotFormatError("source must be a string or null")
        _validate_source(source)
        return cls(
            sensor_id=sensor_id,
            temperature_x16=temperature,
            quality=quality,
            rom_short=rom_short,
            sample_time=payload.get("sample_time"),
            source=source,
            received_at=received_at,
        )

    @property
    def valid(self) -> bool:
        return (
            self.quality is Quality.OK
            and self.temperature_x16 is not None
        )

    @property
    def quality_label(self) -> str:
        return QUALITY_LABELS.get(self.quality, self.quality.value)

    @property
    def temperature_text(self) -> str:
        return _format_x16(self.temperature_x16, "°C")

    @property
    def rom_text(self) -> str:
        if self.rom_short is None or self.rom_short == 0:
            return "--"
        return f"0x{self.rom_short:04X}"

    @property
    def sample_time_text(self) -> str:
        return _format_time(self.sample_time)

    @property
    def updated_at_text(self) -> str:
        if self.received_at is None:
            return "--"
        return self.received_at.strftime("%Y-%m-%d %H:%M:%S")


@dataclass(frozen=True)
class AlarmPhaseReading:
    phase: Phase
    temperature_x16: int | None
    quality: Quality

    @classmethod
    def from_payload(cls, payload: Mapping[str, Any]) -> "AlarmPhaseReading":
        if not isinstance(payload, Mapping):
            raise SnapshotFormatError("alarm phase must be a mapping")
        phase = _normalize_phase(payload.get("phase", payload.get("phase_code")))
        if phase is Phase.NONE:
            raise SnapshotFormatError("alarm phase must be A/B/C")
        quality = normalize_quality(payload.get("quality", "UNKNOWN"))
        temperature = _optional_x16(
            payload.get("temperature_x16"),
            "temperature_x16",
        )
        if quality is Quality.OK and temperature is None:
            raise SnapshotFormatError(
                "alarm phase quality OK requires temperature_x16"
            )
        if quality not in (Quality.OK, Quality.STALE):
            temperature = None
        return cls(
            phase=phase,
            temperature_x16=temperature,
            quality=quality,
        )

    @property
    def valid(self) -> bool:
        return (
            self.quality in (Quality.OK, Quality.STALE)
            and self.temperature_x16 is not None
        )

    @property
    def quality_label(self) -> str:
        return QUALITY_LABELS.get(self.quality, self.quality.value)

    @property
    def temperature_text(self) -> str:
        return _format_x16(self.temperature_x16, "°C")

    @property
    def phase_label(self) -> str:
        return f"{self.phase.value} 相" if self.phase is not Phase.NONE else "无"


@dataclass(frozen=True)
class AlarmSnapshot:
    contract_revision: int
    valid: bool
    level: AlarmLevel
    reason: AlarmReason
    trigger_phase: Phase
    hottest_phase: Phase
    delta_valid: bool
    maximum_delta_x16: int | None
    hottest_temperature_x16: int | None
    phases: tuple[AlarmPhaseReading, ...]
    latched: bool
    acknowledged: bool
    buzzer_active: bool
    event_id: int
    alarm_sample_id: int
    duration_sec: int
    notice_count: int
    warning_count: int
    critical_count: int
    sensor_fault_count: int
    received_at: datetime | None = None
    event_time: datetime | None = None

    @classmethod
    def from_payload(
        cls,
        payload: Mapping[str, Any],
        *,
        received_at: datetime | None = None,
    ) -> "AlarmSnapshot":
        if not isinstance(payload, Mapping):
            raise SnapshotFormatError("thermal alarm must be a mapping")

        contract_revision = payload.get("contract_revision")
        if (
            isinstance(contract_revision, bool)
            or not isinstance(contract_revision, int)
            or contract_revision != 1
        ):
            raise SnapshotFormatError(
                "thermal alarm contract revision must be 1"
            )

        flags = payload.get("flags")
        if flags is None:
            flags = {}
        if not isinstance(flags, Mapping):
            raise SnapshotFormatError("thermal alarm flags must be a mapping")
        valid = _required_bool(
            flags.get("valid", payload.get("valid", False)),
            "alarm valid",
        )
        latched = _required_bool(
            flags.get("latched", payload.get("latched", False)),
            "alarm latched",
        )
        acknowledged = _required_bool(
            flags.get(
                "acknowledged",
                payload.get("acknowledged", False),
            ),
            "alarm acknowledged",
        )
        buzzer_active = _required_bool(
            flags.get(
                "buzzer_active",
                payload.get("buzzer_active", False),
            ),
            "alarm buzzer_active",
        )

        level = _normalize_alarm_level(payload.get("level", "UNKNOWN"))
        reason = _normalize_alarm_reason(payload.get("reason", "NONE"))
        trigger_phase = _normalize_phase(payload.get("trigger_phase", "NONE"))
        hottest_phase = _normalize_phase(payload.get("hottest_phase", "NONE"))
        delta_valid = _required_bool(
            payload.get("delta_valid", False),
            "delta_valid",
        )
        maximum_delta_x16 = _optional_x16(
            payload.get("maximum_delta_x16"),
            "maximum_delta_x16",
        )
        hottest_temperature_x16 = _optional_x16(
            payload.get("hottest_temperature_x16"),
            "hottest_temperature_x16",
        )

        raw_phases = payload.get("phases")
        if not isinstance(raw_phases, Sequence) or isinstance(
            raw_phases,
            (str, bytes, bytearray),
        ):
            raise SnapshotFormatError("thermal alarm requires a phases list")
        by_phase: dict[Phase, AlarmPhaseReading] = {}
        for raw in raw_phases:
            reading = AlarmPhaseReading.from_payload(raw)
            if reading.phase in by_phase:
                raise SnapshotFormatError(
                    f"duplicate alarm phase {reading.phase.value}"
                )
            by_phase[reading.phase] = reading
        phases = []
        for phase in (Phase.A, Phase.B, Phase.C):
            reading = by_phase.get(phase)
            if reading is None:
                reading = AlarmPhaseReading(
                    phase=phase,
                    temperature_x16=None,
                    quality=Quality.NOT_PRESENT,
                )
            phases.append(reading)
        phase_tuple = tuple(phases)

        if not valid:
            if (
                level is not AlarmLevel.UNKNOWN
                or reason is not AlarmReason.NONE
                or trigger_phase is not Phase.NONE
                or hottest_phase is not Phase.NONE
                or delta_valid
            ):
                raise SnapshotFormatError(
                    "invalid thermal alarm must be UNKNOWN/NONE/NONE"
                )
            level = AlarmLevel.UNKNOWN
            reason = AlarmReason.NONE
            trigger_phase = Phase.NONE
            hottest_phase = Phase.NONE
            delta_valid = False
            maximum_delta_x16 = None
            hottest_temperature_x16 = None
        elif level is AlarmLevel.NORMAL and reason is not AlarmReason.NONE:
            raise SnapshotFormatError("NORMAL alarm must have reason NONE")
        elif (
            level in (AlarmLevel.NOTICE, AlarmLevel.WARNING, AlarmLevel.CRITICAL)
            and reason
            not in (
                AlarmReason.PHASE_TEMPERATURE_HIGH,
                AlarmReason.PHASE_DELTA_HIGH,
                AlarmReason.RISE_RATE_HIGH,
            )
        ):
            raise SnapshotFormatError("metric alarm has an invalid reason")
        elif level is AlarmLevel.SENSOR_FAULT and reason not in (
            AlarmReason.SENSOR_NOT_PRESENT,
            AlarmReason.SENSOR_TIMEOUT,
            AlarmReason.SENSOR_CRC_ERROR,
            AlarmReason.SENSOR_RANGE_ERROR,
            AlarmReason.INSUFFICIENT_VALID_PHASES,
            AlarmReason.CONFIG_INVALID,
        ):
            raise SnapshotFormatError("SENSOR_FAULT has an invalid reason")
        elif level is AlarmLevel.UNKNOWN and reason is not AlarmReason.NONE:
            raise SnapshotFormatError("UNKNOWN alarm must have reason NONE")
        if level is AlarmLevel.SENSOR_FAULT and delta_valid:
            raise SnapshotFormatError(
                "sensor fault cannot display a temperature delta"
            )
        if delta_valid:
            if maximum_delta_x16 is None or maximum_delta_x16 < 0:
                raise SnapshotFormatError(
                    "delta_valid requires a non-negative maximum_delta_x16"
                )
            if sum(reading.valid for reading in phase_tuple) < 2:
                raise SnapshotFormatError(
                    "delta_valid requires at least two valid phases"
                )
            valid_temperatures = [
                reading.temperature_x16
                for reading in phase_tuple
                if reading.valid and reading.temperature_x16 is not None
            ]
            expected_delta = max(valid_temperatures) - min(valid_temperatures)
            if maximum_delta_x16 != expected_delta:
                raise SnapshotFormatError(
                    "maximum_delta_x16 is inconsistent with phase temperatures"
                )
        else:
            maximum_delta_x16 = None
        if hottest_phase is not Phase.NONE:
            hottest_index = list((Phase.A, Phase.B, Phase.C)).index(
                hottest_phase
            )
            hottest_reading = phase_tuple[hottest_index]
            if not hottest_reading.valid:
                raise SnapshotFormatError(
                    "hottest phase has no valid temperature"
                )
            if (
                hottest_temperature_x16 is not None
                and hottest_temperature_x16 != hottest_reading.temperature_x16
            ):
                raise SnapshotFormatError(
                    "hottest temperature does not match phase temperature"
                )
            hottest_temperature_x16 = hottest_reading.temperature_x16
        else:
            hottest_temperature_x16 = None

        event_id = _required_u32(payload.get("event_id", 0), "event_id")
        alarm_sample_id = _required_u32(
            payload.get("alarm_sample_id", 0),
            "alarm_sample_id",
        )
        duration_sec = _required_u32(
            payload.get("duration_sec", 0),
            "duration_sec",
        )
        notice_count = _required_u32(
            payload.get("notice_count", 0),
            "notice_count",
        )
        warning_count = _required_u32(
            payload.get("warning_count", 0),
            "warning_count",
        )
        critical_count = _required_u32(
            payload.get("critical_count", 0),
            "critical_count",
        )
        sensor_fault_count = _required_u32(
            payload.get("sensor_fault_count", 0),
            "sensor_fault_count",
        )

        event_time = payload.get("event_time")
        if event_time is not None and not isinstance(event_time, datetime):
            raise SnapshotFormatError("event_time must be a datetime or null")
        if (
            event_time is None
            and valid
            and level
            not in (AlarmLevel.NORMAL, AlarmLevel.UNKNOWN)
            and received_at is not None
        ):
            event_time = received_at - timedelta(seconds=duration_sec)

        return cls(
            contract_revision=contract_revision,
            valid=valid,
            level=level,
            reason=reason,
            trigger_phase=trigger_phase,
            hottest_phase=hottest_phase,
            delta_valid=delta_valid,
            maximum_delta_x16=maximum_delta_x16,
            hottest_temperature_x16=hottest_temperature_x16,
            phases=phase_tuple,
            latched=latched,
            acknowledged=acknowledged,
            buzzer_active=buzzer_active,
            event_id=event_id,
            alarm_sample_id=alarm_sample_id,
            duration_sec=duration_sec,
            notice_count=notice_count,
            warning_count=warning_count,
            critical_count=critical_count,
            sensor_fault_count=sensor_fault_count,
            received_at=received_at,
            event_time=event_time,
        )

    @property
    def level_label(self) -> str:
        return ALARM_LEVEL_LABELS.get(self.level, self.level.value)

    @property
    def reason_label(self) -> str:
        return ALARM_REASON_LABELS.get(self.reason, self.reason.value)

    @property
    def trigger_phase_label(self) -> str:
        return (
            f"{self.trigger_phase.value} 相"
            if self.trigger_phase is not Phase.NONE
            else "无"
        )

    @property
    def hottest_phase_label(self) -> str:
        return (
            f"{self.hottest_phase.value} 相"
            if self.hottest_phase is not Phase.NONE
            else "无"
        )

    @property
    def color(self) -> str:
        return ALARM_COLORS.get(self.level, ALARM_COLORS[AlarmLevel.UNKNOWN])

    @property
    def foreground(self) -> str:
        return ALARM_FOREGROUNDS.get(
            self.level,
            ALARM_FOREGROUNDS[AlarmLevel.UNKNOWN],
        )

    @property
    def maximum_delta_text(self) -> str:
        if not self.delta_valid:
            return "--"
        return _format_x16(self.maximum_delta_x16, "°C")

    @property
    def hottest_temperature_text(self) -> str:
        return _format_x16(self.hottest_temperature_x16, "°C")

    @property
    def buzzer_text(self) -> str:
        return "蜂鸣中" if self.buzzer_active else "静音"

    @property
    def acknowledged_text(self) -> str:
        return "已确认" if self.acknowledged else "未确认"

    @property
    def event_time_text(self) -> str:
        if self.event_time is None:
            return "--"
        return self.event_time.strftime("%Y-%m-%d %H:%M:%S")

    @property
    def duration_text(self) -> str:
        return f"{self.duration_sec} s"

    @property
    def active(self) -> bool:
        return self.valid and self.level not in (
            AlarmLevel.NORMAL,
            AlarmLevel.UNKNOWN,
        )

    def phase_reading(self, phase: Phase) -> AlarmPhaseReading | None:
        for reading in self.phases:
            if reading.phase is phase:
                return reading
        return None


@dataclass(frozen=True)
class TemperatureSnapshot:
    sample_id: int | None
    sensors: tuple[SensorReading, ...]
    source: str | None = None
    received_at: datetime | None = None

    @classmethod
    def from_payload(
        cls,
        payload: Mapping[str, Any],
        *,
        received_at: datetime | None = None,
    ) -> "TemperatureSnapshot":
        if not isinstance(payload, Mapping):
            raise SnapshotFormatError("temperature snapshot must be a mapping")
        raw_sensors = payload.get("sensors")
        if not isinstance(raw_sensors, Sequence) or isinstance(
            raw_sensors,
            (str, bytes, bytearray),
        ):
            raise SnapshotFormatError("temperature snapshot requires a sensors list")

        sample_id = _optional_int(payload.get("sample_id"), "sample_id")
        source = payload.get("source")
        if source is not None and not isinstance(source, str):
            raise SnapshotFormatError("source must be a string or null")
        _validate_source(source)

        by_id: dict[int, SensorReading] = {}
        for index, raw in enumerate(raw_sensors):
            if not isinstance(raw, Mapping):
                raise SnapshotFormatError("each sensor must be a mapping")
            reading = SensorReading.from_payload(
                raw,
                default_sensor_id=index,
                received_at=received_at,
            )
            if reading.sensor_id in by_id:
                raise SnapshotFormatError(
                    f"duplicate sensor_id {reading.sensor_id}"
                )
            if reading.sensor_id not in (0, 1, 2):
                raise SnapshotFormatError(
                    f"unsupported sensor_id {reading.sensor_id}"
                )
            by_id[reading.sensor_id] = reading

        sensors: list[SensorReading] = []
        for sensor_id, _name, _location in SENSOR_LAYOUT:
            reading = by_id.get(sensor_id)
            if reading is None:
                reading = SensorReading(
                    sensor_id=sensor_id,
                    temperature_x16=None,
                    quality=Quality.NOT_PRESENT,
                    source=source,
                    received_at=received_at,
                )
            sensors.append(reading)
        return cls(
            sample_id=sample_id,
            sensors=tuple(sensors),
            source=source,
            received_at=received_at,
        )

    def as_stale(self) -> "TemperatureSnapshot":
        return replace(
            self,
            sensors=tuple(
                replace(reading, quality=Quality.STALE)
                for reading in self.sensors
            ),
        )


@dataclass(frozen=True)
class SensorStatistics:
    valid_sensor_ids: tuple[int, ...] = ()
    minimum_temperature_x16: int | None = None
    maximum_temperature_x16: int | None = None
    median_temperature_x16: float | None = None
    maximum_temperature_delta_x16: int | None = None

    @classmethod
    def calculate(
        cls,
        snapshot: TemperatureSnapshot | None,
    ) -> "SensorStatistics":
        if snapshot is None:
            return cls()
        valid = [reading for reading in snapshot.sensors if reading.valid]
        if not valid:
            return cls()

        temperatures = [
            reading.temperature_x16
            for reading in valid
            if reading.temperature_x16 is not None
        ]
        minimum_temperature = min(temperatures) if temperatures else None
        maximum_temperature = max(temperatures) if temperatures else None
        delta = (
            maximum_temperature - minimum_temperature
            if (
                minimum_temperature is not None
                and maximum_temperature is not None
                and len(temperatures) >= 2
            )
            else None
        )
        return cls(
            valid_sensor_ids=tuple(reading.sensor_id for reading in valid),
            minimum_temperature_x16=minimum_temperature,
            maximum_temperature_x16=maximum_temperature,
            median_temperature_x16=(
                float(median(temperatures)) if temperatures else None
            ),
            maximum_temperature_delta_x16=delta,
        )

    @property
    def has_valid_samples(self) -> bool:
        return bool(self.valid_sensor_ids)

    @property
    def participating_sensor_text(self) -> str:
        if not self.valid_sensor_ids:
            return "无有效样本"
        names = [f"DS18B20-{sensor_id}" for sensor_id in self.valid_sensor_ids]
        return "参与计算: " + ", ".join(names)

    @property
    def minimum_temperature_text(self) -> str:
        return _format_x16(self.minimum_temperature_x16, "°C")

    @property
    def maximum_temperature_text(self) -> str:
        return _format_x16(self.maximum_temperature_x16, "°C")

    @property
    def median_temperature_text(self) -> str:
        return _format_x16(self.median_temperature_x16, "°C")

    @property
    def maximum_temperature_delta_text(self) -> str:
        return _format_x16(self.maximum_temperature_delta_x16, "°C")

    @property
    def valid_count_text(self) -> str:
        return f"{len(self.valid_sensor_ids)} / 3"


@dataclass(frozen=True)
class StorageSnapshot:
    values: Mapping[str, Any]

    @classmethod
    def from_payload(cls, payload: Mapping[str, Any]) -> "StorageSnapshot":
        if not isinstance(payload, Mapping):
            raise SnapshotFormatError("storage status must be a mapping")
        return cls(MappingProxyType(dict(payload)))

    def value(self, key: str, default: Any = "--") -> Any:
        return self.values.get(key, default)

    def text(self, key: str) -> str:
        value = self.value(key)
        return "--" if value is None else str(value)


@dataclass(frozen=True)
class SensorCardView:
    title: str
    location: str
    temperature_text: str
    rom_text: str
    quality_text: str
    sample_time_text: str
    updated_at_text: str


@dataclass(frozen=True)
class ActionAvailability:
    connect: bool
    disconnect: bool
    refresh_sensors: bool
    single_sample: bool
    start_periodic: bool
    stop_periodic: bool
    refresh_storage: bool
    ack_alarm: bool


@dataclass
class GuiState:
    connection: ConnectionState = ConnectionState.DISCONNECTED
    session_open: bool = False
    port: str = ""
    address: int = 1
    identity: Mapping[str, Any] | None = None
    device_status: Mapping[str, Any] | None = None
    snapshot: TemperatureSnapshot | None = None
    statistics: SensorStatistics = field(default_factory=SensorStatistics)
    alarm: AlarmSnapshot | None = None
    storage: StorageSnapshot | None = None
    acquisition: AcquisitionPhase = AcquisitionPhase.STOPPED
    period_sec: int = 30
    completed_count: int = 0
    failed_count: int = 0
    last_sample_id: int | None = None
    last_error: str | None = None
    last_error_at: datetime | None = None
    last_note: str | None = None
    busy_operation: str | None = None
    temperature_history: deque[
        tuple[float | None, float | None, float | None]
    ] = field(
        default_factory=lambda: deque(maxlen=TEMPERATURE_HISTORY_LIMIT)
    )

    @property
    def is_busy(self) -> bool:
        return self.busy_operation is not None

    @property
    def acquisition_text(self) -> str:
        return self.acquisition.value

    def set_error(self, message: str, when: datetime) -> None:
        self.last_error = message
        self.last_error_at = when

    def set_connected_identity(
        self,
        identity: Mapping[str, Any],
        status: Mapping[str, Any],
        *,
        update_period: bool,
    ) -> None:
        self.connection = ConnectionState.CONNECTED
        self.session_open = True
        self.identity = dict(identity)
        self.device_status = dict(status)
        if update_period:
            active = status.get("active_config")
            if isinstance(active, Mapping):
                period = active.get("period_sec")
                if isinstance(period, int) and not isinstance(period, bool):
                    self.period_sec = period
        self._apply_run_state(status)

    def update_device_status(self, status: Mapping[str, Any]) -> None:
        self.connection = ConnectionState.CONNECTED
        self.session_open = True
        self.device_status = dict(status)
        self._apply_run_state(status)

    def _apply_run_state(self, status: Mapping[str, Any]) -> None:
        run_state = status.get("run_state_code")
        if run_state == 1:
            if self.acquisition is not AcquisitionPhase.STOPPING:
                self.acquisition = AcquisitionPhase.RUNNING
        elif run_state == 0:
            self.acquisition = AcquisitionPhase.STOPPED

    def set_snapshot(self, snapshot: TemperatureSnapshot) -> None:
        self.snapshot = snapshot
        self.statistics = SensorStatistics.calculate(snapshot)
        self.temperature_history.append(
            tuple(
                (
                    reading.temperature_x16 / 16.0
                    if reading.valid
                    and reading.temperature_x16 is not None
                    else None
                )
                for reading in snapshot.sensors
            )
        )
        if snapshot.sample_id is not None:
            self.last_sample_id = snapshot.sample_id

    def clear_temperature_history(self) -> None:
        self.temperature_history.clear()

    def set_alarm(self, alarm: AlarmSnapshot) -> None:
        if (
            self.alarm is not None
            and self.alarm.event_id == alarm.event_id
            and alarm.event_id != 0
            and self.alarm.event_time is not None
        ):
            alarm = replace(alarm, event_time=self.alarm.event_time)
        self.alarm = alarm

    def mark_snapshot_stale(self) -> None:
        if self.snapshot is not None:
            self.snapshot = self.snapshot.as_stale()
        else:
            received_at = datetime.now()
            self.snapshot = TemperatureSnapshot(
                sample_id=None,
                sensors=tuple(
                    SensorReading(
                        sensor_id=sensor_id,
                        temperature_x16=None,
                        quality=Quality.TIMEOUT,
                        received_at=received_at,
                    )
                    for sensor_id, _name, _location in SENSOR_LAYOUT
                ),
                received_at=received_at,
            )
        self.statistics = SensorStatistics.calculate(self.snapshot)

    def set_sensor_interface_unavailable(self, received_at: datetime) -> None:
        snapshot = TemperatureSnapshot(
            sample_id=None,
            sensors=tuple(
                SensorReading(
                    sensor_id=sensor_id,
                    temperature_x16=None,
                    quality=Quality.INTERFACE_UNAVAILABLE,
                    received_at=received_at,
                )
                for sensor_id, _name, _location in SENSOR_LAYOUT
            ),
            received_at=received_at,
        )
        self.snapshot = snapshot
        self.statistics = SensorStatistics.calculate(snapshot)

    def set_alarm_interface_unavailable(self) -> None:
        self.alarm = None

    def set_storage(self, payload: Mapping[str, Any]) -> None:
        self.storage = StorageSnapshot.from_payload(payload)

    def availability(self) -> ActionAvailability:
        busy = self.is_busy
        connected = self.connection is ConnectionState.CONNECTED
        startable = self.acquisition is AcquisitionPhase.STOPPED
        stoppable = self.acquisition in {
            AcquisitionPhase.STARTING,
            AcquisitionPhase.RUNNING,
            AcquisitionPhase.STOPPING,
            AcquisitionPhase.STOP_UNCONFIRMED,
        }
        single_allowed = self.acquisition in {
            AcquisitionPhase.STOPPED,
            AcquisitionPhase.RUNNING,
        }
        return ActionAvailability(
            connect=(
                not busy
                and self.connection
                not in {ConnectionState.CONNECTING, ConnectionState.CONNECTED}
            ),
            disconnect=not busy and self.session_open,
            refresh_sensors=not busy and connected,
            single_sample=not busy and connected and single_allowed,
            start_periodic=not busy and connected and startable,
            stop_periodic=not busy and self.session_open and stoppable,
            refresh_storage=not busy and connected,
            ack_alarm=(
                not busy
                and connected
                and self.session_open
                and self.alarm is not None
                and self.alarm.active
                and not self.alarm.acknowledged
            ),
        )

    def sensor_cards(self) -> tuple[SensorCardView, ...]:
        readings = {}
        if self.snapshot is not None:
            readings = {
                reading.sensor_id: reading
                for reading in self.snapshot.sensors
            }
        cards: list[SensorCardView] = []
        for sensor_id, name, location in SENSOR_LAYOUT:
            name = PHASE_CARD_TITLES[sensor_id]
            reading = readings.get(sensor_id)
            if reading is None:
                cards.append(
                    SensorCardView(
                        title=name,
                        location=location,
                        temperature_text="--",
                        rom_text="--",
                        quality_text="等待采样",
                        sample_time_text="--",
                        updated_at_text="--",
                    )
                )
                continue
            cards.append(
                SensorCardView(
                    title=name,
                    location=location,
                    temperature_text=reading.temperature_text,
                    rom_text=reading.rom_text,
                    quality_text=reading.quality_label,
                    sample_time_text=reading.sample_time_text,
                    updated_at_text=reading.updated_at_text,
                )
            )
        return tuple(cards)


def _format_x16(value: int | float | None, unit: str) -> str:
    if value is None:
        return "--"
    return f"{value / 16.0:.2f} {unit}"


def _format_time(value: Any) -> str:
    if value is None:
        return "--"
    if isinstance(value, datetime):
        return value.strftime("%Y-%m-%d %H:%M:%S")
    return str(value)


def _validate_source(source: str | None) -> None:
    if source is None:
        return
    if source.strip().upper() not in ALLOWED_SENSOR_SOURCES:
        raise SnapshotFormatError(
            f"unsupported environmental sensor source {source!r}"
        )
