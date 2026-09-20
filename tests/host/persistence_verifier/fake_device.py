"""Synthetic Modbus transport for runner self-tests only."""

from __future__ import annotations

from collections import deque
from datetime import datetime, timezone
import struct
import time

from tools.modbus_client.protocol import append_crc, response_has_valid_crc


class FakeDeviceTransport:
    """A deterministic protocol-3 device double; never opens a serial port."""

    def __init__(self, *, finite_count: int = 3, protocol_version: int = 3) -> None:
        self.finite_count = finite_count
        self.protocol_version = protocol_version
        self.description = "synthetic-persistence-device"
        self.open_count = 0
        self.close_count = 0
        self.recover_count = 0
        self.writes: list[bytes] = []
        self.closed = True
        self._pending: deque[bytes] = deque()

        self.input_registers = [0] * 0x100
        self.holding_registers = [0] * 0xC0
        self.pending_config = [10, 1, 0]
        self.active_config = [10, 1, 0]
        self.active_config_version = 0
        self.run_state = 0
        self.session_id = 1
        self.sequence = 0
        self.records_this_run = 0
        self.records_since_start = 0
        self.generated = 0
        self.synced = 0
        self.dropped = 0
        self.uncertain = 0
        self.drain_state = 0
        self.last_command = 0
        self.command_result = 0
        self.last_command_id = 0
        self.time_status = 0
        self.current_utc = 0xFFFFFFFF
        self.pending_utc = 0
        self.snapshot_valid = 0
        self.snapshot_trigger = 0
        self.snapshot_channels = [0, 0, 0, 0]
        self.snapshot_qualities = [0, 0, 0, 0]
        self._sync_registers()

    def open(self) -> None:
        self.open_count += 1
        self.closed = False

    def close(self) -> None:
        self.close_count += 1
        self.closed = True

    def write_all(self, data: bytes, deadline: float) -> list[bytes]:
        if self.closed:
            raise RuntimeError("synthetic device is closed")
        if time.monotonic() >= deadline:
            raise TimeoutError("synthetic write deadline expired")
        self.writes.append(data)
        self._pending.append(self._handle_request(data))
        return [data]

    def read(self, max_bytes: int, deadline: float) -> bytes | None:
        if not self._pending:
            return None
        response = self._pending.popleft()
        if len(response) > max_bytes:
            self._pending.appendleft(response[max_bytes:])
            return response[:max_bytes]
        return response

    def read_available(self, max_bytes: int = 512) -> bytes:
        if not self._pending:
            return b""
        data = b"".join(self._pending)
        self._pending.clear()
        return data[:max_bytes]

    def recover(
        self,
        duration_seconds: float,
        quiet_seconds: float,
        deadline: float,
    ) -> bool:
        self.recover_count += 1
        return True

    def _exception(self, function: int, code: int) -> bytes:
        return append_crc(bytes((1, function | 0x80, code)))

    def _read_response(self, function: int, values: list[int]) -> bytes:
        payload = bytes((1, function, len(values) * 2))
        payload += b"".join(value.to_bytes(2, "big") for value in values)
        return append_crc(payload)

    def _handle_request(self, request: bytes) -> bytes:
        if len(request) < 8 or not response_has_valid_crc(request):
            raise RuntimeError("invalid request reached synthetic device")
        function = request[1]
        if function in (0x03, 0x04):
            start, count = struct.unpack_from(">HH", request, 2)
            if function == 0x04 and start <= 0x18 and start + count > 0x0005:
                self._tick()
                self._sync_registers()
            registers = (
                self.holding_registers if function == 0x03 else self.input_registers
            )
            if start + count > len(registers):
                return self._exception(function, 0x02)
            return self._read_response(function, registers[start : start + count])
        if function == 0x06:
            register, value = struct.unpack_from(">HH", request, 2)
            if register >= len(self.holding_registers):
                return self._exception(function, 0x02)
            self.holding_registers[register] = value
            if register == 0x0040:
                self._execute_command(value)
            self._sync_registers()
            return request
        if function == 0x10:
            start, count, byte_count = struct.unpack_from(">HHB", request, 2)
            if byte_count != count * 2 or len(request) != 9 + byte_count:
                return self._exception(function, 0x03)
            values = [
                struct.unpack_from(">H", request, 7 + (index * 2))[0]
                for index in range(count)
            ]
            if start + count > len(self.holding_registers):
                return self._exception(function, 0x02)
            self.holding_registers[start : start + count] = values
            if start == 0:
                self.pending_config = list(values[:3])
            elif start == 0x0020:
                self.pending_utc = (values[0] << 16) | values[1]
            self._sync_registers()
            return append_crc(request[:6])
        return self._exception(function, 0x01)

    def _execute_command(self, command: int) -> None:
        self.last_command = command
        if command == 1:
            self.active_config = list(self.pending_config)
            self.active_config_version = (self.active_config_version + 1) & 0xFFFFFFFF
            self.command_result = 1
        elif command == 3:
            self.run_state = 1
            self.records_this_run = 0
            self.records_since_start = 0
            self.drain_state = 0
            self.command_result = 1
        elif command == 4:
            self.run_state = 0
            self.drain_state = 2
            self.command_result = 1
        elif command == 5:
            self._generate_record(2)
            self.command_result = 1
        elif command == 6:
            if self.pending_utc < 0xFFFFFFFF:
                self.time_status = 1
                self.current_utc = self.pending_utc
                self.command_result = 1
            else:
                self.command_result = 4
        else:
            self.command_result = 3

    def _tick(self) -> None:
        if self.run_state != 1:
            return
        self._generate_record(1)
        self.records_this_run += 1
        self.records_since_start += 1
        if self.finite_count and self.records_this_run >= self.finite_count:
            self.run_state = 0
            self.drain_state = 2

    def _generate_record(self, trigger: int) -> None:
        self.sequence = (self.sequence + 1) & 0xFFFFFFFF
        self.snapshot_valid = 1
        self.snapshot_trigger = trigger
        mask = self.active_config[1]
        for index in range(4):
            if mask & (1 << index):
                self.snapshot_channels[index] = (
                    (self.sequence * 10) + index
                ) & 0xFFFF
                self.snapshot_qualities[index] = 1
            else:
                self.snapshot_channels[index] = 0
                self.snapshot_qualities[index] = 0
        self.generated = (self.generated + 1) & 0xFFFFFFFF
        self.synced = (self.synced + 1) & 0xFFFFFFFF

    def _sync_registers(self) -> None:
        registers = self.input_registers
        registers[0] = 1
        registers[1] = 1
        registers[2] = 6
        registers[3] = 0
        registers[4] = self.protocol_version
        registers[5] = self.run_state
        registers[6] = 1
        registers[7] = self.active_config[0]
        registers[8] = self.active_config[1]
        registers[9] = self.active_config[2]
        registers[10] = (self.active_config_version >> 16) & 0xFFFF
        registers[11] = self.active_config_version & 0xFFFF
        registers[12] = self.last_command
        registers[13] = self.command_result
        registers[14] = (self.last_command_id >> 16) & 0xFFFF
        registers[15] = self.last_command_id & 0xFFFF
        registers[16] = 1
        registers[17] = (self.session_id >> 16) & 0xFFFF
        registers[18] = self.session_id & 0xFFFF
        registers[19] = 1
        registers[20] = 1
        registers[21] = 1
        registers[22] = self.time_status
        registers[23] = (self.records_this_run >> 16) & 0xFFFF
        registers[24] = self.records_this_run & 0xFFFF
        registers[0x16] = self.time_status
        registers[0x19] = 0
        current = self.current_utc
        registers[0x1A] = (current >> 16) & 0xFFFF
        registers[0x1B] = current & 0xFFFF
        registers[0x1C] = 0xFFFF
        registers[0x1D] = 0xFFFF

        registers[0x20] = self.snapshot_valid
        registers[0x21] = (self.sequence >> 16) & 0xFFFF
        registers[0x22] = self.sequence & 0xFFFF
        registers[0x23] = 1
        registers[0x24] = sum(
            1 for index in range(4) if self.active_config[1] & (1 << index)
        )
        for index in range(4):
            registers[0x25 + index] = self.snapshot_channels[index]
            registers[0x29 + index] = self.snapshot_qualities[index]
        registers[0x2D] = self.snapshot_trigger
        registers[0x2E] = 1

        for index in range(26):
            registers[0x60 + index] = 0

        storage = [0] * 48

        def put32(offset: int, value: int) -> None:
            storage[offset] = (value >> 16) & 0xFFFF
            storage[offset + 1] = value & 0xFFFF

        storage[0] = 1
        storage[7] = 1
        storage[8] = 1
        storage[10] = 0
        storage[11] = min(self.records_this_run, 32)
        put32(12, self.generated)
        put32(14, self.synced)
        put32(16, self.dropped)
        put32(18, self.uncertain)
        storage[20] = 0
        storage[21] = self.drain_state
        put32(22, self.sequence)
        put32(24, 1 if self.synced else 0)
        synced_date = 0
        if self.synced and self.current_utc != 0xFFFFFFFF:
            synced_date = int(
                datetime.fromtimestamp(self.current_utc, timezone.utc).strftime(
                    "%Y%m%d"
                )
            )
        put32(26, synced_date)
        put32(28, self.active_config_version)
        put32(30, 1)
        self.input_registers[0x80:0xB0] = storage

    @property
    def command_writes(self) -> list[int]:
        commands: list[int] = []
        for request in self.writes:
            if len(request) == 8 and request[1] == 0x06:
                register, value = struct.unpack_from(">HH", request, 2)
                if register == 0x0040:
                    commands.append(value)
        return commands


class VirtualClock:
    def __init__(self, start: float = 0.0) -> None:
        self.now = start
        self.sleeps: list[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        if seconds < 0:
            raise ValueError("negative virtual sleep")
        self.sleeps.append(seconds)
        self.now += seconds
