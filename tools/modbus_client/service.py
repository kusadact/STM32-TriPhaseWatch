"""Business command sequences built from single Modbus transactions."""

from __future__ import annotations

from typing import Any, Iterable

from .client import ModbusClient, TransactionResult
from .errors import ModbusClientError, ModbusException, StateError
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
    decode_identity,
    decode_snapshot,
    decode_stats,
    decode_status,
)

COMMAND_APPLY_CONFIG = 1
COMMAND_SAVE_CONFIG = 2
COMMAND_START = 3
COMMAND_STOP = 4
COMMAND_SINGLE = 5

HOLDING_CONFIG_START = 0x0000
HOLDING_COMMAND = 0x0040

INPUT_STATUS_START = 0x0000
INPUT_COMMAND_OBSERVATION_START = 0x0005
INPUT_COMMAND_OBSERVATION_COUNT = 11
INPUT_SNAPSHOT_START = 0x0020
INPUT_SNAPSHOT_COUNT = 15
INPUT_STATS_START = 0x0060
INPUT_STATS_COUNT = 26


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

    def stats(self) -> dict[str, Any]:
        return decode_stats(self.read_input(INPUT_STATS_START, INPUT_STATS_COUNT))

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

    def save(self) -> dict[str, Any]:
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

    def start(self) -> dict[str, Any]:
        self.write_single(HOLDING_COMMAND, COMMAND_START)
        observation = self._observe_after_write()
        self._require_command(observation, COMMAND_START, (1,))
        return observation

    def stop(self) -> dict[str, Any]:
        self.write_single(HOLDING_COMMAND, COMMAND_STOP)
        observation = self._observe_after_write()
        self._require_command(observation, COMMAND_STOP, (1,))
        if observation["run_state_code"] != 0:
            raise StateError(
                "STOP command was accepted but the device is still running",
                observation=observation,
                write_acknowledged=True,
            )
        return observation

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
