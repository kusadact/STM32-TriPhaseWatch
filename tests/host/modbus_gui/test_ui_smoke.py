from __future__ import annotations

import unittest

from tools.modbus_gui.controller import GuiController
from fake_backend import FakeBackend

try:
    import tkinter as tk
    from tools.modbus_gui.ui import GuiApplication
except ModuleNotFoundError:
    tk = None
    GuiApplication = None


@unittest.skipIf(tk is None or GuiApplication is None, "Tkinter is unavailable")
class UiSmokeTests(unittest.TestCase):
    def test_window_builds_and_closes_without_device_io(self) -> None:
        backend = FakeBackend()
        controller = GuiController(backend)
        try:
            root = tk.Tk()
        except tk.TclError as exc:
            self.skipTest(f"Tk cannot initialize a display: {exc}")
        root.withdraw()
        try:
            app = GuiApplication(
                root,
                controller,
                port_lister=lambda: ["/dev/fake"],
            )
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


if __name__ == "__main__":
    unittest.main()
