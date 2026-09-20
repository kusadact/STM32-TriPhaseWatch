from __future__ import annotations

import unittest

from tools.modbus_client.errors import ModbusException, ProtocolError
from tools.modbus_client.protocol import (
    FUNCTION_READ_HOLDING,
    FUNCTION_READ_INPUT,
    FUNCTION_WRITE_MULTIPLE,
    FUNCTION_WRITE_SINGLE,
    append_crc,
    build_read_request,
    build_write_multiple_request,
    build_write_single_request,
    crc16,
    expected_response_length,
    parse_response,
)


class ProtocolTests(unittest.TestCase):
    def test_crc_standard_vector(self) -> None:
        self.assertEqual(crc16(b"123456789"), 0x4B37)
        self.assertEqual(append_crc(b"123456789")[-2:], bytes((0x37, 0x4B)))

    def test_read_request_and_response(self) -> None:
        request = build_read_request(1, FUNCTION_READ_INPUT, 0x0020, 15)
        self.assertEqual(request.hex(" "), "01 04 00 20 00 0f b1 c4")
        response = append_crc(
            bytes.fromhex(
                "01041e000100020003000400050006000700080009000a000b000c000d000e000f"
            )
        )
        parsed = parse_response(request, response)
        self.assertEqual(parsed.values[0], 1)
        self.assertEqual(parsed.values[-1], 15)

    def test_write_requests_and_echoes(self) -> None:
        single = build_write_single_request(1, 0x0040, 3)
        self.assertEqual(single.hex(" "), "01 06 00 40 00 03 c8 1f")
        self.assertEqual(parse_response(single, single).function, FUNCTION_WRITE_SINGLE)

        multiple = build_write_multiple_request(1, 0x0040, [5, 0, 1001])
        self.assertEqual(multiple[1], FUNCTION_WRITE_MULTIPLE)
        self.assertEqual(multiple[6], 6)
        response = append_crc(bytes.fromhex("011000400003"))
        parsed = parse_response(multiple, response)
        self.assertEqual(parsed.function, FUNCTION_WRITE_MULTIPLE)

    def test_exception_response(self) -> None:
        request = build_write_single_request(1, 0x0040, 2)
        response = append_crc(bytes.fromhex("018604"))
        with self.assertRaises(ModbusException) as raised:
            parse_response(request, response)
        self.assertEqual(raised.exception.exception_code, 4)

    def test_protocol_errors(self) -> None:
        request = build_read_request(1, FUNCTION_READ_HOLDING, 0, 2)
        good = append_crc(bytes.fromhex("01030411223344"))

        for bad in (
            good[:-1] + bytes((good[-1] ^ 1,)),
            bytes((2,)) + good[1:],
            bytes((1, 4)) + good[2:],
            good[:2] + bytes((3,)) + good[3:],
        ):
            with self.assertRaises(ProtocolError):
                parse_response(request, bad)

        with self.assertRaises(ProtocolError):
            parse_response(request, good + b"\x00")

        write = build_write_single_request(1, 0x0040, 3)
        wrong_echo = bytearray(write[:6])
        wrong_echo[-3] ^= 1
        with self.assertRaises(ProtocolError):
            parse_response(write, append_crc(bytes(wrong_echo)))

    def test_bounds(self) -> None:
        with self.assertRaises(ValueError):
            build_read_request(1, FUNCTION_READ_HOLDING, 0, 0)
        with self.assertRaises(ValueError):
            build_read_request(1, FUNCTION_READ_HOLDING, 0xFFFF, 2)
        with self.assertRaises(ValueError):
            build_write_multiple_request(1, 0, [0] * 124)
        with self.assertRaises(ValueError):
            build_write_single_request(1, 0, 0x10000)
        self.assertEqual(
            expected_response_length(build_write_multiple_request(1, 0, [1] * 123)),
            8,
        )


if __name__ == "__main__":
    unittest.main()
