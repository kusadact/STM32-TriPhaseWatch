"""UTC value parsing and 32-bit register serialization for protocol 2."""

from __future__ import annotations

from datetime import datetime, timezone
import re

MAX_UTC_SECONDS = 0xFFFFFFFE
INVALID_UTC_SECONDS = 0xFFFFFFFF
_EPOCH_RE = re.compile(r"[0-9]+")
_HEX_EPOCH_RE = re.compile(r"0[xX][0-9a-fA-F]+")
_UNIX_EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)


def parse_utc_seconds(value: str) -> int:
    """Parse an explicit epoch or a timezone-aware ISO-8601 timestamp."""

    text = value.strip()
    if not text:
        raise ValueError("UTC value must not be empty")

    if text.lower().startswith("epoch:"):
        number = text[6:]
        if not _EPOCH_RE.fullmatch(number):
            raise ValueError("epoch UTC value must be an unsigned decimal integer")
        seconds = int(number, 10)
    elif _EPOCH_RE.fullmatch(text):
        seconds = int(text, 10)
    elif _HEX_EPOCH_RE.fullmatch(text):
        seconds = int(text, 16)
    else:
        iso_text = text
        if iso_text.endswith(("Z", "z")):
            iso_text = iso_text[:-1] + "+00:00"
        try:
            parsed = datetime.fromisoformat(iso_text)
        except ValueError as exc:
            raise ValueError(
                "UTC value must be an ISO-8601 timestamp with a timezone "
                "or an explicit decimal/0x epoch"
            ) from exc
        if parsed.tzinfo is None or parsed.utcoffset() is None:
            raise ValueError("UTC ISO-8601 value must include a timezone")
        delta = parsed.astimezone(timezone.utc) - _UNIX_EPOCH
        seconds = (delta.days * 86400) + delta.seconds

    if not 0 <= seconds <= MAX_UTC_SECONDS:
        raise ValueError(
            f"UTC seconds must be in 0..{MAX_UTC_SECONDS} "
            f"(0x{MAX_UTC_SECONDS:08X}); 0x{INVALID_UTC_SECONDS:08X} is invalid"
        )
    return seconds


def split_u32(value: int) -> tuple[int, int]:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError("UTC seconds must be an integer")
    if not 0 <= value <= MAX_UTC_SECONDS:
        raise ValueError(
            f"UTC seconds must be in 0..{MAX_UTC_SECONDS} "
            f"(0x{MAX_UTC_SECONDS:08X}); 0x{INVALID_UTC_SECONDS:08X} is invalid"
        )
    return (value >> 16) & 0xFFFF, value & 0xFFFF
