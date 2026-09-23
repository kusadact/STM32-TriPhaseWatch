"""Synthetic backend for ``python3 -m tools.modbus_gui --demo``.

Lets the GUI (and the three-phase temperature trend in particular) be reviewed
on a machine with no hardware attached.  The generator produces smooth A/B/C
temperatures that drift and cross each other, and a benign "no alarm" block so
the alarm panel stays quiet.
"""

from __future__ import annotations

import math
import random
import time
from typing import Any, Callable

DEMO_PORT = "demo"
DEMO_VERSION = "1.10.0-demo"

DEMO_IDENTITY = {
    "device_type": 1,
    "reported_version": DEMO_VERSION,
    "protocol_version": 3,
}
DEMO_STORAGE = {
    "storage_state_name": "READY",
    "storage_error_name": "NONE",
    "generated": 0,
    "synced": 0,
    "dropped": 0,
    "event_dropped": 0,
    "uncertain": 0,
    "queued": 0,
    "in_flight": 0,
}
DEMO_NO_ALARM = {
    "contract_revision": 1,
    "flags": {
        "valid": False,
        "latched": False,
        "acknowledged": False,
        "buzzer_active": False,
    },
    "phases": [],
}

_PHASE_BASE_C = (26.0, 27.4, 25.2)
_PHASE_SWING_C = (2.2, 3.4, 1.4)
_PHASE_OFFSET = (0.0, 0.9, 2.1)
_CYCLE_SAMPLES = 24


class DemoBackend:
    """GuiBackend-compatible generator that never opens a serial port."""

    def __init__(
        self,
        *,
        clock: Callable[[], float] = time.monotonic,
        noise_c: float = 0.05,
        seed: int = 20260923,
    ) -> None:
        self._clock = clock
        self._noise_c = noise_c
        self._random = random.Random(seed)
        self._sample_index = 0
        self._last_alarm = dict(DEMO_NO_ALARM)

    def connect(self, port: str, address: int) -> dict[str, Any]:
        self._sample_index = 0
        return {"identity": dict(DEMO_IDENTITY), "status": self._status()}

    def disconnect(self) -> None:
        return None

    def close(self) -> None:
        return None

    def poll(self) -> dict[str, Any]:
        snapshot, alarm = self._sample()
        self._sample_index += 1
        return {
            "status": self._status(),
            "temperature_snapshot": snapshot,
            "thermal_alarm": alarm,
            "sensor_error": None,
            "alarm_error": None,
        }

    def start_periodic(self, period_sec: int) -> dict[str, Any]:
        return {"pending": False, "period_sec": period_sec}

    def stop_periodic(self) -> dict[str, Any]:
        return {"drain_waited": True, "safe_to_remove": True}

    def single_sample(self, command_id: int) -> dict[str, Any]:
        snapshot, alarm = self._sample()
        self._sample_index += 1
        return {
            "command": {"accepted": True, "command_id": command_id},
            "temperature_snapshot": snapshot,
            "thermal_alarm": alarm,
            "sensor_error": None,
            "alarm_error": None,
        }

    def ack_alarm(self) -> dict[str, Any]:
        payload = {
            **self._last_alarm,
            "flags": {
                **self._last_alarm["flags"],
                "acknowledged": True,
            },
        }
        return {
            "acknowledged": True,
            "thermal_alarm": payload,
            "alarm": payload,
        }

    def refresh_storage(self) -> dict[str, Any]:
        return dict(DEMO_STORAGE)

    @staticmethod
    def _status() -> dict[str, Any]:
        return {
            "run_state": "RUNNING",
            "run_state_code": 1,
            "active_config": {
                "valid": True,
                "period_sec": 30,
                "channel_mask": 0x0001,
                "record_count": 0,
            },
        }

    def _sample(self) -> tuple[dict[str, Any], dict[str, Any]]:
        phase = (
            math.tau
            * (self._sample_index % _CYCLE_SAMPLES)
            / _CYCLE_SAMPLES
        )
        sensors = []
        for index in range(3):
            value_c = (
                _PHASE_BASE_C[index]
                + _PHASE_SWING_C[index]
                * math.sin(phase + _PHASE_OFFSET[index])
                + self._random.uniform(-self._noise_c, self._noise_c)
            )
            sensors.append(
                {
                    "sensor_id": index,
                    "temperature_x16": int(round(value_c * 16.0)),
                    "quality": "OK",
                    "rom_short": 0x1200 + index,
                    "sample_time": int(self._clock() * 1000.0),
                    "source": "REAL_DS18B20",
                }
            )
        sample_id = self._sample_index + 1
        snapshot = {
            "sample_id": sample_id,
            "source": "REAL_DS18B20",
            "sensors": sensors,
        }
        alarm = self._alarm_payload(sample_id, sensors)
        self._last_alarm = alarm
        return snapshot, alarm

    @staticmethod
    def _alarm_payload(
        sample_id: int,
        sensors: list[dict[str, Any]],
    ) -> dict[str, Any]:
        temperatures = [sensor["temperature_x16"] for sensor in sensors]
        hottest_index = max(
            range(len(temperatures)),
            key=lambda index: temperatures[index],
        )
        return {
            "contract_revision": 1,
            "level": "NORMAL",
            "reason": "NONE",
            "trigger_phase": "NONE",
            "hottest_phase": ("A", "B", "C")[hottest_index],
            "delta_valid": True,
            "maximum_delta_x16": max(temperatures) - min(temperatures),
            "hottest_temperature_x16": max(temperatures),
            "phases": [
                {
                    "phase": ("A", "B", "C")[index],
                    "temperature_x16": temperature,
                    "quality": "OK",
                }
                for index, temperature in enumerate(temperatures)
            ],
            "flags": {
                "valid": True,
                "latched": False,
                "acknowledged": False,
                "buzzer_active": False,
            },
            "event_id": 1,
            "alarm_sample_id": sample_id,
            "duration_sec": 0,
            "notice_count": 0,
            "warning_count": 0,
            "critical_count": 0,
            "sensor_fault_count": 0,
        }
