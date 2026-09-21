"""Display models and pure state transitions for the Mac GUI."""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from datetime import datetime
from enum import Enum
from statistics import median
from types import MappingProxyType
from typing import Any, Mapping, Sequence


SENSOR_LAYOUT = (
    (0, "DHT11-0", "PG9 / 1WIRE_DQ"),
    (1, "DHT11-1", "PF6 / GBC_KEY"),
    (2, "DHT11-2", "PE5 / DCMI_D6"),
)
ALLOWED_SENSOR_SOURCES = {"NONE", "DHT11", "REAL_DHT11"}


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
    CHECKSUM_ERROR = "CHECKSUM_ERROR"
    RANGE_ERROR = "RANGE_ERROR"
    STALE = "STALE"
    NOT_PRESENT = "NOT_PRESENT"
    INTERFACE_UNAVAILABLE = "INTERFACE_UNAVAILABLE"
    UNKNOWN = "UNKNOWN"


QUALITY_ALIASES = {
    "OK": Quality.OK,
    "TIMEOUT": Quality.TIMEOUT,
    "TIMEOUT_RESPONSE": Quality.TIMEOUT,
    "TIMEOUT_BIT": Quality.TIMEOUT,
    "CHECKSUM": Quality.CHECKSUM_ERROR,
    "CHECKSUM_ERROR": Quality.CHECKSUM_ERROR,
    "RANGE_ERROR": Quality.RANGE_ERROR,
    "STALE": Quality.STALE,
    "NOT_PRESENT": Quality.NOT_PRESENT,
    "INTERFACE_UNAVAILABLE": Quality.INTERFACE_UNAVAILABLE,
}

QUALITY_LABELS = {
    Quality.OK: "OK",
    Quality.TIMEOUT: "超时",
    Quality.CHECKSUM_ERROR: "校验失败",
    Quality.RANGE_ERROR: "范围错误",
    Quality.STALE: "旧值",
    Quality.NOT_PRESENT: "未接入",
    Quality.INTERFACE_UNAVAILABLE: "接口未冻结",
    Quality.UNKNOWN: "未知",
}


def normalize_quality(value: Any) -> Quality:
    if not isinstance(value, str):
        raise SnapshotFormatError("sensor quality must be a string")
    key = value.strip().upper()
    return QUALITY_ALIASES.get(key, Quality.UNKNOWN)


