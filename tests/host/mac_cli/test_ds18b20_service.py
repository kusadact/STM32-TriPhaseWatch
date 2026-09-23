from __future__ import annotations

import struct
import unittest

from tools.modbus_client import registers
from tools.modbus_client.client import ModbusClient
from tools.modbus_client.errors import (
    ModbusException,
    ProtocolError,
    UnsupportedProtocolError,
)
from tools.modbus_client.protocol import FUNCTION_READ_INPUT, append_crc
from tools.modbus_client.service import (
    COMMAND_ACK_ALARM,
    HOLDING_ALARM_CONFIG_COUNT,
    HOLDING_ALARM_CONFIG_START,
    INPUT_DS18B20_COUNT,
    INPUT_DS18B20_START,
    INPUT_THERMAL_ALARM_COUNT,
    INPUT_THERMAL_ALARM_START,
    INPUT_THERMAL_STATE_COUNT,
    INPUT_THERMAL_STATE_START,
    ModbusService,
)
from fake_transport import ScriptedTransport


def read_response(request: bytes, values: tuple[int, ...]) -> bytes:
    payload = bytes((request[0], request[1], len(values) * 2))
    payload += b"".join(value.to_bytes(2, "big") for value in values)
    return append_crc(payload)


def exception_response(request: bytes, code: int) -> bytes:
    return append_crc(bytes((request[0], request[1] | 0x80, code)))


def ds18b20_values(
    *,
    revision: int = 2,
    source: int = 3,
    valid_mask: int = 0x0007,
    sample_id: int = 1,
    temperatures: tuple[int, int, int] = (200, 210, 220),
    qualities: tuple[int, int, int] = (1, 1, 1),
    errors: tuple[int, int, int] = (0, 0, 0),
    sample_times: tuple[int, int, int] = (1000, 2000, 3000),
    sensor_type: int = 2,
    rom_shorts: tuple[int, int, int] = (0x1200, 0x1201, 0x1202),
) -> tuple[int, ...]:
    """Build the 24-register DS18B20 block published at input 0x00B0."""

    values = [0] * 24
    values[0] = revision
    values[1] = source
    values[2] = valid_mask
    values[3] = (sample_id >> 16) & 0xFFFF
    values[4] = sample_id & 0xFFFF
    values[5:8] = [value & 0xFFFF for value in temperatures]
    values[8:11] = qualities
    values[11:14] = errors
    for index, sample_time in enumerate(sample_times):
        offset = 14 + (index * 2)
        values[offset] = (sample_time >> 16) & 0xFFFF
        values[offset + 1] = sample_time & 0xFFFF
    values[20] = sensor_type
    values[21:24] = rom_shorts
    return tuple(values)


def thermal_alarm_values(
    *,
    revision: int = 1,
    level: int = 1,
    reason: int = 2,
    flags: int = 0x0B,
    trigger_phase: int = 1,
    delta_valid: int = 1,
    maximum_delta_x16: int = 160,
    hottest_temperature_x16: int = 480,
    hottest_phase: int = 1,
    temperatures: tuple[int, int, int] = (480, 320, 320),
    qualities: tuple[int, int, int] = (1, 1, 1),
    event_id: int = 7,
    alarm_sample_id: int = 1,
    duration_sec: int = 12,
    notice_count: int = 1,
    warning_count: int = 0,
    critical_count: int = 0,
    sensor_fault_count: int = 0,
) -> tuple[int, ...]:
    values = [0] * INPUT_THERMAL_ALARM_COUNT
    values[0] = revision
    values[1] = level
    values[2] = reason
    values[3] = flags
    values[4] = trigger_phase
    values[5] = delta_valid
    values[6] = maximum_delta_x16 & 0xFFFF
    values[7] = hottest_temperature_x16 & 0xFFFF
    values[8] = hottest_phase
    values[9:12] = [value & 0xFFFF for value in temperatures]
    values[12:15] = qualities
    values[15] = (event_id >> 16) & 0xFFFF
    values[16] = event_id & 0xFFFF
    values[17] = (alarm_sample_id >> 16) & 0xFFFF
    values[18] = alarm_sample_id & 0xFFFF
    values[19] = (duration_sec >> 16) & 0xFFFF
    values[20] = duration_sec & 0xFFFF
    values[21] = (notice_count >> 16) & 0xFFFF
    values[22] = notice_count & 0xFFFF
    values[23] = (warning_count >> 16) & 0xFFFF
    values[24] = warning_count & 0xFFFF
    values[25] = (critical_count >> 16) & 0xFFFF
    values[26] = critical_count & 0xFFFF
    values[27] = (sensor_fault_count >> 16) & 0xFFFF
    values[28] = sensor_fault_count & 0xFFFF
    return tuple(values)


