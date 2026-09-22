"""Business command sequences built from single Modbus transactions."""

from __future__ import annotations

import time
from typing import Any, Iterable

from .client import ModbusClient, TransactionResult
from .errors import (
    ModbusClientError,
    ModbusException,
    StateError,
    UnsupportedProtocolError,
)
from .protocol import (
    FUNCTION_READ_HOLDING,
    FUNCTION_READ_INPUT,
    build_read_request,
    build_write_multiple_request,
    build_write_single_request,
)
from .registers import (
    command_result_name,
    decode_command_observation,
    decode_ds18b20_snapshot,
    decode_identity,
    decode_persistence_status,
    decode_snapshot,
    decode_stats,
    decode_status,
    decode_time_status,
)
from .timeparse import split_u32

COMMAND_APPLY_CONFIG = 1
COMMAND_SAVE_CONFIG = 2
COMMAND_START = 3
COMMAND_STOP = 4
COMMAND_SINGLE = 5
COMMAND_SET_TIME = 6
COMMAND_ARM_START = 7

HOLDING_CONFIG_START = 0x0000
HOLDING_PENDING_UTC_START = 0x0020
HOLDING_PENDING_START_UTC_START = 0x0022
HOLDING_COMMAND = 0x0040

INPUT_STATUS_START = 0x0000
INPUT_COMMAND_OBSERVATION_START = 0x0005
INPUT_COMMAND_OBSERVATION_COUNT = 11
INPUT_SNAPSHOT_START = 0x0020
INPUT_SNAPSHOT_COUNT = 15
INPUT_STATS_START = 0x0060
INPUT_STATS_COUNT = 26
INPUT_TIME_STATUS_START = 0x0016
INPUT_TIME_STATUS_COUNT = 8
INPUT_PERSISTENCE_STATUS_START = 0x0080
INPUT_PERSISTENCE_STATUS_COUNT = 48
INPUT_DS18B20_START = 0x00B0
INPUT_DS18B20_COUNT = 24
PROTOCOL_PERSISTENCE_VERSION = 3
DS18B20_CONTRACT_REVISION = 2
DS18B20_SENSOR_TYPE = 2
DS18B20_SOURCE_NONE = 0
DS18B20_SOURCE_REAL = 3
DS18B20_QUALITY_NOT_PRESENT = 6
DS18B20_QUALITIES_WITH_VALUE = ("OK", "STALE")


