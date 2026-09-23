from __future__ import annotations

import threading
import time
import unittest

from tools.modbus_client.errors import (
    StateError,
    TransactionTimeout,
    TransportError,
)
from tools.modbus_gui.controller import GuiController
from tools.modbus_gui.model import AcquisitionPhase, ConnectionState
from fake_backend import FakeBackend


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


def alarm_payload(*, acknowledged: bool = False) -> dict[str, object]:
    return {
        "contract_revision": 1,
        "valid": True,
        "level": "NOTICE",
        "reason": "PHASE_DELTA_HIGH",
        "flags": {
            "valid": True,
            "latched": True,
            "acknowledged": acknowledged,
            "buzzer_active": not acknowledged,
        },
        "trigger_phase": "A",
        "delta_valid": True,
        "maximum_delta_x16": 160,
        "hottest_temperature_x16": 480,
        "hottest_phase": "A",
        "phases": [
            {"phase": "A", "temperature_x16": 480, "quality": "OK"},
            {"phase": "B", "temperature_x16": 320, "quality": "OK"},
            {"phase": "C", "temperature_x16": 320, "quality": "OK"},
        ],
        "event_id": 3,
        "alarm_sample_id": 101,
        "duration_sec": 12,
        "notice_count": 1,
        "warning_count": 0,
        "critical_count": 0,
        "sensor_fault_count": 0,
    }


class ControllerTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.backend = FakeBackend()
        self.controller = GuiController(
            self.backend,
            poll_interval=2.0,
            storage_interval=30.0,
            close_timeout=0.5,
        )

    def tearDown(self) -> None:
        self.controller.shutdown(timeout=0.5)

    def wait_until(self, predicate, timeout: float = 1.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.controller.poll()
            if predicate():
                return
            time.sleep(0.001)
        self.fail("condition was not reached before timeout")

    def wait_idle(self, timeout: float = 1.0) -> None:
        self.wait_until(lambda: not self.controller.state.is_busy, timeout)

    def connect(self) -> None:
        self.assertTrue(self.controller.connect("/dev/fake", 1))
        self.assertEqual(
            self.controller.state.connection,
            ConnectionState.CONNECTING,
        )
        self.assertFalse(self.controller.state.session_open)
        self.wait_idle()
        self.assertEqual(
            self.controller.state.connection,
            ConnectionState.CONNECTED,
        )

    def set_three_valid_sensors(self) -> None:
        self.backend.temperature_payload = {
            "sample_id": 101,
            "source": "REAL_DS18B20",
            "sensors": [
                sensor(0, 320, "OK", rom_short=0x1200, sample_time=1000),
                sensor(1, 400, "OK", rom_short=0x1201, sample_time=1001),
                sensor(2, 480, "OK", rom_short=0x1202, sample_time=1002),
            ],
        }


class ControllerTests(ControllerTestCase):
    def test_thermal_alarm_is_rendered_and_acknowledged(self) -> None:
        self.backend.alarm_payload = alarm_payload()
        self.connect()
        self.set_three_valid_sensors()
        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        state = self.controller.state
        self.assertIsNotNone(state.alarm)
        self.assertEqual(state.alarm.level.value, "NOTICE")
        self.assertEqual(state.alarm.maximum_delta_text, "10.00 °C")
        self.assertTrue(state.availability().ack_alarm)

        self.backend.ack_results.append(
            {
                "duplicate": False,
                "command_id": 7,
                "thermal_alarm": alarm_payload(acknowledged=True),
                "acknowledged": True,
            }
        )
        self.assertTrue(self.controller.ack_alarm())
        self.wait_idle()

        self.assertTrue(self.controller.state.alarm.acknowledged)
        self.assertEqual(self.controller.state.alarm.level.value, "NOTICE")
        self.assertEqual(
            self.controller.state.alarm.maximum_delta_text,
            "10.00 °C",
        )
        self.assertFalse(self.controller.state.availability().ack_alarm)
        self.assertIn("ack_alarm", [event[0] for event in self.backend.events])

    def test_all_ok_cards_and_statistics(self) -> None:
        self.connect()
        self.set_three_valid_sensors()

        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        cards = self.controller.state.sensor_cards()
        self.assertEqual(cards[0].temperature_text, "20.00 °C")
        self.assertEqual(cards[1].rom_text, "0x1201")
        self.assertEqual(cards[2].quality_text, "OK")
        self.assertEqual(self.controller.state.last_sample_id, 101)
        self.assertEqual(
            self.controller.state.statistics.median_temperature_text,
            "25.00 °C",
        )
        self.assertEqual(
            self.controller.state.statistics.valid_count_text,
            "3 / 3",
        )
        self.assertEqual(
            [event[0] for event in self.backend.events],
            ["connect", "poll"],
        )

    def test_rom_reorder_keeps_logical_sensor_names(self) -> None:
        self.connect()
        self.backend.temperature_payload = {
            "sample_id": 105,
            "source": "REAL_DS18B20",
            "sensors": [
                sensor(2, 480, "OK", rom_short=0x1202),
                sensor(0, 320, "OK", rom_short=0x1200),
                sensor(1, 400, "OK", rom_short=0x1201),
            ],
        }

        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        cards = self.controller.state.sensor_cards()
        self.assertEqual(
            [card.rom_text for card in cards],
            ["0x1200", "0x1201", "0x1202"],
        )
        self.assertEqual(
            [card.temperature_text for card in cards],
            ["20.00 °C", "25.00 °C", "30.00 °C"],
        )

    def test_one_dropped_probe_keeps_the_other_two_updating(self) -> None:
        self.connect()
        self.set_three_valid_sensors()
        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        self.backend.temperature_payload = {
            "sample_id": 106,
            "source": "REAL_DS18B20",
            "sensors": [
                sensor(0, 328, "OK", rom_short=0x1200),
                sensor(1, 0, "NOT_PRESENT"),
                sensor(2, 488, "OK", rom_short=0x1202),
            ],
        }
        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        cards = self.controller.state.sensor_cards()
        self.assertEqual(cards[0].temperature_text, "20.50 °C")
        self.assertEqual(cards[1].temperature_text, "--")
        self.assertEqual(cards[1].quality_text, "未接入")
        self.assertEqual(cards[2].temperature_text, "30.50 °C")
        self.assertEqual(
            self.controller.state.statistics.valid_sensor_ids,
            (0, 2),
        )

    def test_crc_error_only_excludes_that_card(self) -> None:
        self.connect()
        self.backend.temperature_payload = {
            "sample_id": 102,
            "sensors": [
                sensor(0, 320, "OK", rom_short=0x1200),
                sensor(1, 0, "CRC_ERROR"),
                sensor(2, 480, "OK", rom_short=0x1202),
            ],
        }

        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        cards = self.controller.state.sensor_cards()
        self.assertEqual(cards[1].temperature_text, "--")
        self.assertEqual(cards[1].quality_text, "校验失败")
        self.assertEqual(
            self.controller.state.statistics.valid_sensor_ids,
            (0, 2),
        )

    def test_not_present_card_does_not_show_zero(self) -> None:
        self.connect()
        self.backend.temperature_payload = {
            "sample_id": 103,
            "sensors": [
                sensor(0, 320, "OK", rom_short=0x1200),
                sensor(1, 336, "OK", rom_short=0x1201),
                sensor(2, 0, "NOT_PRESENT"),
            ],
        }

        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        card = self.controller.state.sensor_cards()[2]
        self.assertEqual(card.temperature_text, "--")
        self.assertEqual(card.rom_text, "--")
        self.assertEqual(card.quality_text, "未接入")

    def test_all_invalid_sensors_report_no_valid_samples(self) -> None:
        self.connect()
        self.backend.temperature_payload = {
            "sample_id": 104,
            "sensors": [
                sensor(0, 0, "TIMEOUT"),
                sensor(1, 0, "CRC_ERROR"),
                sensor(2, 0, "RANGE_ERROR"),
            ],
        }

        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        statistics = self.controller.state.statistics
        self.assertFalse(statistics.has_valid_samples)
        self.assertEqual(statistics.minimum_temperature_text, "--")
        self.assertEqual(statistics.participating_sensor_text, "无有效样本")

    def test_missing_temperature_interface_is_honest(self) -> None:
        self.backend.temperature_payload = None
        self.backend.sensor_error = {
            "kind": "unsupported_protocol",
            "message": "DS18B20 interface is not frozen",
            "interface_unavailable": True,
        }
        self.connect()

        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        state = self.controller.state
        self.assertEqual(state.connection, ConnectionState.CONNECTED)
        self.assertEqual(state.sensor_cards()[0].temperature_text, "--")
        self.assertEqual(state.sensor_cards()[0].quality_text, "接口未冻结")
        self.assertFalse(state.statistics.has_valid_samples)

    def test_timeout_keeps_old_values_stale_and_recovers(self) -> None:
        self.connect()
        self.set_three_valid_sensors()
        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        self.backend.poll_results.append(
            TransactionTimeout("simulated transaction timeout")
        )
        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()

        state = self.controller.state
        self.assertEqual(state.connection, ConnectionState.DEVICE_UNRESPONSIVE)
        self.assertEqual(state.sensor_cards()[0].temperature_text, "20.00 °C")
        self.assertEqual(state.sensor_cards()[0].quality_text, "旧值")
        self.assertFalse(state.statistics.has_valid_samples)
        self.assertTrue(state.availability().disconnect)
        self.assertEqual(state.failed_count, 1)

        self.controller.tick(now=time.monotonic() + 60.0)
        # The recovery poll runs as background work, so wait for the state
        # change instead of for the UI busy flag.
        self.wait_until(
            lambda: self.controller.state.connection is ConnectionState.CONNECTED
        )
        self.assertEqual(self.controller.state.connection, ConnectionState.CONNECTED)
        self.assertEqual(self.controller.state.sensor_cards()[0].quality_text, "OK")

    def test_connect_failure_can_retry_successfully(self) -> None:
        self.backend.connect_results.append(TransportError("port unavailable"))

        self.assertTrue(self.controller.connect("/dev/fake", 1))
        self.wait_idle()
        self.assertEqual(
            self.controller.state.connection,
            ConnectionState.DEVICE_UNRESPONSIVE,
        )
        self.assertFalse(self.controller.state.session_open)
        self.assertTrue(self.controller.state.availability().connect)

        self.assertTrue(self.controller.connect("/dev/fake", 1))
        self.wait_idle()
        self.assertEqual(
            self.controller.state.connection,
            ConnectionState.CONNECTED,
        )
        self.assertEqual(
            [event[0] for event in self.backend.events],
            ["connect", "connect"],
        )

    def test_start_success_pending_and_failure_button_states(self) -> None:
        cases = (
            (
                "success",
                None,
                AcquisitionPhase.RUNNING,
                False,
                True,
            ),
            (
                "pending",
                {"pending": True},
                AcquisitionPhase.STARTING,
                False,
                True,
            ),
            (
                "failure",
                StateError("start rejected"),
                AcquisitionPhase.STOPPED,
                True,
                False,
            ),
        )
        for name, outcome, phase, can_start, can_stop in cases:
            with self.subTest(name=name):
                self.tearDown()
                self.setUp()
                self.connect()
                if outcome is not None:
                    self.backend.start_results.append(outcome)

                self.assertTrue(self.controller.start_periodic(30))
                self.wait_idle()

                state = self.controller.state
                self.assertEqual(state.acquisition, phase)
                self.assertEqual(state.period_sec, 30)
                self.assertEqual(
                    state.availability().start_periodic,
                    can_start,
                )
                self.assertEqual(
                    state.availability().stop_periodic,
                    can_stop,
                )

    def test_stop_timeout_does_not_pretend_stopped(self) -> None:
        self.connect()
        self.assertTrue(self.controller.start_periodic(10))
        self.wait_idle()
        self.assertEqual(
            self.controller.state.acquisition,
            AcquisitionPhase.RUNNING,
        )
        self.backend.stop_results.append(
            TransactionTimeout("stop acknowledgement timeout")
        )

        self.assertTrue(self.controller.stop_periodic())
        self.wait_idle()

        state = self.controller.state
        self.assertEqual(state.acquisition, AcquisitionPhase.STOP_UNCONFIRMED)
        self.assertNotEqual(state.acquisition, AcquisitionPhase.STOPPED)
        self.assertFalse(state.availability().start_periodic)
        self.assertTrue(state.availability().stop_periodic)

    def test_storage_counts_are_displayed_verbatim(self) -> None:
        self.connect()
        self.backend.storage_payload = {
            "storage_state_name": "READY",
            "storage_error_name": "NONE",
            "generated": 40,
            "synced": 31,
            "dropped": 2,
            "event_dropped": 3,
            "uncertain": 1,
            "queued": 8,
            "in_flight": 1,
            "last_synced_file": 12,
            "last_synced_date": 20260922,
            "last_synced_seq": 1234,
        }

        self.assertTrue(self.controller.refresh_storage())
        self.wait_idle()

        storage = self.controller.state.storage
        self.assertIsNotNone(storage)
        self.assertEqual(storage.text("generated"), "40")
        self.assertEqual(storage.text("synced"), "31")
        self.assertEqual(storage.text("dropped"), "2")
        self.assertEqual(storage.text("event_dropped"), "3")
        self.assertEqual(storage.text("uncertain"), "1")
        self.assertEqual(storage.text("queued"), "8")
        self.assertEqual(storage.text("in_flight"), "1")
        self.assertEqual(storage.text("last_synced_seq"), "1234")

    def test_shutdown_stops_worker_and_closes_backend(self) -> None:
        started = time.monotonic()
        exited = self.controller.shutdown(timeout=0.5)
        elapsed = time.monotonic() - started

        self.assertTrue(exited)
        self.assertLess(elapsed, 0.5)
        self.assertIn("close", [event[0] for event in self.backend.events])

    def test_background_poll_keeps_ui_usable_and_queues_user_action(self) -> None:
        self.connect()
        self.set_three_valid_sensors()
        self.backend.alarm_payload = alarm_payload()
        self.assertTrue(self.controller.refresh_sensors())
        self.wait_idle()
        self.backend.events.clear()
        self.backend.poll_delay = 0.20

        # A tick-driven poll is background work: it must not disable the UI.
        self.assertTrue(self.controller.tick(now=time.monotonic() + 60.0))
        deadline = time.monotonic() + 1.0
        while (
            "poll" not in [event[0] for event in self.backend.events]
            and time.monotonic() < deadline
        ):
            time.sleep(0.001)
        self.assertIn("poll", [event[0] for event in self.backend.events])
        self.assertFalse(self.controller.state.is_busy)
        self.assertTrue(self.controller.state.availability().single_sample)
        self.assertTrue(self.controller.state.availability().ack_alarm)

        # A user action submitted while that poll is running is queued, not lost.
        self.assertTrue(self.controller.single_sample())
        self.assertTrue(self.controller.state.is_busy)
        self.wait_idle(timeout=2.0)

        operations = [event[0] for event in self.backend.events]
        self.assertIn("poll", operations)
        self.assertIn("single_sample", operations)
        self.assertLess(operations.index("poll"), operations.index("single_sample"))
        self.assertEqual(self.controller.state.last_note, "单次采样完成")
        self.assertEqual(
            self.controller.state.sensor_cards()[0].temperature_text,
            "20.00 °C",
        )

    def test_shutdown_is_bounded_when_backend_call_blocks(self) -> None:
        started = threading.Event()

        class BlockingBackend(FakeBackend):
            def refresh_storage(self):
                started.set()
                time.sleep(0.20)
                return super().refresh_storage()

        backend = BlockingBackend()
        controller = GuiController(backend, close_timeout=1.0)
        self.assertTrue(controller.connect("/dev/fake", 1))
        deadline = time.monotonic() + 1.0
        while controller.state.is_busy and time.monotonic() < deadline:
            controller.poll()
            time.sleep(0.001)
        self.assertTrue(controller.refresh_storage())
        self.assertTrue(started.wait(0.5))

        shutdown_started = time.monotonic()
        exited = controller.shutdown(timeout=0.02)
        elapsed = time.monotonic() - shutdown_started

        self.assertFalse(exited)
        self.assertLess(elapsed, 0.10)
        time.sleep(0.25)
        self.assertIn("close", [event[0] for event in backend.events])


if __name__ == "__main__":
    unittest.main()