def _optional_x10(value: Any, name: str) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise SnapshotFormatError(f"{name} must be an integer or null")
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
    temperature_x10: int | None
    humidity_x10: int | None
    quality: Quality
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
        temperature = _optional_x10(
            payload.get("temperature_x10"),
            "temperature_x10",
        )
        humidity = _optional_x10(payload.get("humidity_x10"), "humidity_x10")

        if quality is Quality.OK and (
            temperature is None or humidity is None
        ):
            raise SnapshotFormatError(
                "sensor quality OK requires temperature_x10 and humidity_x10"
            )
        if quality not in (Quality.OK, Quality.STALE):
            temperature = None
            humidity = None

        source = payload.get("source")
        if source is not None and not isinstance(source, str):
            raise SnapshotFormatError("source must be a string or null")
        _validate_source(source)
        return cls(
            sensor_id=sensor_id,
            temperature_x10=temperature,
            humidity_x10=humidity,
            quality=quality,
            sample_time=payload.get("sample_time"),
            source=source,
            received_at=received_at,
        )

    @property
    def valid(self) -> bool:
        return (
            self.quality is Quality.OK
            and self.temperature_x10 is not None
            and self.humidity_x10 is not None
        )

    @property
    def quality_label(self) -> str:
        return QUALITY_LABELS.get(self.quality, self.quality.value)

    @property
    def temperature_text(self) -> str:
        return _format_x10(self.temperature_x10, "°C")

    @property
    def humidity_text(self) -> str:
        return _format_x10(self.humidity_x10, "%RH")

    @property
    def sample_time_text(self) -> str:
        return _format_time(self.sample_time)

    @property
    def updated_at_text(self) -> str:
        if self.received_at is None:
            return "--"
        return self.received_at.strftime("%Y-%m-%d %H:%M:%S")


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
                    temperature_x10=None,
                    humidity_x10=None,
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
    minimum_temperature_x10: int | None = None
    maximum_temperature_x10: int | None = None
    median_temperature_x10: float | None = None
    minimum_humidity_x10: int | None = None
    maximum_humidity_x10: int | None = None
    maximum_temperature_delta_x10: int | None = None

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
            reading.temperature_x10
            for reading in valid
            if reading.temperature_x10 is not None
        ]
        humidities = [
            reading.humidity_x10
            for reading in valid
            if reading.humidity_x10 is not None
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
            minimum_temperature_x10=minimum_temperature,
            maximum_temperature_x10=maximum_temperature,
            median_temperature_x10=(
                float(median(temperatures)) if temperatures else None
            ),
            minimum_humidity_x10=min(humidities) if humidities else None,
            maximum_humidity_x10=max(humidities) if humidities else None,
            maximum_temperature_delta_x10=delta,
        )

    @property
    def has_valid_samples(self) -> bool:
        return bool(self.valid_sensor_ids)

    @property
    def participating_sensor_text(self) -> str:
        if not self.valid_sensor_ids:
            return "无有效样本"
        names = [f"DHT11-{sensor_id}" for sensor_id in self.valid_sensor_ids]
        return "参与计算: " + ", ".join(names)

    @property
    def minimum_temperature_text(self) -> str:
        return _format_x10(self.minimum_temperature_x10, "°C")

    @property
    def maximum_temperature_text(self) -> str:
        return _format_x10(self.maximum_temperature_x10, "°C")

    @property
    def median_temperature_text(self) -> str:
        return _format_x10(self.median_temperature_x10, "°C")

    @property
    def minimum_humidity_text(self) -> str:
        return _format_x10(self.minimum_humidity_x10, "%RH")

    @property
    def maximum_humidity_text(self) -> str:
        return _format_x10(self.maximum_humidity_x10, "%RH")

    @property
    def maximum_temperature_delta_text(self) -> str:
        return _format_x10(self.maximum_temperature_delta_x10, "°C")

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
    humidity_text: str
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
    storage: StorageSnapshot | None = None
    acquisition: AcquisitionPhase = AcquisitionPhase.STOPPED
    period_sec: int = 10
    completed_count: int = 0
    failed_count: int = 0
    last_sample_id: int | None = None
    last_error: str | None = None
    last_error_at: datetime | None = None
    last_note: str | None = None
    busy_operation: str | None = None

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
        if snapshot.sample_id is not None:
            self.last_sample_id = snapshot.sample_id

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
                        temperature_x10=None,
                        humidity_x10=None,
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
                    temperature_x10=None,
                    humidity_x10=None,
                    quality=Quality.INTERFACE_UNAVAILABLE,
                    received_at=received_at,
                )
                for sensor_id, _name, _location in SENSOR_LAYOUT
            ),
            received_at=received_at,
        )
        self.snapshot = snapshot
        self.statistics = SensorStatistics.calculate(snapshot)

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
            reading = readings.get(sensor_id)
            if reading is None:
                cards.append(
                    SensorCardView(
                        title=name,
                        location=location,
                        temperature_text="--",
                        humidity_text="--",
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
                    humidity_text=reading.humidity_text,
                    quality_text=reading.quality_label,
                    sample_time_text=reading.sample_time_text,
                    updated_at_text=reading.updated_at_text,
                )
            )
        return tuple(cards)


def _format_x10(value: int | float | None, unit: str) -> str:
    if value is None:
        return "--"
    return f"{value / 10.0:.1f} {unit}"


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
