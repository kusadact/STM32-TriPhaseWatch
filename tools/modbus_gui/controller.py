"""Thread-free GUI state controller driven by worker result polling."""

from __future__ import annotations

from collections.abc import Mapping
from datetime import datetime
import time
from typing import Any, Callable

from tools.modbus_client.errors import (
    ModbusClientError,
    ProtocolError,
    StateError,
    TransactionTimeout,
    TransportError,
    UnsupportedProtocolError,
)

from .backend import GuiBackend
from .model import (
    AcquisitionPhase,
    AlarmSnapshot,
    ConnectionState,
    GuiState,
    SnapshotFormatError,
    TemperatureSnapshot,
)
from .worker import CommandWorker, OperationResult


class GuiController:
    """Own the worker and apply operation results in the Tk main thread."""

    def __init__(
        self,
        backend: GuiBackend,
        *,
        poll_interval: float = 2.0,
        storage_interval: float = 10.0,
        close_timeout: float = 5.0,
        clock: Callable[[], float] = time.monotonic,
        wall_clock: Callable[[], datetime] = datetime.now,
    ) -> None:
        if poll_interval < 2.0:
            raise ValueError("GUI sensor poll interval must be at least 2 seconds")
        if storage_interval <= 0:
            raise ValueError("storage interval must be positive")
        self._backend = backend
        self._worker = CommandWorker(backend)
        self._poll_interval = poll_interval
        self._storage_interval = storage_interval
        self._close_timeout = close_timeout
        self._clock = clock
        self._wall_clock = wall_clock
        self.state = GuiState()
        # Worker operation id -> (operation name, is_background). Background
        # polls must not gate the UI, so they are tracked separately from the
        # one user-initiated operation that is allowed to be in flight.
        self._pending: dict[int, tuple[str, bool]] = {}
        self._pending_user_operation: str | None = None
        self._next_poll_at: float | None = None
        self._next_storage_at: float | None = None
        self._last_command_id = 0
        self._closing = False

    def connect(self, port: str, address: int) -> bool:
        if self.state.is_busy or self._closing:
            return False
        port = port.strip()
        if not port:
            self._set_local_error("请选择串口")
            return False
        if isinstance(address, bool) or not isinstance(address, int):
            self._set_local_error("Modbus 地址必须是 1..247 的整数")
            return False
        if not 1 <= address <= 247:
            self._set_local_error("Modbus 地址必须在 1..247")
            return False

        self.state.port = port
        self.state.address = address
        self.state.connection = ConnectionState.CONNECTING
        self.state.session_open = False
        self.state.identity = None
        self.state.device_status = None
        self.state.snapshot = None
        self.state.statistics = type(self.state.statistics)()
        self.state.alarm = None
        self.state.storage = None
        self.state.completed_count = 0
        self.state.failed_count = 0
        self.state.last_sample_id = None
        self.state.last_note = "正在打开串口并验证设备身份"
        self._next_poll_at = None
        self._next_storage_at = None
        return self._submit("connect", port, address)

    def disconnect(self) -> bool:
        if self.state.is_busy or self._closing or not self.state.session_open:
            return False
        return self._submit("disconnect")

    def refresh_sensors(self) -> bool:
        if (
            self.state.is_busy
            or self._closing
            or self.state.connection is not ConnectionState.CONNECTED
        ):
            return False
        self._next_poll_at = None
        return self._submit("poll")

    def refresh_storage(self) -> bool:
        if (
            self.state.is_busy
            or self._closing
            or self.state.connection is not ConnectionState.CONNECTED
        ):
            return False
        self._next_storage_at = None
        return self._submit("refresh_storage")

    def start_periodic(self, period_sec: int | str) -> bool:
        if self.state.is_busy or self._closing:
            return False
        try:
            period = int(period_sec)
        except (TypeError, ValueError):
            self._set_local_error("周期秒数必须是 10..3600 的整数")
            return False
        if not 10 <= period <= 3600:
            self._set_local_error("周期秒数必须在 10..3600")
            return False
        if self.state.connection is not ConnectionState.CONNECTED:
            self._set_local_error("设备未连接，不能开始周期采集")
            return False
        if self.state.acquisition is not AcquisitionPhase.STOPPED:
            return False

        self.state.period_sec = period
        self.state.acquisition = AcquisitionPhase.STARTING
        self.state.last_note = "等待设备确认启动"
        return self._submit("start_periodic", period)

    def stop_periodic(self) -> bool:
        if self.state.is_busy or self._closing:
            return False
        if self.state.acquisition not in {
            AcquisitionPhase.STARTING,
            AcquisitionPhase.RUNNING,
            AcquisitionPhase.STOPPING,
            AcquisitionPhase.STOP_UNCONFIRMED,
        }:
            return False
        self.state.acquisition = AcquisitionPhase.STOPPING
        self.state.last_note = "等待设备确认停止和存储收尾"
        return self._submit("stop_periodic")

    def single_sample(self) -> bool:
        if (
            self.state.is_busy
            or self._closing
            or self.state.connection is not ConnectionState.CONNECTED
        ):
            return False
        if self.state.acquisition not in {
            AcquisitionPhase.STOPPED,
            AcquisitionPhase.RUNNING,
        }:
            return False
        command_id = self._new_command_id()
        return self._submit("single_sample", command_id)

    def ack_alarm(self) -> bool:
        if (
            self.state.is_busy
            or self._closing
            or self.state.connection is not ConnectionState.CONNECTED
            or self.state.alarm is None
            or not self.state.alarm.active
            or self.state.alarm.acknowledged
        ):
            return False
        self.state.last_note = "正在确认热告警"
        return self._submit("ack_alarm")

    def tick(self, now: float | None = None) -> bool:
        if self._closing or self.state.is_busy:
            return False
        if self.state.connection not in {
            ConnectionState.CONNECTED,
            ConnectionState.DEVICE_UNRESPONSIVE,
            ConnectionState.PROTOCOL_ERROR,
        }:
            return False
        now = self._clock() if now is None else now
        if self._next_poll_at is not None and now >= self._next_poll_at:
            self._next_poll_at = None
            return self._submit("poll", background=True)
        if (
            self.state.connection is ConnectionState.CONNECTED
            and self._next_storage_at is not None
            and now >= self._next_storage_at
        ):
            self._next_storage_at = None
            return self._submit("refresh_storage", background=True)
        return False

    def poll(self) -> None:
        for result in self._worker.drain_results():
            if self._closing:
                continue
            self._dispatch(result)

    def shutdown(self, timeout: float | None = None) -> bool:
        if self._closing:
            return not self._worker.is_alive
        self._closing = True
        self._next_poll_at = None
        self._next_storage_at = None
        limit = self._close_timeout if timeout is None else timeout
        exited = self._worker.close(limit)
        self._pending.clear()
        self._pending_user_operation = None
        self.state.busy_operation = None
        self.state.connection = ConnectionState.DISCONNECTED
        self.state.session_open = False
        return exited

    @property
    def worker_close_error(self) -> Exception | None:
        return self._worker.close_error

    def _submit(
        self, operation: str, *args: Any, background: bool = False
    ) -> bool:
        if self._closing:
            return False
        if not background and self._pending_user_operation is not None:
            return False
        try:
            operation_id = self._worker.submit(operation, *args)
        except Exception as exc:
            self._record_error(exc)
            return False
        self._pending[operation_id] = (operation, background)
        if not background:
            self._pending_user_operation = operation
            self.state.busy_operation = operation
        return True

    def _dispatch(self, result: OperationResult) -> None:
        entry = self._pending.pop(result.operation_id, None)
        background = bool(entry is not None and entry[1])
        if not background:
            self._pending_user_operation = None
            self.state.busy_operation = None
        try:
            if result.error is not None:
                self._handle_failure(result.operation, result.error)
            else:
                self._handle_success(result.operation, result.value)
        except Exception as exc:
            self._handle_failure(result.operation, exc)

    def _handle_success(self, operation: str, value: Any) -> None:
        if operation == "connect":
            self._handle_connect(value)
        elif operation == "disconnect":
            self._handle_disconnect()
        elif operation == "poll":
            self._handle_poll(value)
        elif operation == "refresh_storage":
            self._handle_storage(value)
        elif operation == "start_periodic":
            self._handle_start(value)
        elif operation == "stop_periodic":
            self._handle_stop(value)
        elif operation == "single_sample":
            self._handle_single(value)
        elif operation == "ack_alarm":
            self._handle_ack_alarm(value)

    def _handle_connect(self, value: Mapping[str, Any]) -> None:
        identity = value.get("identity")
        status = value.get("status")
        if not isinstance(identity, Mapping) or not isinstance(status, Mapping):
            raise ProtocolError("connect response is missing identity or status")
        self.state.set_connected_identity(
            identity,
            status,
            update_period=True,
        )
        self.state.last_note = (
            f"已连接 {identity.get('reported_version', 'unknown')} "
            f"/ protocol {identity.get('protocol_version', 'unknown')}"
        )
        now = self._clock()
        self._next_poll_at = now
        self._next_storage_at = now + self._storage_interval

    def _handle_disconnect(self) -> None:
        self.state.connection = ConnectionState.DISCONNECTED
        self.state.session_open = False
        self.state.identity = None
        self.state.device_status = None
        self.state.alarm = None
        self.state.last_note = "已断开"
        self._next_poll_at = None
        self._next_storage_at = None

    def _handle_poll(self, value: Mapping[str, Any]) -> None:
        status = value.get("status")
        if not isinstance(status, Mapping):
            raise ProtocolError("poll response is missing status")
        self.state.update_device_status(status)

        payload = value.get("temperature_snapshot")
        if payload is not None:
            snapshot = TemperatureSnapshot.from_payload(
                payload,
                received_at=self._wall_clock(),
            )
            self.state.set_snapshot(snapshot)
            self.state.completed_count += 1

        alarm_payload = value.get("thermal_alarm")
        if alarm_payload is None:
            alarm_payload = value.get("alarm")
        if alarm_payload is not None:
            self.state.set_alarm(
                AlarmSnapshot.from_payload(
                    alarm_payload,
                    received_at=self._wall_clock(),
                )
            )

        alarm_error = value.get("alarm_error")
        if isinstance(alarm_error, Mapping) and alarm_error.get(
            "interface_unavailable"
        ):
            self.state.set_alarm_interface_unavailable()

        sensor_error = value.get("sensor_error")
        if isinstance(sensor_error, Mapping):
            if sensor_error.get("interface_unavailable"):
                self.state.set_sensor_interface_unavailable(self._wall_clock())
                self.state.last_note = str(sensor_error.get("message", "接口未冻结"))
            else:
                self.state.last_note = str(sensor_error.get("message", "传感器读取失败"))
        self._next_poll_at = self._clock() + self._poll_interval

    def _handle_storage(self, value: Mapping[str, Any]) -> None:
        self.state.set_storage(value)
        self._next_storage_at = self._clock() + self._storage_interval

    def _handle_start(self, value: Mapping[str, Any]) -> None:
        pending = bool(value.get("pending", False))
        period = value.get("period_sec")
        if isinstance(period, int) and not isinstance(period, bool):
            self.state.period_sec = period
        if pending:
            self.state.acquisition = AcquisitionPhase.STARTING
            self.state.last_note = "设备尚未确认周期运行"
            return

        run_state = _run_state_code(value)
        if run_state == 1:
            self.state.acquisition = AcquisitionPhase.RUNNING
            self.state.last_note = "设备已确认周期运行"
        elif run_state == 0:
            self.state.acquisition = AcquisitionPhase.STOPPED
            self.state.last_note = "启动命令已接受，设备当前已停止"
        else:
            self.state.acquisition = AcquisitionPhase.START_UNCONFIRMED
            self.state.last_note = "启动响应缺少运行状态"

    def _handle_stop(self, value: Mapping[str, Any]) -> None:
        run_state = _run_state_code(value)
        if run_state == 0:
            self.state.acquisition = AcquisitionPhase.STOPPED
            self.state.last_note = "设备已确认停止"
        else:
            self.state.acquisition = AcquisitionPhase.STOP_UNCONFIRMED
            self.state.last_note = "停止响应未确认设备已停止"

    def _handle_single(self, value: Mapping[str, Any]) -> None:
        payload = value.get("temperature_snapshot")
        if payload is not None:
            snapshot = TemperatureSnapshot.from_payload(
                payload,
                received_at=self._wall_clock(),
            )
            self.state.set_snapshot(snapshot)
            self.state.completed_count += 1
            self.state.last_note = "单次采样完成"
        alarm_payload = value.get("thermal_alarm")
        if alarm_payload is None:
            alarm_payload = value.get("alarm")
        if alarm_payload is not None:
            self.state.set_alarm(
                AlarmSnapshot.from_payload(
                    alarm_payload,
                    received_at=self._wall_clock(),
                )
            )
        sensor_error = value.get("sensor_error")
        if isinstance(sensor_error, Mapping):
            self.state.last_note = (
                "单次命令已接受；"
                + str(sensor_error.get("message", "温度接口未冻结"))
            )

    def _handle_ack_alarm(self, value: Mapping[str, Any]) -> None:
        alarm_payload = value.get("thermal_alarm")
        if alarm_payload is None:
            alarm_payload = value.get("alarm")
        if not isinstance(alarm_payload, Mapping):
            raise ProtocolError("ACK_ALARM response is missing thermal_alarm")
        self.state.set_alarm(
            AlarmSnapshot.from_payload(
                alarm_payload,
                received_at=self._wall_clock(),
            )
        )
        if self.state.alarm is None or not self.state.alarm.acknowledged:
            raise StateError(
                "ACK_ALARM was accepted but acknowledgement was not observed",
                write_acknowledged=True,
            )
        self.state.last_note = "热告警已确认，实际告警状态保持不变"

    def _handle_failure(self, operation: str, error: Exception) -> None:
        self._record_error(error)
        if operation == "connect":
            self.state.connection = _connection_state_for_error(error)
            self.state.session_open = False
            self.state.identity = None
            self.state.device_status = None
            self._next_poll_at = None
            self._next_storage_at = None
            return
        if operation == "disconnect":
            self.state.connection = ConnectionState.DISCONNECTED
            self.state.session_open = False
            self._next_poll_at = None
            self._next_storage_at = None
            return
        if operation in {
            "poll",
            "refresh_storage",
            "single_sample",
            "ack_alarm",
        }:
            state = _connection_state_for_error(error)
            if state is not ConnectionState.CONNECTED:
                self.state.connection = state
                self.state.mark_snapshot_stale()
            if operation == "poll":
                self.state.failed_count += 1
                self._next_poll_at = self._clock() + self._poll_interval
            elif operation == "refresh_storage":
                self._next_storage_at = self._clock() + self._storage_interval
            elif operation == "ack_alarm":
                self.state.last_note = "热告警确认失败，实际告警状态未改变"
            return
        if operation == "start_periodic":
            connection = _connection_state_for_error(error)
            if connection is not ConnectionState.CONNECTED:
                self.state.connection = connection
            if _write_acknowledged(error) or isinstance(
                error,
                (TransactionTimeout, TransportError),
            ):
                self.state.acquisition = AcquisitionPhase.START_UNCONFIRMED
            else:
                self.state.acquisition = AcquisitionPhase.STOPPED
            return
        if operation == "stop_periodic":
            connection = _connection_state_for_error(error)
            if connection is not ConnectionState.CONNECTED:
                self.state.connection = connection
            self.state.acquisition = AcquisitionPhase.STOP_UNCONFIRMED

    def _record_error(self, error: Exception) -> None:
        kind = error.kind if isinstance(error, ModbusClientError) else "gui"
        message = error.message if isinstance(error, ModbusClientError) else str(error)
        self.state.set_error(f"{kind}: {message}", self._wall_clock())

    def _set_local_error(self, message: str) -> None:
        self.state.set_error(f"input: {message}", self._wall_clock())
        self.state.last_note = message

    def _new_command_id(self) -> int:
        command_id = time.monotonic_ns() & 0xFFFFFFFF
        if command_id == 0 or command_id == self._last_command_id:
            command_id = (self._last_command_id + 1) & 0xFFFFFFFF
            if command_id == 0:
                command_id = 1
        self._last_command_id = command_id
        return command_id


def _run_state_code(value: Any) -> int | None:
    if not isinstance(value, Mapping):
        return None
    direct = value.get("run_state_code")
    if isinstance(direct, int) and not isinstance(direct, bool):
        return direct
    for key in ("observation", "command", "stop_result", "save_result"):
        nested = value.get(key)
        if nested is not None:
            result = _run_state_code(nested)
            if result is not None:
                return result
    return None


def _write_acknowledged(error: Exception) -> bool:
    if not isinstance(error, ModbusClientError):
        return False
    return bool(error.details.get("write_acknowledged"))


def _connection_state_for_error(error: Exception) -> ConnectionState:
    if isinstance(error, (TransactionTimeout, TransportError)):
        return ConnectionState.DEVICE_UNRESPONSIVE
    if isinstance(error, (ProtocolError, UnsupportedProtocolError, SnapshotFormatError)):
        return ConnectionState.PROTOCOL_ERROR
    if isinstance(error, StateError):
        return ConnectionState.CONNECTED
    if isinstance(error, ModbusClientError):
        return ConnectionState.PROTOCOL_ERROR
    return ConnectionState.PROTOCOL_ERROR
