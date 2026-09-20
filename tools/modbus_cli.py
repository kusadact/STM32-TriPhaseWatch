#!/usr/bin/env python3
"""Command-line access to the board A Modbus RTU register contract."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
from typing import Any, Callable

if __package__:
    from .modbus_client.client import ModbusClient
    from .modbus_client.errors import (
        ArgumentError,
        FrameLogError,
        ModbusClientError,
        ModbusException,
    )
    from .modbus_client.frame_log import FrameLogger
    from .modbus_client.service import ModbusService
    from .modbus_client.transport import MacOSTTYTransport, Transport
else:
    from modbus_client.client import ModbusClient
    from modbus_client.errors import (
        ArgumentError,
        FrameLogError,
        ModbusClientError,
        ModbusException,
    )
    from modbus_client.frame_log import FrameLogger
    from modbus_client.service import ModbusService
    from modbus_client.transport import MacOSTTYTransport, Transport

EXIT_OK = 0
EXIT_ARGUMENT = 2
EXIT_TRANSPORT = 3
EXIT_TIMEOUT = 4
EXIT_MODBUS_EXCEPTION = 5
EXIT_PROTOCOL = 6
EXIT_STATE = 7
EXIT_INTERRUPTED = 130

TransportFactory = Callable[[argparse.Namespace, FrameLogger | None], Transport]


def _unsigned(text: str, maximum: int, name: str) -> int:
    if not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", text):
        raise argparse.ArgumentTypeError(
            f"{name} must be an unsigned decimal or 0x-prefixed integer"
        )
    value = int(text, 16 if text.lower().startswith("0x") else 10)
    if value > maximum:
        raise argparse.ArgumentTypeError(f"{name} must be in 0..{maximum}")
    return value


def _u16(text: str) -> int:
    return _unsigned(text, 0xFFFF, "value")


def _u32(text: str) -> int:
    return _unsigned(text, 0xFFFFFFFF, "value")


def _address(text: str) -> int:
    value = _unsigned(text, 247, "address")
    if value == 0:
        raise argparse.ArgumentTypeError("address must be in 1..247")
    return value


def _count(text: str) -> int:
    value = _unsigned(text, 125, "count")
    if value == 0:
        raise argparse.ArgumentTypeError("count must be in 1..125")
    return value


def _timeout(text: str) -> float:
    try:
        value = float(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("timeout must be a finite number") from exc
    if not math.isfinite(value) or value < 3.0:
        raise argparse.ArgumentTypeError("timeout must be finite and at least 3.0")
    return value


def _interval(text: str) -> float:
    try:
        value = float(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("interval must be a finite number") from exc
    if not math.isfinite(value) or value < 1.0:
        raise argparse.ArgumentTypeError("interval must be finite and at least 1.0")
    return value


def _positive(text: str) -> int:
    value = _unsigned(text, 0x7FFFFFFF, "samples")
    if value == 0:
        raise argparse.ArgumentTypeError("samples must be positive")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Modbus RTU CLI for the board A water-quality monitor."
    )
    parser.add_argument("--port", required=True, help="serial device path")
    parser.add_argument("--address", type=_address, default=1)
    parser.add_argument("--timeout", type=_timeout, default=3.0)
    parser.add_argument("--json", action="store_true", dest="json_output")
    parser.add_argument("--frame-log", metavar="PATH")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("identity")
    subparsers.add_parser("status")
    subparsers.add_parser("snapshot")
    subparsers.add_parser("stats")

    read_holding = subparsers.add_parser("read-holding")
    read_holding.add_argument("--start", type=_u16, required=True)
    read_holding.add_argument("--count", type=_count, required=True)

    read_input = subparsers.add_parser("read-input")
    read_input.add_argument("--start", type=_u16, required=True)
    read_input.add_argument("--count", type=_count, required=True)

    write_single = subparsers.add_parser("write-single")
    write_single.add_argument("--register", type=_u16, required=True)
    write_single.add_argument("--value", type=_u16, required=True)

    write_multiple = subparsers.add_parser("write-multiple")
    write_multiple.add_argument("--start", type=_u16, required=True)
    write_multiple.add_argument(
        "--values",
        type=_u16,
        nargs="+",
        required=True,
    )

    config = subparsers.add_parser("config")
    config.add_argument("--period", type=_u16, required=True)
    config.add_argument("--channels", type=_u16, required=True)
    config.add_argument("--count", type=_u16, required=True)

    subparsers.add_parser("apply")
    subparsers.add_parser("save")
    subparsers.add_parser("start")
    subparsers.add_parser("stop")

    single = subparsers.add_parser("single")
    single.add_argument("--id", type=_u32, required=True)

    watch = subparsers.add_parser("watch")
    watch.add_argument("--interval", type=_interval, required=True)
    watch_mode = watch.add_mutually_exclusive_group(required=True)
    watch_mode.add_argument("--samples", type=_positive)
    watch_mode.add_argument("--forever", action="store_true")
    return parser


def _validate_business_arguments(args: argparse.Namespace) -> None:
    if args.command in ("read-holding", "read-input"):
        if args.start + args.count > 0x10000:
            raise ArgumentError("start + count exceeds the 16-bit address space")
    if args.command == "write-multiple" and not 1 <= len(args.values) <= 123:
        raise ArgumentError("write-multiple requires 1..123 values")
    if args.command == "write-multiple":
        if args.start + len(args.values) > 0x10000:
            raise ArgumentError("start + values exceeds the 16-bit address space")
    if args.command == "config":
        if not 10 <= args.period <= 3600:
            raise ArgumentError("period must be in 10..3600")
        if not 1 <= args.channels <= 15:
            raise ArgumentError("channels must be in 1..15")
        if not 0 <= args.count <= 65535:
            raise ArgumentError("count must be in 0..65535")


def _dispatch(
    args: argparse.Namespace,
    service: ModbusService,
) -> dict[str, Any]:
    if args.command == "identity":
        return service.identity()
    if args.command == "status":
        return service.status()
    if args.command == "snapshot":
        return service.snapshot()
    if args.command == "stats":
        return service.stats()
    if args.command == "read-holding":
        values = service.read_holding(args.start, args.count)
        return {
            "start": args.start,
            "count": args.count,
            "values": list(values),
            "registers": [
                {"address": args.start + index, "value": value}
                for index, value in enumerate(values)
            ],
        }
    if args.command == "read-input":
        values = service.read_input(args.start, args.count)
        return {
            "start": args.start,
            "count": args.count,
            "values": list(values),
            "registers": [
                {"address": args.start + index, "value": value}
                for index, value in enumerate(values)
            ],
        }
    if args.command == "write-single":
        service.write_single(args.register, args.value)
        return {
            "register": args.register,
            "value": args.value,
            "write_acknowledged": True,
        }
    if args.command == "write-multiple":
        service.write_multiple(args.start, args.values)
        return {
            "start": args.start,
            "count": len(args.values),
            "values": list(args.values),
            "write_acknowledged": True,
        }
    if args.command == "config":
        return service.config(args.period, args.channels, args.count)
    if args.command == "apply":
        return service.apply()
    if args.command == "save":
        return service.save()
    if args.command == "start":
        return service.start()
    if args.command == "stop":
        return service.stop()
    if args.command == "single":
        return service.single(args.id)
    raise ArgumentError(f"unsupported command {args.command!r}")


def _envelope(
    args: argparse.Namespace,
    result: dict[str, Any] | None,
    error: ModbusClientError | None,
    elapsed_ms: float,
) -> dict[str, Any]:
    if error is None:
        error_payload = None
        ok = True
    else:
        error_payload = error.as_error()
        if isinstance(error, ModbusException) and error.observation is not None:
            error_payload["observation"] = error.observation
        if result is None:
            result = {}
        ok = False
    return {
        "ok": ok,
        "operation": args.command,
        "address": args.address,
        "result": result,
        "error": error_payload,
        "elapsed_ms": round(elapsed_ms, 3),
    }


def _partial_result(error: ModbusClientError) -> dict[str, Any]:
    result: dict[str, Any] = {}
    if error.details.get("write_acknowledged"):
        result["write_acknowledged"] = True
    observation = error.details.get("observation")
    if observation is not None:
        result["observation"] = observation
    return result


def _emit_json(payload: dict[str, Any]) -> None:
    print(json.dumps(payload, separators=(",", ":"), ensure_ascii=True), flush=True)


def _emit_text(payload: dict[str, Any]) -> None:
    if not payload["ok"]:
        error = payload["error"]
        print(
            f"{payload['operation']}: {error['kind']}: {error['message']}",
            file=sys.stderr,
        )
        return
    operation = payload["operation"]
    result = payload["result"]
    if operation == "identity":
        print(
            "identity: "
            f"device_type={result['device_type']} "
            f"reported_version={result['reported_version']} "
            f"protocol_version={result['protocol_version']}"
        )
    elif operation == "status":
        config = result["active_config"]
        print(
            "status: "
            f"run_state={result['run_state']} "
            f"active_period={config['period_sec']}s "
            f"active_mask=0x{config['channel_mask']:04X} "
            f"active_count={config['record_count']} "
            f"config_version={config['version']} "
            f"last_command={result['last_command']['name']} "
            f"result={result['last_command']['result']}"
        )
    elif operation == "snapshot":
        print(
            "snapshot: "
            f"valid={str(result['valid']).lower()} "
            f"sequence={result['sequence']} "
            f"trigger={result['trigger']} "
            f"unit={result['unit']}"
        )
        for channel in result["channels"]:
            print(
                f"channel[{channel['index']}]: "
                f"value={channel['value']} "
                f"quality={channel['quality']}"
            )
    elif operation == "stats":
        for key, value in result.items():
            print(f"{key}={value}")
    elif operation in ("read-holding", "read-input"):
        print(
            f"{operation}: start=0x{result['start']:04X} "
            f"count={result['count']} values="
            + " ".join(f"0x{value:04X}" for value in result["values"])
        )
    elif operation == "write-single":
        print(
            "write-single: "
            f"register=0x{result['register']:04X} "
            f"value={result['value']} write_acknowledged=true"
        )
    elif operation == "write-multiple":
        print(
            "write-multiple: "
            f"start=0x{result['start']:04X} "
            f"count={result['count']} write_acknowledged=true"
        )
    elif operation == "config":
        print(
            "config: "
            f"period={result['period_sec']}s "
            f"channels=0x{result['channel_mask']:04X} "
            f"count={result['record_count']} "
            "staged=true readback_matched=true"
        )
    elif operation in ("apply", "start", "stop"):
        command = result["last_command"]
        print(
            f"{operation}: result={command['result']} "
            f"run_state={result['run_state']} "
            f"config_version={result['active_config']['version']}"
        )
    elif operation == "single":
        command = result["command"]["last_command"]
        snapshot = result["snapshot"]
        print(
            "single: "
            f"result={command['result']} "
            f"duplicate={str(result['duplicate']).lower()} "
            f"sequence={snapshot['sequence']} "
            f"valid={str(snapshot['valid']).lower()}"
        )
    elif operation == "watch":
        snapshot = result["snapshot"]
        print(
            f"watch[{result['sample']}]: "
            f"sequence={snapshot['sequence']} "
            f"valid={str(snapshot['valid']).lower()} "
            f"trigger={snapshot['trigger']}"
        )
    else:
        print(json.dumps(result, separators=(",", ":"), ensure_ascii=True))


def _run_watch(
    args: argparse.Namespace,
    service: ModbusService,
) -> int:
    sample = 0
    next_start = time.monotonic()
    while args.forever or sample < args.samples:
        sample += 1
        sample_started = time.monotonic()
        snapshot = service.snapshot()
        payload = _envelope(
            args,
            {"sample": sample, "snapshot": snapshot},
            None,
            (time.monotonic() - sample_started) * 1000.0,
        )
        if args.json_output:
            _emit_json(payload)
        else:
            _emit_text(payload)
        if not args.forever and sample >= args.samples:
            break
        next_start += args.interval
        wait = next_start - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        else:
            next_start = time.monotonic()
    return EXIT_OK


def _close_preserving_error(
    client: ModbusClient | None,
    logger: FrameLogger | None,
) -> None:
    if client is not None:
        try:
            client.close()
        except Exception:
            pass
    if logger is not None:
        try:
            logger.close()
        except Exception:
            pass


def _execute(
    args: argparse.Namespace,
    transport_factory: TransportFactory,
) -> int:
    started = time.monotonic()
    logger: FrameLogger | None = None
    client: ModbusClient | None = None
    try:
        if args.frame_log:
            logger = FrameLogger(args.frame_log, f"{args.port}:9600:8E1")
        transport = transport_factory(args, logger)
        client = ModbusClient(
            transport,
            timeout_seconds=args.timeout,
            logger=logger,
        )
        client.open()
        service = ModbusService(client, address=args.address)
        if args.command == "watch":
            result_code = _run_watch(args, service)
            client.close()
            client = None
            if logger is not None:
                logger.close()
                logger = None
            return result_code
        result = _dispatch(args, service)
        elapsed_ms = (time.monotonic() - started) * 1000.0
        payload = _envelope(args, result, None, elapsed_ms)
        client.close()
        client = None
        if logger is not None:
            logger.close()
            logger = None
        if args.json_output:
            _emit_json(payload)
        else:
            _emit_text(payload)
        return EXIT_OK
    except KeyboardInterrupt:
        _close_preserving_error(client, logger)
        payload = _envelope(
            args,
            {},
            ArgumentError("interrupted by user"),
            (time.monotonic() - started) * 1000.0,
        )
        payload["error"] = {
            "kind": "interrupted",
            "message": "interrupted by user",
            "function": None,
            "exception_code": None,
        }
        if args.json_output:
            _emit_json(payload)
        else:
            print("interrupted", file=sys.stderr)
        return EXIT_INTERRUPTED
    except ModbusClientError as exc:
        _close_preserving_error(client, logger)
        result = _partial_result(exc)
        if isinstance(exc, ModbusException) and exc.observation is not None:
            result["observation"] = exc.observation
        payload = _envelope(
            args,
            result,
            exc,
            (time.monotonic() - started) * 1000.0,
        )
        if args.json_output:
            _emit_json(payload)
        else:
            _emit_text(payload)
        return exc.exit_code
    except ValueError as exc:
        _close_preserving_error(client, logger)
        error = ArgumentError(str(exc))
        payload = _envelope(
            args,
            {},
            error,
            (time.monotonic() - started) * 1000.0,
        )
        if args.json_output:
            _emit_json(payload)
        else:
            _emit_text(payload)
        return error.exit_code
    except OSError as exc:
        _close_preserving_error(client, logger)
        error = FrameLogError(str(exc))
        payload = _envelope(
            args,
            {},
            error,
            (time.monotonic() - started) * 1000.0,
        )
        if args.json_output:
            _emit_json(payload)
        else:
            _emit_text(payload)
        return EXIT_TRANSPORT


def main(
    argv: list[str] | None = None,
    transport_factory: TransportFactory | None = None,
) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        _validate_business_arguments(args)
    except ArgumentError as exc:
        if args.json_output:
            _emit_json(_envelope(args, {}, exc, 0.0))
        else:
            _emit_text(_envelope(args, {}, exc, 0.0))
        return exc.exit_code

    if transport_factory is None:

        def transport_factory(
            args: argparse.Namespace,
            logger: FrameLogger | None,
        ) -> Transport:
            return MacOSTTYTransport(args.port, logger=logger)

    return _execute(args, transport_factory)


if __name__ == "__main__":
    sys.exit(main())
