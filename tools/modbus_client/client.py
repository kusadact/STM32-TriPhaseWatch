"""Single-outstanding-transaction Modbus client."""

from __future__ import annotations

from dataclasses import dataclass
import time

from .errors import (
    FrameLogError,
    ModbusClientError,
    ModbusException,
    ProtocolError,
    TransactionTimeout,
    TransportError,
)
from .frame_log import FrameLogger
from .protocol import Response, expected_response_length, parse_response
from .transport import Transport

MAX_RESPONSE_BUFFER = 512


@dataclass(frozen=True)
class TransactionResult:
    response: Response
    request: bytes
    response_bytes: bytes
    elapsed_ms: float


class ModbusClient:
    def __init__(
        self,
        transport: Transport,
        *,
        timeout_seconds: float = 3.0,
        minimum_gap_seconds: float = 0.010,
        recovery_seconds: float = 1.5,
        recovery_quiet_seconds: float = 0.010,
        recovery_limit_seconds: float = 3.0,
        logger: FrameLogger | None = None,
    ) -> None:
        self.transport = transport
        self.timeout_seconds = timeout_seconds
        self.minimum_gap_seconds = minimum_gap_seconds
        self.recovery_seconds = recovery_seconds
        self.recovery_quiet_seconds = recovery_quiet_seconds
        self.recovery_limit_seconds = recovery_limit_seconds
        self.logger = logger
        self._opened = False
        self._closed = False
        self._transaction_id = 0
        self._last_activity_end: float | None = None

    def open(self) -> None:
        if self._closed:
            raise TransportError("client is closed")
        if self._opened:
            return
        self.transport.open()
        self._opened = True

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self.transport.close()
        finally:
            self._opened = False

    def transaction(self, request: bytes) -> TransactionResult:
        if not self._opened:
            raise TransportError("client is not open")
        if self._closed:
            raise TransportError("client is closed")

        self._transaction_id += 1
        transaction_id = self._transaction_id
        try:
            self._wait_for_transaction_gap()
            pending = self.transport.read_available(MAX_RESPONSE_BUFFER)
            if pending:
                raise ProtocolError(
                    "unexpected bytes were pending before request",
                    unexpected_bytes=pending.hex(" "),
                )

            deadline = time.monotonic() + self.timeout_seconds
            started = time.monotonic()
            self._set_transport_transaction(transaction_id)
            self._log(
                "tx",
                request,
                transaction_id=transaction_id,
                deadline=deadline,
                result="assembled",
            )
            try:
                self.transport.write_all(request, deadline)
            except TransportError as exc:
                if time.monotonic() >= deadline:
                    sent_bytes = int(exc.details.get("bytes_written", 0))
                    raise TransactionTimeout(
                        "deadline expired while sending request",
                        write_may_have_executed=sent_bytes > 0,
                        sent_bytes=sent_bytes,
                    ) from exc
                raise

            expected_length = expected_response_length(request)
            response = bytearray()
            while True:
                if len(response) >= 2 and response[1] == (request[1] | 0x80):
                    if len(response) > 5:
                        raise ProtocolError(
                            "exception response exceeded 5 bytes",
                            actual_length=len(response),
                        )
                    if len(response) == 5:
                        break
                elif len(response) >= expected_length:
                    break
                if len(response) > expected_length:
                    raise ProtocolError(
                        "response exceeded the expected length",
                        expected_length=expected_length,
                        actual_length=len(response),
                    )
                chunk = self.transport.read(
                    expected_length - len(response),
                    deadline,
                )
                if chunk is None:
                    raise TransactionTimeout(
                        "deadline expired before a complete response arrived",
                        write_may_have_executed=request[1] in (0x06, 0x10),
                        received_bytes=response.hex(" "),
                    )
                response.extend(chunk)

            trailing = self.transport.read_available(MAX_RESPONSE_BUFFER)
            if trailing:
                raise ProtocolError(
                    "response had unexpected trailing bytes",
                    expected_length=expected_length,
                    trailing_bytes=trailing.hex(" "),
                )

            response_bytes = bytes(response)
            self._log(
                "rx",
                response_bytes,
                transaction_id=transaction_id,
                deadline=deadline,
                result="assembled",
            )
            try:
                parsed = parse_response(request, response_bytes)
            except ModbusException as exc:
                self._last_activity_end = time.monotonic()
                self._log(
                    "transaction",
                    b"",
                    transaction_id=transaction_id,
                    deadline=deadline,
                    result="exception",
                    error=str(exc),
                )
                raise
            elapsed_ms = (time.monotonic() - started) * 1000.0
            self._last_activity_end = time.monotonic()
            self._log(
                "transaction",
                b"",
                transaction_id=transaction_id,
                deadline=deadline,
                result="ok",
                extra={"elapsed_ms": round(elapsed_ms, 3)},
            )
            return TransactionResult(
                response=parsed,
                request=request,
                response_bytes=response_bytes,
                elapsed_ms=elapsed_ms,
            )
        except FrameLogError:
            self.close()
            raise
        except ModbusException:
            raise
        except ModbusClientError as exc:
            self._last_activity_end = time.monotonic()
            if isinstance(exc, TransportError) and not isinstance(
                exc,
                TransactionTimeout,
            ):
                self.close()
                raise
            recovered = self._recover(transaction_id, exc)
            if not recovered:
                exc.details["recovery_failed"] = True
                self.close()
            raise

    def _wait_for_transaction_gap(self) -> None:
        if self._last_activity_end is None:
            return
        target = self._last_activity_end + self.minimum_gap_seconds
        remaining = target - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)

    def _recover(self, transaction_id: int, cause: Exception) -> bool:
        deadline = time.monotonic() + self.recovery_limit_seconds
        self._set_transport_transaction(transaction_id)
        try:
            recovered = self.transport.recover(
                self.recovery_seconds,
                self.recovery_quiet_seconds,
                deadline,
            )
        except ModbusClientError:
            recovered = False
        self._log(
            "recovery",
            b"",
            transaction_id=transaction_id,
            deadline=deadline,
            result="ok" if recovered else "failed",
            error=str(cause),
        )
        return recovered

    def _set_transport_transaction(self, transaction_id: int) -> None:
        setattr(self.transport, "_log_transaction", transaction_id)

    def _log(
        self,
        direction: str,
        data: bytes,
        *,
        transaction_id: int,
        deadline: float | None = None,
        result: str | None = None,
        error: str | None = None,
        extra: dict[str, object] | None = None,
    ) -> None:
        if self.logger is not None:
            self.logger.log(
                direction,
                data,
                transaction_id=transaction_id,
                deadline=deadline,
                result=result,
                error=error,
                extra=extra,
            )

    def __enter__(self) -> "ModbusClient":
        self.open()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()
