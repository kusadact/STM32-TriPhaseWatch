from __future__ import annotations

import struct
import unittest

from tools.modbus_client.client import ModbusClient
from tools.modbus_client.errors import (
    ModbusException,
    ProtocolError,
    TransactionTimeout,
    TransportError,
)
from tools.modbus_client.protocol import (
    FUNCTION_READ_HOLDING,
    FUNCTION_READ_INPUT,
    FUNCTION_WRITE_MULTIPLE,
    FUNCTION_WRITE_SINGLE,
    append_crc,
    build_read_request,
    build_write_multiple_request,
    build_write_single_request,
)
from fake_transport import ScriptedTransport


def response_for(request: bytes, values: tuple[int, ...] | None = None) -> bytes:
    function = request[1]
    if function in (FUNCTION_READ_HOLDING, FUNCTION_READ_INPUT):
        if values is None:
            raise AssertionError("read response needs values")
        payload = bytes((request[0], function, len(values) * 2))
        payload += b"".join(struct.pack(">H", value) for value in values)
        return append_crc(payload)
    if function == FUNCTION_WRITE_SINGLE:
        return request
    if function == FUNCTION_WRITE_MULTIPLE:
        return append_crc(request[:6])
    raise AssertionError(f"unsupported function {function}")


class ClientTests(unittest.TestCase):
    def test_short_read_and_write_are_assembled(self) -> None:
        request = build_read_request(1, FUNCTION_READ_INPUT, 0, 2)
        response = response_for(request, (0x1234, 0x5678))
        transport = ScriptedTransport(
            lambda _request: [response[:1], response[1:4], response[4:]],
            write_fragments=[1, 2],
        )
        client = ModbusClient(
            transport,
            timeout_seconds=0.1,
            recovery_seconds=0.001,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.01,
        )
        client.open()
        result = client.transaction(request)
        self.assertEqual(result.response.values, (0x1234, 0x5678))
        self.assertEqual(
            transport.writes,
            [request[:1], request[1:3], request[3:]],
        )
        self.assertEqual(len(set(transport.read_deadlines)), 1)
        client.close()

    def test_timeout_does_not_retry(self) -> None:
        request = build_write_single_request(1, 0x0040, 3)
        transport = ScriptedTransport(lambda _request: None)
        client = ModbusClient(
            transport,
            timeout_seconds=0.01,
            recovery_seconds=0.001,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.01,
        )
        client.open()
        with self.assertRaises(TransactionTimeout) as raised:
            client.transaction(request)
        self.assertTrue(raised.exception.details["write_may_have_executed"])
        self.assertEqual(len(transport.writes), 1)
        self.assertEqual(transport.recoveries, 1)
        client.close()

    def test_bad_crc_does_not_report_success(self) -> None:
        request = build_read_request(1, FUNCTION_READ_HOLDING, 0, 1)
        bad = bytearray(response_for(request, (10,)))
        bad[-1] ^= 1
        transport = ScriptedTransport(lambda _request: bytes(bad))
        client = ModbusClient(
            transport,
            timeout_seconds=0.01,
            recovery_seconds=0.001,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.01,
        )
        client.open()
        with self.assertRaises(ProtocolError):
            client.transaction(request)
        self.assertEqual(len(transport.writes), 1)
        self.assertEqual(transport.recoveries, 1)
        client.close()

    def test_extra_data_is_rejected(self) -> None:
        request = build_read_request(1, FUNCTION_READ_HOLDING, 0, 1)
        transport = ScriptedTransport(
            lambda _request: response_for(request, (10,)) + b"\x00"
        )
        client = ModbusClient(
            transport,
            timeout_seconds=0.01,
            recovery_seconds=0.001,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.01,
        )
        client.open()
        with self.assertRaises(ProtocolError):
            client.transaction(request)
        client.close()

    def test_modbus_exception_is_not_retried(self) -> None:
        request = build_write_single_request(1, 0x0040, 2)
        exception = append_crc(bytes.fromhex("018604"))
        transport = ScriptedTransport(lambda _request: exception)
        client = ModbusClient(transport, timeout_seconds=0.01)
        client.open()
        with self.assertRaises(ModbusException):
            client.transaction(request)
        self.assertEqual(len(transport.writes), 1)
        self.assertEqual(transport.recoveries, 0)
        client.close()

    def test_disconnect_is_not_retried(self) -> None:
        request = build_read_request(1, FUNCTION_READ_HOLDING, 0, 1)

        def handler(_request: bytes) -> bytes:
            raise TransportError("simulated disconnect")

        transport = ScriptedTransport(handler)
        client = ModbusClient(transport, timeout_seconds=0.01)
        client.open()
        with self.assertRaises(TransportError):
            client.transaction(request)
        self.assertEqual(len(transport.writes), 1)
        self.assertTrue(transport.closed)

    def test_maximum_read_and_write_frames(self) -> None:
        read_request = build_read_request(
            1,
            FUNCTION_READ_INPUT,
            0,
            125,
        )
        read_transport = ScriptedTransport(
            lambda request: response_for(request, tuple(range(125)))
        )
        read_client = ModbusClient(read_transport, timeout_seconds=0.01)
        read_client.open()
        read_result = read_client.transaction(read_request)
        self.assertEqual(len(read_result.response.values), 125)
        read_client.close()

        write_request = build_write_multiple_request(1, 0, [1] * 123)
        write_transport = ScriptedTransport(lambda request: response_for(request))
        write_client = ModbusClient(write_transport, timeout_seconds=0.01)
        write_client.open()
        write_result = write_client.transaction(write_request)
        self.assertEqual(write_result.response.function, FUNCTION_WRITE_MULTIPLE)
        self.assertEqual(len(write_transport.writes), 1)
        write_client.close()

    def test_persistent_garbage_keeps_one_deadline_and_no_success(self) -> None:
        request = build_read_request(1, FUNCTION_READ_HOLDING, 0, 1)
        transport = ScriptedTransport(lambda _request: [b"\xaa"] * 7)
        client = ModbusClient(
            transport,
            timeout_seconds=0.01,
            recovery_seconds=0.001,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.01,
        )
        client.open()
        with self.assertRaises(ProtocolError):
            client.transaction(request)
        self.assertEqual(len(transport.writes), 1)
        self.assertEqual(len(set(transport.read_deadlines)), 1)
        client.close()

    def test_stale_response_is_not_used_by_next_transaction(self) -> None:
        first = build_read_request(1, FUNCTION_READ_HOLDING, 0, 1)
        second = build_read_request(1, FUNCTION_READ_HOLDING, 1, 1)
        stale = response_for(first, (10,))
        transport = ScriptedTransport(lambda _request: None)
        client = ModbusClient(
            transport,
            timeout_seconds=0.01,
            recovery_seconds=0.001,
            recovery_quiet_seconds=0.001,
            recovery_limit_seconds=0.01,
        )
        client.open()
        with self.assertRaises(TransactionTimeout):
            client.transaction(first)
        transport.queue_extra(stale)
        with self.assertRaises(ProtocolError):
            client.transaction(second)
        self.assertEqual(len(transport.writes), 1)
        client.close()


if __name__ == "__main__":
    unittest.main()
