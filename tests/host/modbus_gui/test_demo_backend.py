from __future__ import annotations

import unittest

from tools.modbus_gui.demo import DEMO_PORT, DemoBackend
from tools.modbus_gui.model import AlarmSnapshot, TemperatureSnapshot


class DemoBackendTests(unittest.TestCase):
    def test_poll_generates_moving_three_phase_snapshots(self) -> None:
        backend = DemoBackend(clock=lambda: 0.0, noise_c=0.0)
        connected = backend.connect(DEMO_PORT, 1)

        self.assertEqual(connected["identity"]["protocol_version"], 3)
        self.assertEqual(connected["status"]["run_state_code"], 1)

        samples: list[tuple[int | None, ...]] = []
        for _ in range(8):
            result = backend.poll()
            snapshot = TemperatureSnapshot.from_payload(
                result["temperature_snapshot"]
            )
            self.assertEqual(len(snapshot.sensors), 3)
            samples.append(
                tuple(reading.temperature_x16 for reading in snapshot.sensors)
            )
            alarm = AlarmSnapshot.from_payload(result["thermal_alarm"])
            self.assertTrue(alarm.valid)
            self.assertEqual(alarm.level.value, "NORMAL")
            self.assertEqual(alarm.alarm_sample_id, snapshot.sample_id)
            self.assertEqual(
                alarm.maximum_delta_x16,
                max(samples[-1]) - min(samples[-1]),
            )

        self.assertEqual(len(set(samples)), len(samples))

    def test_ack_reports_acknowledged_normal_alarm(self) -> None:
        backend = DemoBackend()
        backend.poll()
        result = backend.ack_alarm()
        alarm = AlarmSnapshot.from_payload(result["thermal_alarm"])

        self.assertTrue(alarm.acknowledged)
        self.assertTrue(alarm.valid)
        self.assertEqual(alarm.level.value, "NORMAL")

    def test_single_sample_and_storage_stay_consistent(self) -> None:
        backend = DemoBackend(clock=lambda: 12.5)
        result = backend.single_sample(7)
        snapshot = TemperatureSnapshot.from_payload(
            result["temperature_snapshot"]
        )

        self.assertEqual(result["command"]["command_id"], 7)
        self.assertEqual(
            {reading.sensor_id for reading in snapshot.sensors},
            {0, 1, 2},
        )
        self.assertEqual(backend.refresh_storage()["storage_state_name"], "READY")


if __name__ == "__main__":
    unittest.main()
