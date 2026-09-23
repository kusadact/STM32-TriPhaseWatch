from __future__ import annotations

from datetime import datetime
import unittest

from tools.modbus_gui.model import (
    AcquisitionPhase,
    AlarmLevel,
    AlarmReason,
    AlarmSnapshot,
    ConnectionState,
    GuiState,
    Phase,
    Quality,
    SensorStatistics,
    SnapshotFormatError,
    StorageSnapshot,
    TemperatureSnapshot,
)


def sensor(
    sensor_id: int,
    temperature_x16: int | None,
    quality: str,
    *,
    rom_short: int | None = None,
    sample_time: int | None = None,
) -> dict[str, object]:
    return {
        "sensor_id": sensor_id,
        "temperature_x16": temperature_x16,
        "quality": quality,
        "rom_short": rom_short,
        "sample_time": sample_time,
        "source": "REAL_DS18B20",
    }


def alarm_payload(
    *,
    level: str = "NOTICE",
    reason: str = "PHASE_DELTA_HIGH",
    flags: dict[str, bool] | None = None,
    delta_valid: bool = True,
    maximum_delta_x16: int | None = 160,
    trigger_phase: str = "A",
    hottest_phase: str = "A",
    hottest_temperature_x16: int | None = 480,
    temperatures: tuple[int | None, int | None, int | None] = (480, 320, 320),
    qualities: tuple[str, str, str] = ("OK", "OK", "OK"),
) -> dict[str, object]:
    if flags is None:
        flags = {
            "valid": True,
            "latched": True,
            "acknowledged": False,
            "buzzer_active": True,
        }
    return {
        "contract_revision": 1,
        "valid": flags["valid"],
        "level": level,
        "reason": reason,
        "flags": flags,
        "trigger_phase": trigger_phase,
        "delta_valid": delta_valid,
        "maximum_delta_x16": maximum_delta_x16,
        "hottest_temperature_x16": hottest_temperature_x16,
        "hottest_phase": hottest_phase,
        "phases": [
            {
                "phase": phase,
                "temperature_x16": temperatures[index],
                "quality": qualities[index],
            }
            for index, phase in enumerate(("A", "B", "C"))
        ],
        "event_id": 3,
        "alarm_sample_id": 99,
        "duration_sec": 12,
        "notice_count": 1,
        "warning_count": 0,
        "critical_count": 0,
        "sensor_fault_count": 0,
    }


