#!/usr/bin/env python3

import argparse
import os
import select
import sys
import termios
import time
import tty


def parse_arguments():
    parser = argparse.ArgumentParser(description="Print STM32 USART1 text output.")
    parser.add_argument("port", help="Serial device path (for example, a USB serial port)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=0.0,
                        help="Stop after this many seconds; 0 means run until Ctrl-C.")
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    fd = os.open(arguments.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original = termios.tcgetattr(fd)
    settings = termios.tcgetattr(fd)
    settings[4] = arguments.baud
    settings[5] = arguments.baud
    termios.tcsetattr(fd, termios.TCSANOW, settings)
    tty.setraw(fd, termios.TCSANOW)

    print(f"listening on {arguments.port} at {arguments.baud} baud", file=sys.stderr)
    pending = bytearray()
    deadline = time.monotonic() + arguments.seconds if arguments.seconds > 0 else None

    try:
        while deadline is None or time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.2)
            if not ready:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            pending.extend(chunk)

            while b"\n" in pending:
                line, _, remainder = pending.partition(b"\n")
                pending = bytearray(remainder)
                text = line.decode("ascii", errors="replace").rstrip("\r")
                if text:
                    print(text)
        if pending:
            print(pending.decode("ascii", errors="replace").rstrip("\r"))
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, original)
        os.close(fd)


if __name__ == "__main__":
    main()