def write_response(request: bytes) -> bytes:
    return append_crc(request[:6])


def observation_values(
    *,
    command: int,
    result: int,
    command_id: int,
) -> tuple[int, ...]:
    return (
        0,
        1,
        10,
        1,
        0,
        0,
        0,
        command,
        result,
        (command_id >> 16) & 0xFFFF,
        command_id & 0xFFFF,
    )


def alarm_config_values() -> tuple[int, ...]:
    return (
        1,
        50 * 16,
        55 * 16,
        75 * 16,
        5 * 16,
        10 * 16,
        15 * 16,
        5 * 16,
        10 * 16,
        20 * 16,
        3,
        5,
        2 * 16,
        1,
        0,
        0,
    )


class Ds18b20ServiceTests(unittest.TestCase):
    def _service(self, handler):
        transport = ScriptedTransport(handler)
        client = ModbusClient(
            transport,
            timeout_seconds=0.05,
            recovery_seconds=0.001,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.01,
        )
        client.open()
        return ModbusService(client), client, transport

    def test_maps_three_sensors_and_32_bit_fields(self) -> None:
        values = ds18b20_values(
            sample_id=0x12345678,
            # a CRC_ERROR point keeps no valid bit
            valid_mask=0x0003,
            temperatures=(230, -160, 480),
            qualities=(1, 5, 3),
            errors=(0, 6, 3),
            sample_times=(0x00010002, 0x00030004, 0x89ABCDEF),
            rom_shorts=(0x1200, 0x1201, 0x1202),
        )
        service, client, transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            snapshot = service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertEqual(snapshot["contract_revision"], 2)
        self.assertEqual(snapshot["sensor_type_code"], 2)
        self.assertEqual(snapshot["sample_id"], 0x12345678)
        self.assertEqual(snapshot["source"], "REAL_DS18B20")
        self.assertEqual(snapshot["valid_mask"], 0x0003)
        self.assertEqual(len(snapshot["sensors"]), 3)
        self.assertEqual(snapshot["sensors"][0]["temperature_x16"], 230)
        self.assertEqual(snapshot["sensors"][1]["temperature_x16"], -160)
        self.assertEqual(snapshot["sensors"][1]["quality"], "STALE")
        self.assertEqual(snapshot["sensors"][2]["quality"], "CRC_ERROR")
        self.assertIsNone(snapshot["sensors"][2]["temperature_x16"])
        self.assertEqual(
            [sensor["rom_short"] for sensor in snapshot["sensors"]],
            [0x1200, 0x1201, 0x1202],
        )
        self.assertEqual(
            [sensor["sample_time"] for sensor in snapshot["sensors"]],
            [0x00010002, 0x00030004, 0x89ABCDEF],
        )
        self.assertTrue(
            all(
                sensor["source"] == "REAL_DS18B20"
                for sensor in snapshot["sensors"]
            )
        )
        self.assertTrue(
            all("humidity_x10" not in sensor for sensor in snapshot["sensors"])
        )

        request = transport.writes[0]
        self.assertEqual(request[1], FUNCTION_READ_INPUT)
        self.assertEqual(
            struct.unpack_from(">HH", request, 2),
            (INPUT_DS18B20_START, INPUT_DS18B20_COUNT),
        )

    def test_negative_and_zero_temperatures_are_sign_extended(self) -> None:
        values = ds18b20_values(
            temperatures=(-160, 0, 160),
            qualities=(1, 1, 1),
        )
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            snapshot = service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertEqual(
            [sensor["temperature_x16"] for sensor in snapshot["sensors"]],
            [-160, 0, 160],
        )
        self.assertTrue(all(sensor["valid"] for sensor in snapshot["sensors"]))

    def test_failed_qualities_do_not_publish_a_temperature(self) -> None:
        values = ds18b20_values(
            valid_mask=0x0000,
            temperatures=(0, 0, 0),
            qualities=(2, 4, 3),
            errors=(1, 7, 6),
        )
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            snapshot = service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertEqual(
            [sensor["quality"] for sensor in snapshot["sensors"]],
            ["TIMEOUT", "RANGE_ERROR", "CRC_ERROR"],
        )
        self.assertEqual(
            [sensor["error"] for sensor in snapshot["sensors"]],
            ["RESET_TIMEOUT", "RANGE", "SCRATCHPAD_CRC"],
        )
        self.assertTrue(
            all(sensor["temperature_x16"] is None for sensor in snapshot["sensors"])
        )
        self.assertTrue(
            all(not sensor["valid"] for sensor in snapshot["sensors"])
        )

    def test_not_present_sensor_has_no_fake_values(self) -> None:
        values = ds18b20_values(
            valid_mask=0x0003,
            temperatures=(200, 210, 0),
            qualities=(1, 1, 6),
            errors=(0, 0, 1),
            rom_shorts=(0x1200, 0x1201, 0),
        )
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            snapshot = service.read_temperature_snapshot()
        finally:
            client.close()

        sensor = snapshot["sensors"][2]
        self.assertEqual(sensor["quality"], "NOT_PRESENT")
        self.assertIsNone(sensor["temperature_x16"])
        self.assertEqual(sensor["rom_short"], 0)
        self.assertFalse(sensor["valid"])

    def test_source_zero_before_first_sample_is_not_present(self) -> None:
        values = ds18b20_values(
            source=0,
            valid_mask=0,
            sample_id=0,
            temperatures=(230, 240, 250),
            qualities=(0, 0, 0),
            errors=(0, 0, 0),
            sample_times=(100, 200, 300),
            rom_shorts=(0, 0, 0),
        )
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            snapshot = service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertEqual(snapshot["source"], "NONE")
        self.assertEqual(snapshot["sample_id"], 0)
        self.assertEqual(
            [sensor["quality"] for sensor in snapshot["sensors"]],
            ["NOT_PRESENT", "NOT_PRESENT", "NOT_PRESENT"],
        )
        self.assertTrue(
            all(sensor["temperature_x16"] is None for sensor in snapshot["sensors"])
        )
        self.assertTrue(
            all(sensor["source"] == "NONE" for sensor in snapshot["sensors"])
        )
        self.assertEqual(
            [sensor["sample_time"] for sensor in snapshot["sensors"]],
            [100, 200, 300],
        )

    def test_illegal_address_maps_to_interface_unavailable(self) -> None:
        service, client, transport = self._service(
            lambda request: exception_response(request, 0x02)
        )
        try:
            with self.assertRaises(UnsupportedProtocolError) as raised:
                service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertTrue(raised.exception.details["interface_unavailable"])
        self.assertEqual(raised.exception.details["start"], INPUT_DS18B20_START)
        self.assertEqual(raised.exception.details["count"], INPUT_DS18B20_COUNT)
        self.assertEqual(raised.exception.details["exception_code"], 0x02)
        self.assertEqual(len(transport.writes), 1)

    def test_other_modbus_exception_is_not_mislabeled(self) -> None:
        service, client, _transport = self._service(
            lambda request: exception_response(request, 0x03)
        )
        try:
            with self.assertRaises(ModbusException) as raised:
                service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertEqual(raised.exception.exception_code, 0x03)

    def test_legacy_dht11_block_is_interface_unavailable(self) -> None:
        values = ds18b20_values(revision=1, source=2, sensor_type=1)
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            with self.assertRaises(UnsupportedProtocolError) as raised:
                service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertTrue(raised.exception.details["interface_unavailable"])
        self.assertEqual(raised.exception.details["contract_revision"], 1)
        self.assertEqual(raised.exception.details["sensor_type_code"], 1)
        self.assertEqual(raised.exception.details["source_type"], 2)

    def test_test_source_is_interface_unavailable(self) -> None:
        values = ds18b20_values(source=1)
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            with self.assertRaises(UnsupportedProtocolError) as raised:
                service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertTrue(raised.exception.details["interface_unavailable"])
        self.assertEqual(raised.exception.details["source_type"], 1)

    def test_legacy_snapshot_decoder_still_names_real_dht11_source(self) -> None:
        decoded = registers.decode_snapshot(
            (1, 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        )
        self.assertEqual(decoded["source"], "REAL_DHT11")

    def test_source_none_with_valid_mask_is_rejected(self) -> None:
        values = ds18b20_values(source=0, valid_mask=0x0007)
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            with self.assertRaises(ProtocolError) as raised:
                service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertEqual(raised.exception.details["source_type"], 0)
        self.assertEqual(raised.exception.details["valid_mask"], 0x0007)

    def test_valid_mask_set_without_value_quality_is_rejected(self) -> None:
        values = ds18b20_values(valid_mask=0x0001, qualities=(6, 6, 6))
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            with self.assertRaises(ProtocolError) as raised:
                service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertEqual(raised.exception.details["sensor_id"], 0)
        self.assertEqual(raised.exception.details["quality"], "NOT_PRESENT")

    def test_ok_quality_without_valid_mask_bit_is_rejected(self) -> None:
        values = ds18b20_values(valid_mask=0x0006, qualities=(1, 1, 1))
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            with self.assertRaises(ProtocolError) as raised:
                service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertEqual(raised.exception.details["sensor_id"], 0)
        self.assertEqual(raised.exception.details["valid_mask"], 0x0006)

    def test_thermal_state_reads_temperature_and_alarm_in_one_transaction(
        self,
    ) -> None:
        values = ds18b20_values(
            sample_id=0x12345678,
            temperatures=(480, 320, 320),
            qualities=(1, 1, 1),
        ) + thermal_alarm_values(
            alarm_sample_id=0x12345678,
            event_id=9,
        )
        service, client, transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            state = service.read_thermal_state()
        finally:
            client.close()

        self.assertEqual(len(transport.writes), 1)
        self.assertEqual(
            struct.unpack_from(">HH", transport.writes[0], 2),
            (INPUT_THERMAL_STATE_START, INPUT_THERMAL_STATE_COUNT),
        )
        self.assertEqual(state["temperature_snapshot"]["sample_id"], 0x12345678)
        self.assertEqual(state["thermal_alarm"]["level"], "NOTICE")
        self.assertEqual(state["alarm"]["event_id"], 9)
        self.assertEqual(
            state["thermal_alarm"]["phases"][0]["temperature_x16"],
            480,
        )

    def test_thermal_state_rejects_cross_block_sample_id_mismatch(self) -> None:
        values = ds18b20_values(sample_id=10) + thermal_alarm_values(
            alarm_sample_id=11
        )
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            with self.assertRaises(ProtocolError) as raised:
                service.read_thermal_state()
        finally:
            client.close()

        self.assertEqual(
            raised.exception.details["temperature_sample_id"],
            10,
        )
        self.assertEqual(raised.exception.details["alarm_sample_id"], 11)

    def test_standalone_alarm_read_and_ack_preserve_level(self) -> None:
        alarm = thermal_alarm_values(
            level=2,
            reason=2,
            flags=0x0B,
            trigger_phase=1,
            maximum_delta_x16=160,
            hottest_temperature_x16=480,
            temperatures=(480, 320, 320),
            event_id=3,
            alarm_sample_id=99,
        )
        command_id = 0x12345678
        acknowledged = False

        def handler(request: bytes) -> bytes:
            nonlocal acknowledged
            if request[1] != FUNCTION_READ_INPUT:
                acknowledged = True
                return write_response(request)
            start = struct.unpack_from(">H", request, 2)[0]
            if start == 0x0005:
                return read_response(
                    request,
                    observation_values(
                        command=COMMAND_ACK_ALARM,
                        result=1,
                        command_id=command_id,
                    ),
                )
            if start == INPUT_THERMAL_ALARM_START:
                current = list(alarm)
                if acknowledged:
                    current[3] |= 0x04
                return read_response(request, tuple(current))
            raise AssertionError(f"unexpected read address 0x{start:04X}")

        service, client, transport = self._service(handler)
        try:
            read_alarm = service.read_thermal_alarm()
            result = service.ack_alarm(command_id)
        finally:
            client.close()

        self.assertEqual(read_alarm["level"], "WARNING")
        self.assertEqual(result["command"]["last_command"]["name"], "ACK_ALARM")
        self.assertEqual(result["thermal_alarm"]["level"], "WARNING")
        self.assertTrue(result["thermal_alarm"]["acknowledged"])
        self.assertFalse(result["duplicate"])
        self.assertEqual(len(transport.writes), 4)
        self.assertEqual(
            struct.unpack_from(">HHH", transport.writes[1], 7),
            (COMMAND_ACK_ALARM, 0x1234, 0x5678),
        )

    def test_read_alarm_config(self) -> None:
        service, client, transport = self._service(
            lambda request: read_response(request, alarm_config_values())
        )
        try:
            config = service.read_alarm_config()
        finally:
            client.close()

        self.assertEqual(config["phase_critical_x16"], 75 * 16)
        self.assertEqual(config["assert_samples"], 3)
        self.assertTrue(config["buzzer_enable"])
        self.assertEqual(
            struct.unpack_from(">HH", transport.writes[0], 2),
            (HOLDING_ALARM_CONFIG_START, HOLDING_ALARM_CONFIG_COUNT),
        )

    def test_thermal_alarm_illegal_address_is_interface_unavailable(self) -> None:
        service, client, transport = self._service(
            lambda request: exception_response(request, 0x02)
        )
        try:
            with self.assertRaises(UnsupportedProtocolError) as raised:
                service.read_thermal_alarm()
        finally:
            client.close()

        self.assertTrue(raised.exception.details["interface_unavailable"])
        self.assertEqual(
            raised.exception.details["start"],
            INPUT_THERMAL_ALARM_START,
        )
        self.assertEqual(
            raised.exception.details["count"],
            INPUT_THERMAL_ALARM_COUNT,
        )
        self.assertEqual(len(transport.writes), 1)


if __name__ == "__main__":
    unittest.main()
