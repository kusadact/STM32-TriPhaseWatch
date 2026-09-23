"""Deterministic fake backend for GUI controller tests."""

from __future__ import annotations

from collections import deque
from copy import deepcopy
import threading
import time
from typing import Any


def _take(queue: deque[Any], default: Any) -> Any:
    if not queue:
        return deepcopy(default)
    outcome = queue.popleft()
    if isinstance(outcome, BaseException):
        raise outcome
    return deepcopy(outcome)


class FakeBackend:
    def __init__(self) -> None:
        self.events: list[tuple[Any, ...]] = []
        self._events_lock = threading.Lock()
        self.connected = False
        self.connect_results: deque[Any] = deque()
        self.poll_results: deque[Any] = deque()
        self.storage_results: deque[Any] = deque()
        self.start_results: deque[Any] = deque()
        self.stop_results: deque[Any] = deque()
        self.single_results: deque[Any] = deque()
        self.ack_results: deque[Any] = deque()
        self.identity_payload = {
            "device_type": 1,
            "reported_version": "1.8.0",
            "protocol_version": 3,
        }
        self.status_payload = {
            "run_state": "STOPPED",
            "run_state_code": 0,
            "active_config": {
                "valid": True,
                "period_sec": 10,
                "channel_mask": 0x0001,
                "record_count": 0,
            },
        }
        self.temperature_payload: Any = {
            "sample_id": 1,
            "source": "REAL_DS18B20",
            "sensors": [],
        }
        self.alarm_payload: Any = None
        self.alarm_error: Any = None
        self.sensor_error: Any = None
        self.storage_payload: Any = {
            "storage_state_name": "READY",
            "storage_error_name": "NONE",
            "generated": 0,
            "synced": 0,
            "dropped": 0,
            "uncertain": 0,
            "queued": 0,
            "in_flight": 0,
            "last_synced_file": 0,
            "last_synced_date": 0,
            "last_synced_seq": 0,
        }
        self.close_delay = 0.0
        self.poll_delay = 0.0
        self.raise_on_close: BaseException | None = None

    def _record(self, name: str, *args: Any) -> None:
        with self._events_lock:
            self.events.append((name, *args))

    def connect(self, port: str, address: int) -> dict[str, Any]:
        self._record("connect", port, address)
        self.connected = False
        result = _take(
            self.connect_results,
            {
                "identity": self.identity_payload,
                "status": self.status_payload,
            },
        )
        self.connected = True
        return result

    def disconnect(self) -> None:
        self._record("disconnect")
        self.connected = False

    def poll(self) -> dict[str, Any]:
        self._record("poll")
        if self.poll_delay > 0:
            time.sleep(self.poll_delay)
        return _take(
            self.poll_results,
            {
                "status": self.status_payload,
                "temperature_snapshot": self.temperature_payload,
                "thermal_alarm": self.alarm_payload,
                "sensor_error": self.sensor_error,
                "alarm_error": self.alarm_error,
            },
        )

    def refresh_storage(self) -> dict[str, Any]:
        self._record("refresh_storage")
        return _take(self.storage_results, self.storage_payload)

    def start_periodic(self, period_sec: int) -> dict[str, Any]:
        self._record("start_periodic", period_sec)
        return _take(
            self.start_results,
            {
                "pending": False,
                "period_sec": period_sec,
                "observation": {
                    "run_state": "RUNNING",
                    "run_state_code": 1,
                    "last_command": {
                        "code": 3,
                        "result_code": 1,
                        "result": "ACCEPTED",
                    },
                },
            },
        )

    def stop_periodic(self) -> dict[str, Any]:
        self._record("stop_periodic")
        return _take(
            self.stop_results,
            {
                "run_state": "STOPPED",
                "run_state_code": 0,
                "last_command": {
                    "code": 4,
                    "result_code": 1,
                    "result": "ACCEPTED",
                },
            },
        )

    def single_sample(self, command_id: int) -> dict[str, Any]:
        self._record("single_sample", command_id)
        return _take(
            self.single_results,
            {
                "command": {
                    "run_state": "STOPPED",
                    "run_state_code": 0,
                    "last_command": {
                        "code": 5,
                        "result_code": 1,
                        "result": "ACCEPTED",
                    },
                },
                "temperature_snapshot": self.temperature_payload,
                "thermal_alarm": self.alarm_payload,
                "sensor_error": self.sensor_error,
                "alarm_error": self.alarm_error,
            },
        )

    def ack_alarm(self) -> dict[str, Any]:
        self._record("ack_alarm")
        return _take(
            self.ack_results,
            {
                "command_id": 1,
                "duplicate": False,
                "thermal_alarm": self.alarm_payload,
                "acknowledged": bool(
                    isinstance(self.alarm_payload, dict)
                    and self.alarm_payload.get("acknowledged", False)
                ),
            },
        )

    def close(self) -> None:
        self._record("close")
        if self.close_delay > 0:
            time.sleep(self.close_delay)
        if self.raise_on_close is not None:
            raise self.raise_on_close
        self.connected = False
