"""Client error types with stable CLI exit-code semantics."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


class ModbusClientError(Exception):
    kind = "client_error"
    exit_code = 1

    def __init__(self, message: str, **details: Any) -> None:
        super().__init__(message)
        self.message = message
        self.details = details

    def as_error(self) -> dict[str, Any]:
        error: dict[str, Any] = {
            "kind": self.kind,
            "message": self.message,
            "function": self.details.get("function"),
            "exception_code": self.details.get("exception_code"),
        }
        for key, value in self.details.items():
            if key not in error:
                error[key] = value
        return error


class ArgumentError(ModbusClientError):
    kind = "argument"
    exit_code = 2


class TransportError(ModbusClientError):
    kind = "transport"
    exit_code = 3


class TransactionTimeout(ModbusClientError):
    kind = "timeout"
    exit_code = 4


class ModbusException(ModbusClientError):
    kind = "modbus_exception"
    exit_code = 5

    def __init__(
        self,
        function: int,
        exception_code: int,
        message: str | None = None,
    ) -> None:
        text = message or (
            f"Modbus exception 0x{exception_code:02X} for function 0x{function:02X}"
        )
        super().__init__(
            text,
            function=function,
            exception_code=exception_code,
        )
        self.function = function
        self.exception_code = exception_code
        self.observation: dict[str, Any] | None = None


class ProtocolError(ModbusClientError):
    kind = "response_validation"
    exit_code = 6


class StateError(ModbusClientError):
    kind = "state_mismatch"
    exit_code = 7


class FrameLogError(TransportError):
    kind = "frame_log"


@dataclass
class PartialResult:
    """Evidence retained when a write acknowledgement was observed."""

    values: dict[str, Any] = field(default_factory=dict)

    def as_result(self) -> dict[str, Any]:
        return dict(self.values)
