"""Regression tests for the two Windows packaging bugs.

1. Double-clicking CodexBridge.exe used to spawn a permanent console window,
   because the ``codex app-server`` child was started without
   ``CREATE_NO_WINDOW`` while the tray process itself has no console.
2. Left clicking the tray icon used to quit the whole bridge, because the
   "exit" menu item was marked as pystray's default action. That made the icon
   look like it "disappeared by itself" seconds after launch.
"""

from pathlib import Path
import os
import threading
import unittest

import tray_app
import windows_codex_backend

RUNTIME = Path(__file__).resolve().parents[1] / "runtime"


class NoConsoleChildProcessTests(unittest.TestCase):
    def test_console_flag_is_used_for_app_server(self):
        source = (RUNTIME / "windows_codex_backend.py").read_text(
            encoding="utf-8"
        )
        # The comment above the flags mentions ")" so slice on the next
        # statement instead of on the first closing parenthesis.
        popen_call = source.split("self.process = subprocess.Popen(", 1)[1]
        popen_call = popen_call.split("except OSError", 1)[0]
        self.assertIn("creationflags=CREATE_NO_WINDOW", popen_call)
        self.assertIn("startupinfo=_hidden_startupinfo()", popen_call)

    def test_hidden_startupinfo_hides_the_window(self):
        info = windows_codex_backend._hidden_startupinfo()
        if os.name != "nt":
            self.assertIsNone(info)
            return
        self.assertTrue(info.dwFlags & windows_codex_backend.subprocess.
                        STARTF_USESHOWWINDOW)
        self.assertEqual(
            info.wShowWindow, windows_codex_backend.subprocess.SW_HIDE
        )
        self.assertNotEqual(windows_codex_backend.CREATE_NO_WINDOW, 0)


class TrayDefaultActionTests(unittest.TestCase):
    def _menu(self):
        class Menu(object):
            SEPARATOR = object()

            def __init__(self, *items):
                self.items = items

        class MenuItem(object):
            def __init__(self, text, action, **kwargs):
                self.text = text
                self.default = kwargs.get("default", False)

        app = tray_app.TrayApp.__new__(tray_app.TrayApp)
        app._pystray = type("Pystray", (), {"Menu": Menu, "MenuItem": MenuItem})
        app._state_lock = threading.Lock()
        app._state = tray_app.STATE_UNPAIRED
        app._detail = "尚未配对设备"
        app._bridge = None
        return app._build_menu()

    @staticmethod
    def _items(menu):
        return [item for item in menu.items if hasattr(item, "text")]

    def test_left_click_does_not_quit(self):
        defaults = [item.text for item in self._items(self._menu()) if item.default]
        self.assertEqual(defaults, ["查看状态"])

    def test_exit_is_explicit_only(self):
        items = self._items(self._menu())
        self.assertIn("退出", [item.text for item in items])
        self.assertNotIn("退出", [item.text for item in items if item.default])


class TrayIconWatchdogTests(unittest.TestCase):
    def _app(self, probe_result):
        class FakeIcon(object):
            def __init__(self):
                self._hwnd = 4242
                self.title = "Codex 桥接"
                self.shown = 0

            def _show(self):
                self.shown += 1

        app = tray_app.TrayApp.__new__(tray_app.TrayApp)
        icon = FakeIcon()
        app._icon = icon
        original = tray_app.tray_icon_registered
        self.addCleanup(setattr, tray_app, "tray_icon_registered", original)
        tray_app.tray_icon_registered = lambda hwnd, uid, tip: probe_result
        return app, icon

    def test_missing_icon_is_re_registered(self):
        app, icon = self._app(False)
        app._ensure_icon_registered()
        self.assertEqual(icon.shown, 1)

    def test_present_icon_is_left_alone(self):
        app, icon = self._app(True)
        app._ensure_icon_registered()
        self.assertEqual(icon.shown, 0)

    def test_unknown_state_is_not_treated_as_missing(self):
        # 探测不了(None)时不能乱挂,否则会周期性闪图标。
        app, icon = self._app(None)
        app._ensure_icon_registered()
        self.assertEqual(icon.shown, 0)

    def test_unknown_hwnd_is_ignored(self):
        app, icon = self._app(False)
        icon._hwnd = None
        app._ensure_icon_registered()
        self.assertEqual(icon.shown, 0)

    def test_probe_is_inert_without_windows_or_hwnd(self):
        self.assertIsNone(tray_app.tray_icon_registered(None, 1, "x"))


if __name__ == "__main__":
    unittest.main()