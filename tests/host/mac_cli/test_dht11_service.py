from __future__ import annotations

import struct
import unittest

from tools.modbus_client import registers
from tools.modbus_client.client import ModbusClient
from tools.modbus_client.errors import ModbusException, UnsupportedProtocolError
from tools.modbus_client.protocol import FUNCTION_READ_INPUT, append_crc
from tools.modbus_client.service import (
    INPUT_DHT11_COUNT,
    INPUT_DHT11_START,
    ModbusService,
)
from fake_transport import ScriptedTransport


def read_response(request: bytes, values: tuple[int, ...]) -> bytes:
    payload = bytes((request[0], request[1], len(values) * 2))
    payload += b"".join(value.to_bytes(2, "big") for value in values)
    return append_crc(payload)


def exception_response(request: bytes, code: int) -> bytes:
    return append_crc(bytes((request[0], request[1] | 0x80, code)))


def dht11_values(
    *,
    revision: int = 1,
    source: int = 2,
    valid_mask: int = 0x0007,
    sample_id: int = 1,
    temperatures: tuple[int, int, int] = (200, 210, 220),
    humidities: tuple[int, int, int] = (500, 510, 520),
    qualities: tuple[int, int, int] = (1, 1, 1),
    errors: tuple[int, int, int] = (0, 0, 0),
    sample_times: tuple[int, int, int] = (1000, 2000, 3000),
    sensor_type: int = 1,
) -> tuple[int, ...]:
    values = [0] * 24
    values[0] = revision
    values[1] = source
    values[2] = valid_mask
    values[3] = (sample_id >> 16) & 0xFFFF
    values[4] = sample_id & 0xFFFF
    values[5:8] = temperatures
    values[8:11] = humidities
    values[11:14] = qualities
    values[14:17] = errors
    for index, sample_time in enumerate(sample_times):
        offset = 17 + (index * 2)
        values[offset] = (sample_time >> 16) & 0xFFFF
        values[offset + 1] = sample_time & 0xFFFF
    values[23] = sensor_type
    return tuple(values)


class Dht11ServiceTests(unittest.TestCase):
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
        values = dht11_values(
            sample_id=0x12345678,
            temperatures=(230, 240, 250),
            humidities=(450, 550, 650),
            qualities=(1, 3, 5),
            errors=(0, 3, 1),
            sample_times=(0x00010002, 0x00030004, 0x89ABCDEF),
        )
        service, client, transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            snapshot = service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertEqual(snapshot["sample_id"], 0x12345678)
        self.assertEqual(snapshot["source"], "REAL_DHT11")
        self.assertEqual(len(snapshot["sensors"]), 3)
        self.assertEqual(snapshot["sensors"][0]["temperature_x10"], 230)
        self.assertEqual(snapshot["sensors"][2]["humidity_x10"], 650)
        self.assertEqual(snapshot["sensors"][1]["quality"], "CHECKSUM_ERROR")
        self.assertEqual(snapshot["sensors"][2]["quality"], "STALE")
        self.assertEqual(
            [sensor["sample_time"] for sensor in snapshot["sensors"]],
            [0x00010002, 0x00030004, 0x89ABCDEF],
        )
        self.assertTrue(
            all(sensor["source"] == "REAL_DHT11" for sensor in snapshot["sensors"])
        )
        self.assertTrue(
            all("sample_time_ms" not in sensor for sensor in snapshot["sensors"])
        )

        request = transport.writes[0]
        self.assertEqual(request[1], FUNCTION_READ_INPUT)
        self.assertEqual(
            struct.unpack_from(">HH", request, 2),
            (INPUT_DHT11_START, INPUT_DHT11_COUNT),
        )

    def test_not_present_sensor_has_no_fake_values(self) -> None:
        values = dht11_values(
            valid_mask=0x0003,
            temperatures=(200, 210, 0),
            humidities=(500, 510, 0),
            qualities=(1, 1, 6),
            errors=(0, 0, 1),
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
        self.assertIsNone(sensor["temperature_x10"])
        self.assertIsNone(sensor["humidity_x10"])

    def test_source_zero_before_first_sample_is_not_present(self) -> None:
        values = dht11_values(
            source=0,
            valid_mask=0,
            sample_id=0,
            temperatures=(230, 240, 250),
            humidities=(450, 550, 650),
            qualities=(0, 0, 0),
            errors=(0, 0, 0),
            sample_times=(100, 200, 300),
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
            all(sensor["temperature_x10"] is None for sensor in snapshot["sensors"])
        )
        self.assertTrue(
            all(sensor["humidity_x10"] is None for sensor in snapshot["sensors"])
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
        self.assertEqual(raised.exception.details["start"], INPUT_DHT11_START)
        self.assertEqual(raised.exception.details["count"], INPUT_DHT11_COUNT)
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

    def test_unsupported_contract_revision_is_interface_unavailable(self) -> None:
        values = dht11_values(revision=2)
        service, client, _transport = self._service(
            lambda request: read_response(request, values)
        )
        try:
            with self.assertRaises(UnsupportedProtocolError) as raised:
                service.read_temperature_snapshot()
        finally:
            client.close()

        self.assertTrue(raised.exception.details["interface_unavailable"])
        self.assertEqual(raised.exception.details["contract_revision"], 2)

    def test_decoder_rejects_wrong_block_length(self) -> None:
        with self.assertRaises(ValueError):
            registers.decode_dht11_snapshot([1, 2, 3])

    def test_legacy_snapshot_decoder_names_real_dht11_source(self) -> None:
        decoded = registers.decode_snapshot(
            (1, 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        )
        self.assertEqual(decoded["source"], "REAL_DHT11")


if __name__ == "__main__":
    unittest.main()
