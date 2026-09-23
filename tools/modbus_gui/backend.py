"""High-level service adapter used by the GUI worker."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any, Protocol

from tools.modbus_client.client import ModbusClient
from tools.modbus_client.errors import StateError, UnsupportedProtocolError
from tools.modbus_client.service import ModbusService
from tools.modbus_client.transport import MacOSTTYTransport


class GuiBackend(Protocol):
    def connect(self, port: str, address: int) -> dict[str, Any]: ...

    def disconnect(self) -> None: ...

    def poll(self) -> dict[str, Any]: ...

    def start_periodic(self, period_sec: int) -> dict[str, Any]: ...

    def stop_periodic(self) -> dict[str, Any]: ...

    def single_sample(self, command_id: int) -> dict[str, Any]: ...

    def ack_alarm(self) -> dict[str, Any]: ...

    def refresh_storage(self) -> dict[str, Any]: ...


TransportFactory = Callable[..., Any]
ClientFactory = Callable[..., Any]
ServiceFactory = Callable[..., Any]


class ModbusServiceBackend:
    """Own one Modbus client/service pair and expose only high-level operations."""

    def __init__(
        self,
        *,
        transport_factory: TransportFactory = MacOSTTYTransport,
        client_factory: ClientFactory = ModbusClient,
        service_factory: ServiceFactory = ModbusService,
        timeout_seconds: float = 3.0,
    ) -> None:
        self._transport_factory = transport_factory
        self._client_factory = client_factory
        self._service_factory = service_factory
        self._timeout_seconds = timeout_seconds
        self._transport: Any | None = None
        self._client: Any | None = None
        self._service: Any | None = None

    def connect(self, port: str, address: int) -> dict[str, Any]:
        self.close()
        client = None
        try:
            transport = self._transport_factory(port)
            client = self._client_factory(
                transport,
                timeout_seconds=self._timeout_seconds,
            )
            client.open()
            service = self._service_factory(client, address=address)
            identity = service.identity()
            status = service.status()
        except Exception:
            if client is not None:
                try:
                    client.close()
                except Exception:
                    pass
            raise

        self._transport = transport
        self._client = client
        self._service = service
        return {"identity": identity, "status": status}

    def disconnect(self) -> None:
        self.close()

    def close(self) -> None:
        client = self._client
        self._service = None
        self._client = None
        self._transport = None
        if client is not None:
            client.close()

    def identity(self) -> dict[str, Any]:
        return self._require_service().identity()

    def status(self) -> dict[str, Any]:
        return self._require_service().status()

    def poll(self) -> dict[str, Any]:
        service = self._require_service()
        status = service.status()
        temperature_snapshot = None
        thermal_alarm = None
        sensor_error = None
        alarm_error = None
        try:
            thermal_state = self._read_thermal_state(service)
            temperature_snapshot = thermal_state.get("temperature_snapshot")
            thermal_alarm = thermal_state.get("thermal_alarm")
            if thermal_alarm is None:
                thermal_alarm = thermal_state.get("alarm")
            alarm_error = thermal_state.get("alarm_error")
        except UnsupportedProtocolError as exc:
            if not exc.details.get("interface_unavailable"):
                raise
            sensor_error = exc.as_error()
            alarm_error = sensor_error
        return {
            "status": status,
            "temperature_snapshot": temperature_snapshot,
            "thermal_alarm": thermal_alarm,
            "sensor_error": sensor_error,
            "alarm_error": alarm_error,
        }

    def read_temperature_snapshot(self) -> dict[str, Any]:
        return self._read_temperature_snapshot(self._require_service())

    def _read_temperature_snapshot(self, service: Any) -> dict[str, Any]:
        method = getattr(service, "read_temperature_snapshot", None)
        if not callable(method):
            raise UnsupportedProtocolError(
                "当前 service 没有 read_temperature_snapshot()；"
                "无法读取 DS18B20 温度接口",
                interface_unavailable=True,
            )
        return method()

    def _read_thermal_state(self, service: Any) -> dict[str, Any]:
        method = getattr(service, "read_thermal_state", None)
        if callable(method):
            return method()

        temperature_snapshot = self._read_temperature_snapshot(service)
        thermal_alarm = None
        alarm_error = None
        alarm_method = getattr(service, "read_thermal_alarm", None)
        if callable(alarm_method):
            try:
                thermal_alarm = alarm_method()
            except UnsupportedProtocolError as exc:
                if not exc.details.get("interface_unavailable"):
                    raise
                alarm_error = exc.as_error()
        return {
            "temperature_snapshot": temperature_snapshot,
            "thermal_alarm": thermal_alarm,
            "alarm_error": alarm_error,
        }

    def start_periodic(self, period_sec: int) -> dict[str, Any]:
        if not 10 <= period_sec <= 3600:
            raise StateError("period must be in 10..3600", period_sec=period_sec)
        service = self._require_service()
        status = service.status()
        active = status.get("active_config")
        if not isinstance(active, Mapping) or not active.get("valid"):
            raise StateError(
                "device has no valid active configuration to preserve",
                status=status,
            )
        channel_mask = active.get("channel_mask")
        record_count = active.get("record_count")
        if (
            isinstance(channel_mask, bool)
            or not isinstance(channel_mask, int)
            or not 1 <= channel_mask <= 0x000F
        ):
            raise StateError(
                "device active channel mask is outside the GUI-supported range",
                channel_mask=channel_mask,
            )
        if (
            isinstance(record_count, bool)
            or not isinstance(record_count, int)
            or record_count < 0
        ):
            raise StateError(
                "device active record count is invalid",
                record_count=record_count,
            )

        service.config(period_sec, channel_mask, record_count)
        service.apply()
        observation = service.start()
        return {
            "observation": observation,
            "pending": bool(
                isinstance(observation, Mapping)
                and observation.get("pending", False)
            ),
            "period_sec": period_sec,
            "channel_mask": channel_mask,
            "record_count": record_count,
        }

    def stop_periodic(self) -> dict[str, Any]:
        return self._require_service().stop()

    def single_sample(self, command_id: int) -> dict[str, Any]:
        service = self._require_service()
        command = service.single(command_id)
        temperature_snapshot = None
        thermal_alarm = None
        sensor_error = None
        alarm_error = None
        try:
            thermal_state = self._read_thermal_state(service)
            temperature_snapshot = thermal_state.get("temperature_snapshot")
            thermal_alarm = thermal_state.get("thermal_alarm")
            if thermal_alarm is None:
                thermal_alarm = thermal_state.get("alarm")
            alarm_error = thermal_state.get("alarm_error")
        except UnsupportedProtocolError as exc:
            if not exc.details.get("interface_unavailable"):
                raise
            sensor_error = exc.as_error()
            alarm_error = sensor_error
        return {
            "command": command,
            "temperature_snapshot": temperature_snapshot,
            "thermal_alarm": thermal_alarm,
            "sensor_error": sensor_error,
            "alarm_error": alarm_error,
        }

    def ack_alarm(self) -> dict[str, Any]:
        service = self._require_service()
        method = getattr(service, "ack_alarm", None)
        if not callable(method):
            raise UnsupportedProtocolError(
                "当前 service 没有 ack_alarm()；无法确认热告警",
                interface_unavailable=True,
            )
        return method()

    def refresh_storage(self) -> dict[str, Any]:
        return self._require_service().storage_status()

    def _require_service(self) -> Any:
        if self._service is None:
            raise StateError("device service is not connected")
        return self._service