class ModelTests(unittest.TestCase):
    def test_alarm_snapshot_maps_levels_phases_and_colors(self) -> None:
        received = datetime(2026, 9, 23, 10, 0, 12)
        alarm = AlarmSnapshot.from_payload(
            alarm_payload(level="WARNING"),
            received_at=received,
        )

        self.assertEqual(alarm.level, AlarmLevel.WARNING)
        self.assertEqual(alarm.reason, AlarmReason.PHASE_DELTA_HIGH)
        self.assertEqual(alarm.trigger_phase, Phase.A)
        self.assertEqual(alarm.maximum_delta_text, "10.00 °C")
        self.assertEqual(alarm.hottest_temperature_text, "30.00 °C")
        self.assertEqual(alarm.buzzer_text, "蜂鸣中")
        self.assertEqual(alarm.acknowledged_text, "未确认")
        self.assertEqual(alarm.event_time_text, "2026-09-23 10:00:00")
        self.assertEqual(alarm.color, "#f97316")

    def test_alarm_normal_is_green(self) -> None:
        alarm = AlarmSnapshot.from_payload(
            alarm_payload(
                level="NORMAL",
                reason="NONE",
                maximum_delta_x16=0,
                trigger_phase="NONE",
                hottest_phase="A",
                hottest_temperature_x16=400,
                temperatures=(400, 400, 400),
            )
        )

        self.assertEqual(alarm.level, AlarmLevel.NORMAL)
        self.assertEqual(alarm.reason, AlarmReason.NONE)
        self.assertEqual(alarm.color, "#15803d")

    def test_alarm_snapshot_rejects_fault_delta_and_hides_invalid_temperature(
        self,
    ) -> None:
        payload = alarm_payload(
            level="SENSOR_FAULT",
            reason="SENSOR_CRC_ERROR",
            trigger_phase="B",
            qualities=("OK", "CRC_ERROR", "OK"),
            temperatures=(480, 0, 320),
        )
        with self.assertRaises(SnapshotFormatError):
            AlarmSnapshot.from_payload(payload)

        payload["delta_valid"] = False
        payload["maximum_delta_x16"] = None
        payload["hottest_phase"] = "NONE"
        payload["hottest_temperature_x16"] = None
        alarm = AlarmSnapshot.from_payload(payload)
        self.assertEqual(alarm.phases[1].temperature_text, "--")
        self.assertEqual(alarm.maximum_delta_text, "--")
        self.assertEqual(alarm.level, AlarmLevel.SENSOR_FAULT)

    def test_gui_state_preserves_event_time_for_same_event_id(self) -> None:
        state = GuiState()
        first = AlarmSnapshot.from_payload(
            alarm_payload(),
            received_at=datetime(2026, 9, 23, 10, 0, 12),
        )
        state.set_alarm(first)
        event_time = state.alarm.event_time

        later = AlarmSnapshot.from_payload(
            alarm_payload(),
            received_at=datetime(2026, 9, 23, 10, 0, 20),
        )
        state.set_alarm(later)

        self.assertEqual(state.alarm.event_time, event_time)

    def test_three_valid_sensors_drive_cards_and_statistics(self) -> None:
        received = datetime(2026, 9, 22, 10, 11, 12)
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 77,
                "source": "REAL_DS18B20",
                "sensors": [
                    sensor(0, 320, "OK", rom_short=0x1200, sample_time=1000),
                    sensor(1, 400, "OK", rom_short=0x1201, sample_time=1001),
                    sensor(2, 480, "OK", rom_short=0x1202, sample_time=1002),
                ],
            },
            received_at=received,
        )

        self.assertEqual(snapshot.sample_id, 77)
        self.assertEqual(snapshot.sensors[0].temperature_text, "20.00 °C")
        self.assertEqual(snapshot.sensors[1].temperature_text, "25.00 °C")
        self.assertEqual(snapshot.sensors[1].rom_text, "0x1201")
        self.assertEqual(snapshot.sensors[2].updated_at_text, "2026-09-22 10:11:12")

        statistics = SensorStatistics.calculate(snapshot)
        self.assertEqual(statistics.valid_sensor_ids, (0, 1, 2))
        self.assertEqual(statistics.minimum_temperature_text, "20.00 °C")
        self.assertEqual(statistics.maximum_temperature_text, "30.00 °C")
        self.assertEqual(statistics.median_temperature_text, "25.00 °C")
        self.assertEqual(statistics.maximum_temperature_delta_text, "10.00 °C")
        self.assertEqual(statistics.valid_count_text, "3 / 3")
        self.assertEqual(
            statistics.participating_sensor_text,
            "参与计算: DS18B20-0, DS18B20-1, DS18B20-2",
        )

    def test_logical_sensor_ids_ignore_payload_order(self) -> None:
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 78,
                "source": "REAL_DS18B20",
                "sensors": [
                    sensor(2, 480, "OK", rom_short=0x1202),
                    sensor(0, 320, "OK", rom_short=0x1200),
                    sensor(1, 400, "OK", rom_short=0x1201),
                ],
            }
        )

        self.assertEqual(
            [reading.sensor_id for reading in snapshot.sensors],
            [0, 1, 2],
        )
        self.assertEqual(
            [reading.temperature_text for reading in snapshot.sensors],
            ["20.00 °C", "25.00 °C", "30.00 °C"],
        )
        self.assertEqual(
            [reading.rom_text for reading in snapshot.sensors],
            ["0x1200", "0x1201", "0x1202"],
        )

    def test_negative_temperature_and_missing_rom_are_displayed_honestly(
        self,
    ) -> None:
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 79,
                "source": "REAL_DS18B20",
                "sensors": [sensor(0, -160, "OK")],
            }
        )

        self.assertEqual(snapshot.sensors[0].temperature_text, "-10.00 °C")
        self.assertEqual(snapshot.sensors[0].rom_text, "--")

    def test_error_quality_never_displays_the_zero_value(self) -> None:
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 1,
                "sensors": [
                    sensor(0, 0, "CRC_ERROR", sample_time=1),
                    sensor(1, 0, "NOT_PRESENT", sample_time=2),
                    sensor(2, 0, "RANGE_ERROR", sample_time=3),
                ],
            }
        )

        self.assertEqual(snapshot.sensors[0].quality, Quality.CRC_ERROR)
        self.assertEqual(snapshot.sensors[0].temperature_text, "--")
        self.assertEqual(snapshot.sensors[0].quality_label, "校验失败")
        self.assertEqual(snapshot.sensors[1].quality_label, "未接入")
        self.assertEqual(snapshot.sensors[2].temperature_text, "--")
        self.assertFalse(SensorStatistics.calculate(snapshot).has_valid_samples)

    def test_missing_sensor_is_filled_as_not_present(self) -> None:
        snapshot = TemperatureSnapshot.from_payload(
            {
                "sample_id": 2,
                "sensors": [sensor(0, 210, "OK")],
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
                "sensors": [sensor(0, 352, "OK", rom_short=0x1200)],
            }
        )
        stale = snapshot.as_stale()

        self.assertEqual(stale.sensors[0].temperature_text, "22.00 °C")
        self.assertEqual(stale.sensors[0].quality_label, "旧值")
        self.assertFalse(SensorStatistics.calculate(stale).has_valid_samples)

    def test_ok_without_temperature_is_rejected(self) -> None:
        with self.assertRaises(SnapshotFormatError):
            TemperatureSnapshot.from_payload(
                {
                    "sample_id": 4,
                    "sensors": [sensor(0, None, "OK")],
                }
            )

    def test_rom_short_must_fit_in_16_bits(self) -> None:
        with self.assertRaises(SnapshotFormatError):
            TemperatureSnapshot.from_payload(
                {
                    "sample_id": 4,
                    "sensors": [sensor(0, 200, "OK", rom_short=0x10000)],
                }
            )

    def test_test_source_is_not_displayed_as_environmental_data(self) -> None:
        with self.assertRaises(SnapshotFormatError):
            TemperatureSnapshot.from_payload(
                {
                    "sample_id": 4,
                    "source": "TEST",
                    "sensors": [sensor(0, 200, "OK")],
                }
            )

    def test_legacy_dht11_source_is_rejected(self) -> None:
        with self.assertRaises(SnapshotFormatError):
            TemperatureSnapshot.from_payload(
                {
                    "sample_id": 4,
                    "source": "REAL_DHT11",
                    "sensors": [sensor(0, 200, "OK")],
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
                        "temperature_x16": None,
                        "quality": "NOT_PRESENT",
                        "rom_short": 0,
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
        self.assertEqual(
            [sensor.rom_text for sensor in snapshot.sensors],
            ["--", "--", "--"],
        )
        self.assertFalse(SensorStatistics.calculate(snapshot).has_valid_samples)

    def test_storage_snapshot_preserves_raw_counts_and_missing_fields(self) -> None:
        storage = StorageSnapshot.from_payload(
            {
                "generated": 40,
                "synced": 31,
                "dropped": 0,
                "event_dropped": 3,
                "queued": 8,
            }
        )

        self.assertEqual(storage.text("generated"), "40")
        self.assertEqual(storage.text("synced"), "31")
        self.assertEqual(storage.text("event_dropped"), "3")
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
