from __future__ import annotations

import unittest

from tools.modbus_gui.controller import GuiController
from tools.modbus_gui.model import (
    AlarmSnapshot,
    ConnectionState,
    TemperatureSnapshot,
)
from fake_backend import FakeBackend

try:
    import tkinter as tk
    from tools.modbus_gui.ui import (
        TEMPERATURE_TREND_COLORS,
        GuiApplication,
    )
except ModuleNotFoundError:
    tk = None
    TEMPERATURE_TREND_COLORS = ()
    GuiApplication = None


@unittest.skipIf(tk is None or GuiApplication is None, "Tkinter is unavailable")
class UiSmokeTests(unittest.TestCase):
    def _application(self):
        backend = FakeBackend()
        controller = GuiController(backend)
        try:
            root = tk.Tk()
        except tk.TclError as exc:
            self.skipTest(f"Tk cannot initialize a display: {exc}")
        root.withdraw()
        app = GuiApplication(
            root,
            controller,
            port_lister=lambda: ["/dev/fake"],
        )
        return root, controller, app

    def _set_temperature_snapshot(
        self,
        controller: GuiController,
        sample_id: int,
        temperatures: tuple[int | None, int | None, int | None],
        qualities: tuple[str, str, str] | None = None,
    ) -> None:
        if qualities is None:
            qualities = tuple(
                "NOT_PRESENT" if value is None else "OK"
                for value in temperatures
            )
        controller.state.set_snapshot(
            TemperatureSnapshot.from_payload(
                {
                    "sample_id": sample_id,
                    "source": "REAL_DS18B20",
                    "sensors": [
                        {
                            "sensor_id": sensor_id,
                            "temperature_x16": temperature_x16,
                            "quality": qualities[sensor_id],
                            "rom_short": 0x1200 + sensor_id,
                            "sample_time": sample_id * 10 + sensor_id,
                            "source": "REAL_DS18B20",
                        }
                        for sensor_id, temperature_x16 in enumerate(
                            temperatures
                        )
                    ],
                }
            )
        )

    def test_window_builds_and_closes_without_device_io(self) -> None:
        root, controller, app = self._application()
        try:
            root.update_idletasks()
            self.assertEqual(controller.state.connection.value, "未连接")
            app.period_var.set("30")
            app._render()
            self.assertEqual(app.period_var.get(), "30")
            app.close()
        finally:
            try:
                root.destroy()
            except tk.TclError:
                pass
            controller.shutdown(timeout=0.5)

    def test_render_empty_temperature_trend_shows_waiting_state(self) -> None:
        root, controller, app = self._application()
        try:
            root.update_idletasks()
            app._render()

            items = app.temperature_trend_canvas.find_withtag(
                "temperature_trend_empty"
            )
            self.assertEqual(len(items), 1)
            self.assertEqual(
                app.temperature_trend_canvas.itemcget(items[0], "text"),
                "等待采样",
            )
            app.close()
        finally:
            try:
                root.destroy()
            except tk.TclError:
                pass
            controller.shutdown(timeout=0.5)

    def test_render_temperature_trend_draws_three_phase_lines(self) -> None:
        root, controller, app = self._application()
        try:
            self._set_temperature_snapshot(
                controller,
                1,
                (320, 400, 480),
            )
            self._set_temperature_snapshot(
                controller,
                2,
                (336, 416, 496),
            )
            root.update_idletasks()
            app._render()

            items = app.temperature_trend_canvas.find_withtag(
                "temperature_trend_line"
            )
            self.assertEqual(len(items), 3)
            self.assertEqual(
                [
                    app.temperature_trend_canvas.itemcget(item, "fill")
                    for item in items
                ],
                list(TEMPERATURE_TREND_COLORS),
            )
            app.close()
        finally:
            try:
                root.destroy()
            except tk.TclError:
                pass
            controller.shutdown(timeout=0.5)

    def test_render_temperature_trend_handles_missing_and_equal_values(
        self,
    ) -> None:
        root, controller, app = self._application()
        try:
            self._set_temperature_snapshot(
                controller,
                1,
                (320, 320, 320),
            )
            self._set_temperature_snapshot(
                controller,
                2,
                (None, 320, None),
                ("NOT_PRESENT", "OK", "NOT_PRESENT"),
            )
            root.update_idletasks()
            app._render()

            for phase_index in range(3):
                items = app.temperature_trend_canvas.find_withtag(
                    f"temperature_trend_phase_{phase_index}"
                )
                self.assertEqual(len(items), 1)
                coordinates = app.temperature_trend_canvas.coords(items[0])
                self.assertEqual(len(coordinates), 4)
                if phase_index == 1:
                    self.assertNotEqual(coordinates[:2], coordinates[2:])
                else:
                    self.assertEqual(coordinates[:2], coordinates[2:])
            app.close()
        finally:
            try:
                root.destroy()
            except tk.TclError:
                pass
            controller.shutdown(timeout=0.5)

    def test_render_shows_ds18b20_fields_and_no_humidity(self) -> None:
        root, controller, app = self._application()
        try:
            controller.state.set_snapshot(
                TemperatureSnapshot.from_payload(
                    {
                        "sample_id": 12,
                        "source": "REAL_DS18B20",
                        "sensors": [
                            {
                                "sensor_id": 0,
                                "temperature_x16": 320,
                                "quality": "OK",
                                "rom_short": 0x1200,
                                "sample_time": 1000,
                                "source": "REAL_DS18B20",
                            },
                            {
                                "sensor_id": 1,
                                "temperature_x16": None,
                                "quality": "NOT_PRESENT",
                                "rom_short": 0,
                                "sample_time": 1001,
                                "source": "REAL_DS18B20",
                            },
                            {
                                "sensor_id": 2,
                                "temperature_x16": 480,
                                "quality": "OK",
                                "rom_short": 0x1202,
                                "sample_time": 1002,
                                "source": "REAL_DS18B20",
                            },
                        ],
                    }
                )
            )
            app._render()

            self.assertEqual(app.card_vars[0]["temperature"].get(), "20.00 °C")
            self.assertEqual(app.card_vars[0]["rom"].get(), "0x1200")
            self.assertEqual(app.card_vars[1]["temperature"].get(), "--")
            self.assertEqual(app.card_vars[1]["quality"].get(), "未接入")
            self.assertEqual(app.card_vars[2]["rom"].get(), "0x1202")
            self.assertEqual(
                app.statistics_vars["minimum_temperature"].get(),
                "20.00 °C",
            )
            self.assertEqual(
                app.statistics_vars["maximum_temperature"].get(),
                "30.00 °C",
            )
            self.assertNotIn("humidity", app.card_vars[0])
            self.assertNotIn("minimum_humidity", app.statistics_vars)
            self.assertNotIn("maximum_humidity", app.statistics_vars)
            app.close()
        finally:
            try:
                root.destroy()
            except tk.TclError:
                pass
            controller.shutdown(timeout=0.5)

    def test_render_shows_three_phase_alarm_and_ack_button(self) -> None:
        root, controller, app = self._application()
        try:
            controller.state.connection = ConnectionState.CONNECTED
            controller.state.session_open = True
            controller.state.set_alarm(
                AlarmSnapshot.from_payload(
                    {
                        "contract_revision": 1,
                        "valid": True,
                        "level": "CRITICAL",
                        "reason": "PHASE_TEMPERATURE_HIGH",
                        "flags": {
                            "valid": True,
                            "latched": True,
                            "acknowledged": False,
                            "buzzer_active": True,
                        },
                        "trigger_phase": "B",
                        "delta_valid": True,
                        "maximum_delta_x16": 160,
                        "hottest_temperature_x16": 1200,
                        "hottest_phase": "B",
                        "phases": [
                            {
                                "phase": "A",
                                "temperature_x16": 1040,
                                "quality": "OK",
                            },
                            {
                                "phase": "B",
                                "temperature_x16": 1200,
                                "quality": "OK",
                            },
                            {
                                "phase": "C",
                                "temperature_x16": None,
                                "quality": "CRC_ERROR",
                            },
                        ],
                        "event_id": 4,
                        "alarm_sample_id": 55,
                        "duration_sec": 9,
                        "notice_count": 0,
                        "warning_count": 0,
                        "critical_count": 1,
                        "sensor_fault_count": 0,
                    }
                )
            )
            app._render()

            self.assertEqual(
                app.alarm_vars["level"].get(),
                "CRITICAL / 严重",
            )
            self.assertEqual(app.alarm_vars["trigger_phase"].get(), "B 相")
            self.assertEqual(app.alarm_vars["maximum_delta"].get(), "10.00 °C")
            self.assertEqual(app.alarm_vars["buzzer"].get(), "蜂鸣中")
            self.assertEqual(app.card_status_labels[0].cget("background"), "#dc2626")
            self.assertEqual(app.ack_button.instate(["!disabled"]), True)
            app.close()
        finally:
            try:
                root.destroy()
            except tk.TclError:
                pass
            controller.shutdown(timeout=0.5)


if __name__ == "__main__":
    unittest.main()
