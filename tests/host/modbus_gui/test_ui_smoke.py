from __future__ import annotations

import unittest

from tools.modbus_gui.controller import GuiController
from tools.modbus_gui.model import TemperatureSnapshot
from fake_backend import FakeBackend

try:
    import tkinter as tk
    from tools.modbus_gui.ui import GuiApplication
except ModuleNotFoundError:
    tk = None
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


if __name__ == "__main__":
    unittest.main()