class ModbusService:
    def __init__(self, client: ModbusClient, address: int = 1) -> None:
        self.client = client
        self.address = address

    def read_registers(
        self,
        function: int,
        start: int,
        count: int,
    ) -> tuple[int, ...]:
        request = build_read_request(self.address, function, start, count)
        return self.client.transaction(request).response.values

    def read_holding(self, start: int, count: int) -> tuple[int, ...]:
        return self.read_registers(FUNCTION_READ_HOLDING, start, count)

    def read_input(self, start: int, count: int) -> tuple[int, ...]:
        return self.read_registers(FUNCTION_READ_INPUT, start, count)

    def write_single(self, register: int, value: int) -> TransactionResult:
        return self.client.transaction(
            build_write_single_request(self.address, register, value)
        )

    def write_multiple(
        self,
        start: int,
        values: Iterable[int],
    ) -> TransactionResult:
        value_list = list(values)
        return self.client.transaction(
            build_write_multiple_request(self.address, start, value_list)
        )

    def identity(self) -> dict[str, Any]:
        return decode_identity(self.read_input(0x0000, 5))

    def status(self) -> dict[str, Any]:
        return decode_status(self.read_input(INPUT_STATUS_START, 25))

    def snapshot(self) -> dict[str, Any]:
        return decode_snapshot(
            self.read_input(INPUT_SNAPSHOT_START, INPUT_SNAPSHOT_COUNT)
        )

    def read_temperature_snapshot(self) -> dict[str, Any]:
        try:
            values = self.read_input(INPUT_DS18B20_START, INPUT_DS18B20_COUNT)
        except ModbusException as exc:
            if exc.exception_code != 0x02:
                raise
            raise UnsupportedProtocolError(
                "设备不支持 DS18B20 温度扩展块，接口尚未冻结",
                interface_unavailable=True,
                start=INPUT_DS18B20_START,
                count=INPUT_DS18B20_COUNT,
                exception_code=exc.exception_code,
            ) from exc

        decoded = decode_ds18b20_snapshot(values)
        source_code = decoded["source_type"]
        if (
            decoded["contract_revision"] != DS18B20_CONTRACT_REVISION
            or decoded["sensor_type_code"] != DS18B20_SENSOR_TYPE
            or source_code not in (DS18B20_SOURCE_NONE, DS18B20_SOURCE_REAL)
        ):
            raise UnsupportedProtocolError(
                "DS18B20 温度扩展块版本、类型或数据源不受支持",
                interface_unavailable=True,
                contract_revision=decoded["contract_revision"],
                source_type=source_code,
                sensor_type_code=decoded["sensor_type_code"],
            )

        no_sample = (
            source_code == DS18B20_SOURCE_NONE and decoded["valid_mask"] == 0
        )
        sensors = []
        for sensor in decoded["sensors"]:
            quality_code = (
                DS18B20_QUALITY_NOT_PRESENT
                if no_sample
                else sensor["quality_code"]
            )
            if no_sample:
                quality = "NOT_PRESENT"
                temperature_x16 = None
            else:
                quality = sensor["quality"]
                temperature_x16 = sensor["temperature_x16"]
                if quality not in DS18B20_QUALITIES_WITH_VALUE:
                    temperature_x16 = None
            sensors.append(
                {
                    "sensor_id": sensor["sensor_id"],
                    "sensor_type": sensor["sensor_type"],
                    "valid": sensor["valid"],
                    "temperature_x16": temperature_x16,
                    "temperature_unit": sensor["temperature_unit"],
                    "rom_short": sensor["rom_short"],
                    "quality_code": quality_code,
                    "quality": quality,
                    "error_code": sensor["error_code"],
                    "error": sensor["error"],
                    "sample_time": sensor["sample_time_ms"],
                    "source": decoded["source"],
                }
            )
        return {
            "contract_revision": decoded["contract_revision"],
            "sample_id": decoded["sample_id"],
            "source_type": source_code,
            "source": decoded["source"],
            "valid_mask": decoded["valid_mask"],
            "sensors": sensors,
            "sensor_type_code": decoded["sensor_type_code"],
        }

    def stats(self) -> dict[str, Any]:
        return decode_stats(self.read_input(INPUT_STATS_START, INPUT_STATS_COUNT))

    def _require_time_protocol(self) -> None:
        protocol_version = self.identity()["protocol_version"]
        if protocol_version not in (2, 3):
            raise UnsupportedProtocolError(
                "不支持协议 2/3 时间功能 "
                f"(device reports protocol version {protocol_version})",
                protocol_version=protocol_version,
            )

    def _read_time_status_v2(self) -> dict[str, Any]:
        return decode_time_status(
            self.read_input(INPUT_TIME_STATUS_START, INPUT_TIME_STATUS_COUNT)
        )

    def time_status(self) -> dict[str, Any]:
        self._require_time_protocol()
        return self._read_time_status_v2()

    def time_set(self, utc_seconds: int) -> dict[str, Any]:
        self._require_time_protocol()
        words = split_u32(utc_seconds)
        self.write_multiple(HOLDING_PENDING_UTC_START, words)
        return self._execute_time_command(
            COMMAND_SET_TIME,
            utc_seconds,
            words,
        )

    def schedule(self, utc_seconds: int) -> dict[str, Any]:
        self._require_time_protocol()
        words = split_u32(utc_seconds)
        self.write_multiple(HOLDING_PENDING_START_UTC_START, words)
        return self._execute_time_command(
            COMMAND_ARM_START,
            utc_seconds,
            words,
        )

    def _execute_time_command(
        self,
        command: int,
        requested_utc_seconds: int,
        words: tuple[int, int],
    ) -> dict[str, Any]:
        try:
            self.write_single(HOLDING_COMMAND, command)
        except ModbusException as exc:
            self._attach_time_observation(exc)
            raise

        observation = self._observe_after_write()
        try:
            time_status = self._read_time_status_v2()
        except ModbusClientError as exc:
            exc.details["write_acknowledged"] = True
            exc.details["observation"] = observation
            raise
        try:
            self._require_command(observation, command, (1,))
        except StateError as exc:
            exc.details["time_status"] = time_status
            raise
        return {
            "requested_utc_seconds": requested_utc_seconds,
            "pending_words": list(words),
            "command": observation,
            "time_status": time_status,
        }

    def _attach_time_observation(self, exc: ModbusException) -> None:
        try:
            exc.observation = self._observe_command()
        except ModbusClientError:
            exc.observation = None
        try:
            exc.details["time_status"] = self._read_time_status_v2()
        except ModbusClientError:
            pass

    def config(
        self,
        period_sec: int,
        channel_mask: int,
        record_count: int,
    ) -> dict[str, Any]:
        staged = [period_sec, channel_mask, record_count]
        self.write_multiple(HOLDING_CONFIG_START, staged)
        try:
            readback = list(self.read_holding(HOLDING_CONFIG_START, 3))
        except ModbusClientError as exc:
            exc.details["write_acknowledged"] = True
            raise
        if readback != staged:
            error = StateError(
                "configuration readback mismatch",
                expected_values=staged,
                actual_values=readback,
            )
            error.details["write_acknowledged"] = True
            raise error
        return {
            "period_sec": readback[0],
            "channel_mask": readback[1],
            "record_count": readback[2],
            "staged": True,
            "readback_matched": True,
        }

    def _observe_command(self) -> dict[str, Any]:
        return decode_command_observation(
            self.read_input(
                INPUT_COMMAND_OBSERVATION_START,
                INPUT_COMMAND_OBSERVATION_COUNT,
            )
        )

    def apply(self) -> dict[str, Any]:
        self.write_single(HOLDING_COMMAND, COMMAND_APPLY_CONFIG)
        observation = self._observe_after_write()
        self._require_command(observation, COMMAND_APPLY_CONFIG, (1,))
        return observation

    def persistence_status(self) -> dict[str, Any]:
        return decode_persistence_status(
            self.read_input(
                INPUT_PERSISTENCE_STATUS_START,
                INPUT_PERSISTENCE_STATUS_COUNT,
            )
        )

    def storage_status(self) -> dict[str, Any]:
        return self.persistence_status()

    def _legacy_save(self) -> dict[str, Any]:
        try:
            self.write_single(HOLDING_COMMAND, COMMAND_SAVE_CONFIG)
        except ModbusException as exc:
            if exc.exception_code != 0x04:
                raise
            try:
                exc.observation = self._observe_command()
            except ModbusClientError:
                exc.observation = None
            raise
        raise StateError("SAVE_CONFIG unexpectedly returned a normal write response")

    def save(
        self,
        wait_timeout: float = 5.0,
        command_id: int | None = None,
    ) -> dict[str, Any]:
        protocol_version = self.identity()["protocol_version"]
        if protocol_version not in (1, 2, 3):
            raise UnsupportedProtocolError(
                "不支持的设备协议版本 "
                f"(device reports protocol version {protocol_version})",
                protocol_version=protocol_version,
            )
        if protocol_version != PROTOCOL_PERSISTENCE_VERSION:
            return self._legacy_save()

        if command_id is None:
            command_id = time.monotonic_ns() & 0xFFFFFFFF
            if command_id == 0:
                command_id = 1
        if not 0 <= command_id <= 0xFFFFFFFF:
            raise ValueError("command_id must be in 0..0xFFFFFFFF")

        deadline = time.monotonic() + wait_timeout
        write_error: ModbusClientError | None = None
        try:
            self.write_multiple(
                HOLDING_COMMAND,
                [
                    COMMAND_SAVE_CONFIG,
                    (command_id >> 16) & 0xFFFF,
                    command_id & 0xFFFF,
                ],
            )
        except ModbusException as exc:
            try:
                exc.observation = self._observe_command()
            except ModbusClientError:
                exc.observation = None
            try:
                exc.details["persistence_status"] = self.persistence_status()
            except ModbusClientError:
                pass
            raise
        except ModbusClientError as exc:
            write_error = exc
            exc.details["write_may_have_executed"] = True

        last_status: dict[str, Any] | None = None
        last_read_error: ModbusClientError | None = None
        while True:
            try:
                last_status = self.persistence_status()
                last_read_error = None
            except ModbusClientError as exc:
                last_read_error = exc

            if (
                last_status is not None
                and last_status["save_command_id"] == command_id
                and last_status["save_state"] != 0
            ):
                result = {
                    "accepted": True,
                    "completed": last_status["save_state"] in (2, 3),
                    "inconclusive": False,
                    "command_id": command_id,
                    "captured_config": {
                        "period_sec": last_status["captured_period_s"],
                        "channel_mask": last_status["captured_mask"],
                        "record_count": last_status["captured_count"],
                    },
                    "save_state": last_status["save_state"],
                    "save_state_name": last_status["save_state_name"],
                    "save_error": last_status["save_error"],
                    "save_error_name": last_status["save_error_name"],
                    "persistence_status": last_status,
                }
                if last_status["save_state"] == 2:
                    return result
                if last_status["save_state"] == 3:
                    error = StateError(
                        "SAVE_CONFIG completed with a persistence failure",
                        save_result=result,
                        write_acknowledged=True,
                    )
                    raise error

            if time.monotonic() >= deadline:
                raise StateError(
                    "SAVE_CONFIG result was not confirmed before the deadline",
                    save_result={
                        "accepted": bool(
                            last_status is not None
                            and last_status["save_command_id"] == command_id
                        ),
                        "completed": False,
                        "inconclusive": True,
                        "command_id": command_id,
                        "save_state": (
                            last_status["save_state"]
                            if last_status is not None
                            else None
                        ),
                    },
                    command_id=command_id,
                    write_error=(
                        write_error.as_error()
                        if write_error is not None
                        else None
                    ),
                    last_status=last_status,
                    last_read_error=(
                        last_read_error.as_error()
                        if last_read_error is not None
                        else None
                    ),
                )

            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))

    def start(self) -> dict[str, Any]:
        self.write_single(HOLDING_COMMAND, COMMAND_START)
        observation = self._observe_after_write()
        self._require_command(observation, COMMAND_START, (1,))
        return observation

    def stop(
        self,
        wait_timeout: float = 5.0,
        wait_for_drain: bool = True,
    ) -> dict[str, Any]:
        protocol_version = self.identity()["protocol_version"]
        if protocol_version not in (1, 2, 3):
            raise UnsupportedProtocolError(
                "不支持的设备协议版本 "
                f"(device reports protocol version {protocol_version})",
                protocol_version=protocol_version,
            )
        before_generation = 0
        if protocol_version == PROTOCOL_PERSISTENCE_VERSION:
            before_generation = self.persistence_status()["drain_generation"]

        self.write_single(HOLDING_COMMAND, COMMAND_STOP)
        observation = self._observe_after_write()
        self._require_command(observation, COMMAND_STOP, (1,))
        if observation["run_state_code"] != 0:
            raise StateError(
                "STOP command was accepted but the device is still running",
                observation=observation,
                write_acknowledged=True,
            )
        if protocol_version != PROTOCOL_PERSISTENCE_VERSION:
            return observation
        if not wait_for_drain:
            return {
                "command": observation,
                "drain_waited": False,
                "safe_to_remove": False,
                "safe_to_remove_condition": (
                    "storage drain has not completed"
                ),
                "persistence_status": None,
            }

        deadline = time.monotonic() + wait_timeout
        last_status: dict[str, Any] | None = None
        while True:
            try:
                last_status = self.persistence_status()
            except ModbusClientError as exc:
                if time.monotonic() >= deadline:
                    raise StateError(
                        "STOP drain result could not be confirmed",
                        stop_result={
                            "drain_waited": True,
                            "safe_to_remove": False,
                            "inconclusive": True,
                            "last_read_error": exc.as_error(),
                        },
                        write_acknowledged=True,
                    ) from exc
                time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
                continue

            if last_status["drain_generation"] > before_generation:
                if last_status["drain_state"] == 2:
                    return {
                        "command": observation,
                        "drain_waited": True,
                        "safe_to_remove": True,
                        "safe_to_remove_condition": (
                            "until another START, SINGLE, or ARM_START "
                            "is accepted"
                        ),
                        "persistence_status": last_status,
                    }
                if last_status["drain_state"] == 3:
                    raise StateError(
                        "STOP stopped acquisition but storage drain failed",
                        stop_result={
                            "drain_waited": True,
                            "safe_to_remove": False,
                            "inconclusive": False,
                            "persistence_status": last_status,
                        },
                        write_acknowledged=True,
                    )

            if time.monotonic() >= deadline:
                raise StateError(
                    "STOP drain did not complete before the CLI deadline",
                    stop_result={
                        "drain_waited": True,
                        "safe_to_remove": False,
                        "inconclusive": True,
                        "persistence_status": last_status,
                    },
                    write_acknowledged=True,
                )
            time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))

    def single(self, command_id: int) -> dict[str, Any]:
        self.write_multiple(
            HOLDING_COMMAND,
            [
                COMMAND_SINGLE,
                (command_id >> 16) & 0xFFFF,
                command_id & 0xFFFF,
            ],
        )
        observation = self._observe_after_write()
        self._require_command(observation, COMMAND_SINGLE, (1, 2), command_id)
        try:
            snapshot = self.snapshot()
        except ModbusClientError as exc:
            exc.details["write_acknowledged"] = True
            raise
        result = observation["last_command"]["result"]
        return {
            "duplicate": result == "DUPLICATE",
            "command": observation,
            "snapshot": snapshot,
        }

    def _observe_after_write(self) -> dict[str, Any]:
        try:
            return self._observe_command()
        except ModbusClientError as exc:
            exc.details["write_acknowledged"] = True
            raise

    @staticmethod
    def _require_command(
        observation: dict[str, Any],
        expected_command: int,
        accepted_results: tuple[int, ...],
        expected_id: int | None = None,
    ) -> None:
        command = observation["last_command"]
        if command["code"] != expected_command:
            raise StateError(
                "command status reported a different command",
                expected_command=expected_command,
                actual_command=command["code"],
                observation=observation,
                write_acknowledged=True,
            )
        if command["result_code"] not in accepted_results:
            raise StateError(
                "command status was not accepted",
                expected_results=list(accepted_results),
                actual_result=command["result_code"],
                actual_result_name=command_result_name(command["result_code"]),
                observation=observation,
                write_acknowledged=True,
            )
        if expected_id is not None and command["id"] != expected_id:
            raise StateError(
                "command status reported a different command ID",
                expected_id=expected_id,
                actual_id=command["id"],
                observation=observation,
                write_acknowledged=True,
            )
