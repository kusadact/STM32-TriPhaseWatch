from __future__ import annotations

from datetime import datetime
import unittest

from tools.modbus_gui.model import (
    AcquisitionPhase,
    ConnectionState,
    GuiState,
    Quality,
    SensorStatistics,
    SnapshotFormatError,
    StorageSnapshot,
    TemperatureSnapshot,
)


def sensor(
    sensor_id: int,
    temperature_x10: int | None,
    humidity_x10: int | None,
    quality: str,
    sample_time: int | None = None,
) -> dict[str, object]:
    return {
        "sensor_id": sensor_id,
        "temperature_x10": temperature_x10,
        "humidity_x10": humidity_x10,
        "quality": quality,
        "sample_time": sample_time,
        "source": "DHT11",
    }


class ModelTests(unittest.TestCase):
    def test_three_valid_sensors_drive_cards_and_statistics(self) -> None:
        received = datetime(2026, 9, 22, 10, 11, 12)
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 77,
                "source": "DHT11",
                "sensors": [
                    sensor(0, 200, 600, "OK", 1000),
                    sensor(1, 250, 550, "OK", 1001),
                    sensor(2, 300, 500, "OK", 1002),
                ],
            },
            received_at=received,
        )

        self.assertEqual(snapshot.sample_id, 77)
        self.assertEqual(snapshot.sensors[0].temperature_text, "20.0 °C")
        self.assertEqual(snapshot.sensors[1].humidity_text, "55.0 %RH")
        self.assertEqual(snapshot.sensors[2].updated_at_text, "2026-09-22 10:11:12")

        statistics = SensorStatistics.calculate(snapshot)
        self.assertEqual(statistics.valid_sensor_ids, (0, 1, 2))
        self.assertEqual(statistics.minimum_temperature_text, "20.0 °C")
        self.assertEqual(statistics.maximum_temperature_text, "30.0 °C")
        self.assertEqual(statistics.median_temperature_text, "25.0 °C")
        self.assertEqual(statistics.maximum_temperature_delta_text, "10.0 °C")
        self.assertEqual(statistics.minimum_humidity_text, "50.0 %RH")
        self.assertEqual(statistics.maximum_humidity_text, "60.0 %RH")
        self.assertEqual(statistics.valid_count_text, "3 / 3")

    def test_error_quality_never_displays_the_zero_value(self) -> None:
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 1,
                "sensors": [
                    sensor(0, 0, 0, "CHECKSUM_ERROR", 1),
                    sensor(1, 0, 0, "NOT_PRESENT", 2),
                    sensor(2, 0, 0, "RANGE_ERROR", 3),
                ],
            }
        )

        self.assertEqual(snapshot.sensors[0].quality, Quality.CHECKSUM_ERROR)
        self.assertEqual(snapshot.sensors[0].temperature_text, "--")
        self.assertEqual(snapshot.sensors[0].humidity_text, "--")
        self.assertEqual(snapshot.sensors[1].quality_label, "未接入")
        self.assertFalse(SensorStatistics.calculate(snapshot).has_valid_samples)

    def test_missing_sensor_is_filled_as_not_present(self) -> None:
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 2,
                "sensors": [sensor(0, 210, 610, "OK")],
            }
        )

        self.assertEqual(snapshot.sensors[1].quality, Quality.NOT_PRESENT)
        self.assertEqual(snapshot.sensors[2].quality, Quality.NOT_PRESENT)
        self.assertEqual(snapshot.sensors[2].temperature_text, "--")
        self.assertEqual(
            SensorStatistics.calculate(snapshot).valid_sensor_ids,
            (0,),
        )
        self.assertEqual(
            SensorStatistics.calculate(snapshot).maximum_temperature_delta_text,
            "--",
        )

    def test_stale_snapshot_keeps_values_but_excludes_statistics(self) -> None:
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 3,
                "sensors": [sensor(0, 220, 620, "OK")],
            }
        )
        stale = snapshot.as_stale()

        self.assertEqual(stale.sensors[0].temperature_text, "22.0 °C")
        self.assertEqual(stale.sensors[0].quality_label, "旧值")
        self.assertFalse(SensorStatistics.calculate(stale).has_valid_samples)

    def test_ok_without_both_values_is_rejected(self) -> None:
        with self.assertRaises(SnapshotFormatError):
            TemperatureSnapshot.from_payload(
                {
                    "sample_id": 4,
                    "sensors": [sensor(0, 200, None, "OK")],
                }
            )

    def test_test_source_is_not_displayed_as_environmental_data(self) -> None:
        with self.assertRaises(SnapshotFormatError):
            TemperatureSnapshot.from_payload(
                {
                    "sample_id": 4,
                    "source": "TEST",
                    "sensors": [sensor(0, 200, 600, "OK")],
                }
            )

    def test_source_none_accepts_honest_not_present_cards(self) -> None:
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 0,
                "source": "NONE",
                "sensors": [
                    {
                        "sensor_id": index,
                        "temperature_x10": None,
                        "humidity_x10": None,
                        "quality": "NOT_PRESENT",
                        "sample_time": 100 + index,
                        "source": "NONE",
                    }
                    for index in range(3)
                ],
            }
        )

        self.assertEqual(snapshot.source, "NONE")
        self.assertEqual(
            [sensor.quality_label for sensor in snapshot.sensors],
            ["未接入", "未接入", "未接入"],
        )
        self.assertFalse(SensorStatistics.calculate(snapshot).has_valid_samples)

    def test_storage_snapshot_preserves_raw_counts_and_missing_fields(self) -> None:
        storage = StorageSnapshot.from_payload(
            {
                "generated": 40,
                "synced": 31,
                "dropped": 0,
                "queued": 8,
            }
        )

        self.assertEqual(storage.text("generated"), "40")
        self.assertEqual(storage.text("synced"), "31")
        self.assertEqual(storage.text("queued"), "8")
        self.assertEqual(storage.text("uncertain"), "--")

    def test_action_availability_tracks_connection_and_acquisition(self) -> None:
        state = GuiState()
        self.assertTrue(state.availability().connect)
        self.assertFalse(state.availability().start_periodic)

        state.connection = ConnectionState.CONNECTED
        state.session_open = True
        state.acquisition = AcquisitionPhase.STOPPED
        self.assertTrue(state.availability().start_periodic)
        self.assertTrue(state.availability().single_sample)
        self.assertFalse(state.availability().stop_periodic)

        state.acquisition = AcquisitionPhase.STARTING
        self.assertFalse(state.availability().start_periodic)
        self.assertFalse(state.availability().single_sample)
        self.assertTrue(state.availability().stop_periodic)


if __name__ == "__main__":
    unittest.main()
