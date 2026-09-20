"""Pure Modbus RTU request construction and response validation."""

from __future__ import annotations

from dataclasses import dataclass
import struct

from .errors import ModbusException, ProtocolError

FUNCTION_READ_HOLDING = 0x03
FUNCTION_READ_INPUT = 0x04
FUNCTION_WRITE_SINGLE = 0x06
FUNCTION_WRITE_MULTIPLE = 0x10

MAX_READ_REGISTERS = 125
MAX_WRITE_REGISTERS = 123
MAX_ADU_SIZE = 256


@dataclass(frozen=True)
class Response:
    address: int
    function: int
    values: tuple[int, ...]
    exception_code: int | None = None


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def append_crc(payload: bytes) -> bytes:
    crc = crc16(payload)
    return payload + bytes((crc & 0xFF, crc >> 8))


def response_has_valid_crc(response: bytes) -> bool:
    if len(response) < 4:
        return False
    received = response[-2] | (response[-1] << 8)
    return received == crc16(response[:-2])


def _require_u16(value: int, name: str) -> None:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f"{name} must be an integer")
    if not 0 <= value <= 0xFFFF:
        raise ValueError(f"{name} must be in 0..65535")


def _require_unit_id(address: int) -> None:
    if not isinstance(address, int) or isinstance(address, bool):
        raise ValueError("address must be an integer")
    if not 0 <= address <= 247:
        raise ValueError("address must be in 0..247")


def _validate_range(start: int, quantity: int, maximum: int) -> None:
    _require_u16(start, "start")
    if not isinstance(quantity, int) or isinstance(quantity, bool):
        raise ValueError("quantity must be an integer")
    if not 1 <= quantity <= maximum:
        raise ValueError(f"quantity must be in 1..{maximum}")
    if start + quantity > 0x10000:
        raise ValueError("start + quantity exceeds the 16-bit address space")


def build_read_request(
    address: int,
    function: int,
    start: int,
    quantity: int,
) -> bytes:
    _require_unit_id(address)
    if function not in (FUNCTION_READ_HOLDING, FUNCTION_READ_INPUT):
        raise ValueError("read function must be 0x03 or 0x04")
    _validate_range(start, quantity, MAX_READ_REGISTERS)
    payload = struct.pack(">BBHH", address, function, start, quantity)
    return append_crc(payload)


def build_write_single_request(
    address: int,
    register: int,
    value: int,
) -> bytes:
    _require_unit_id(address)
    _require_u16(register, "register")
    _require_u16(value, "value")
    payload = struct.pack(
        ">BBHH",
        address,
        FUNCTION_WRITE_SINGLE,
        register,
        value,
    )
    return append_crc(payload)


def build_write_multiple_request(
    address: int,
    start: int,
    values: list[int] | tuple[int, ...],
) -> bytes:
    _require_unit_id(address)
    _validate_range(start, len(values), MAX_WRITE_REGISTERS)
    for index, value in enumerate(values):
        _require_u16(value, f"values[{index}]")
    byte_count = len(values) * 2
    payload = struct.pack(
        ">BBHHB",
        address,
        FUNCTION_WRITE_MULTIPLE,
        start,
        len(values),
        byte_count,
    )
    payload += b"".join(struct.pack(">H", value) for value in values)
    return append_crc(payload)


def expected_response_length(request: bytes) -> int:
    if len(request) != 8:
        if request[1:2] == bytes((FUNCTION_WRITE_MULTIPLE,)):
            return 8
        raise ValueError("invalid request length")
    function = request[1]
    if function in (FUNCTION_READ_HOLDING, FUNCTION_READ_INPUT):
        quantity = struct.unpack_from(">H", request, 4)[0]
        return 5 + (2 * quantity)
    if function in (FUNCTION_WRITE_SINGLE, FUNCTION_WRITE_MULTIPLE):
        return 8
    raise ValueError(f"unsupported function 0x{function:02X}")


def parse_response(request: bytes, response: bytes) -> Response:
    if len(request) < 4 or len(request) > MAX_ADU_SIZE:
        raise ProtocolError("invalid request size")
    if len(response) < 5 or len(response) > MAX_ADU_SIZE:
        raise ProtocolError(
            f"invalid response size {len(response)}",
            response_length=len(response),
        )
    if not response_has_valid_crc(response):
        raise ProtocolError("response CRC mismatch")

    request_address = request[0]
    request_function = request[1]
    if response[0] != request_address:
        raise ProtocolError(
            "response address mismatch",
            expected_address=request_address,
            actual_address=response[0],
        )

    response_function = response[1]
    if response_function == (request_function | 0x80):
        if len(response) != 5:
            raise ProtocolError("exception response must be exactly 5 bytes")
        raise ModbusException(request_function, response[2])

    if response_function != request_function:
        raise ProtocolError(
            "response function mismatch",
            expected_function=request_function,
            actual_function=response_function,
        )

    if request_function in (FUNCTION_READ_HOLDING, FUNCTION_READ_INPUT):
        if len(request) != 8:
            raise ProtocolError("read request must be exactly 8 bytes")
        quantity = struct.unpack_from(">H", request, 4)[0]
        expected_length = 5 + (quantity * 2)
        if len(response) != expected_length:
            raise ProtocolError(
                f"read response length is {len(response)}, expected {expected_length}",
                expected_length=expected_length,
                actual_length=len(response),
            )
        expected_byte_count = quantity * 2
        if response[2] != expected_byte_count:
            raise ProtocolError(
                "read response byte-count mismatch",
                expected_byte_count=expected_byte_count,
                actual_byte_count=response[2],
            )
        values = struct.unpack(f">{quantity}H", response[3:-2])
        return Response(request_address, request_function, tuple(values))

    if request_function == FUNCTION_WRITE_SINGLE:
        if len(response) != 8 or response != request:
            raise ProtocolError("write-single response did not echo the request")
        return Response(request_address, request_function, ())

    if request_function == FUNCTION_WRITE_MULTIPLE:
        if len(response) != 8:
            raise ProtocolError("write-multiple response must be 8 bytes")
        if len(request) < 9:
            raise ProtocolError("write-multiple request is too short")
        expected_start = struct.unpack_from(">H", request, 2)[0]
        expected_quantity = struct.unpack_from(">H", request, 4)[0]
        actual_start = struct.unpack_from(">H", response, 2)[0]
        actual_quantity = struct.unpack_from(">H", response, 4)[0]
        if (actual_start, actual_quantity) != (
            expected_start,
            expected_quantity,
        ):
            raise ProtocolError(
                "write-multiple response range mismatch",
                expected_start=expected_start,
                expected_quantity=expected_quantity,
                actual_start=actual_start,
                actual_quantity=actual_quantity,
            )
        return Response(request_address, request_function, ())

    raise ProtocolError(f"unsupported response function 0x{request_function:02X}")
