import io
import json
from pathlib import Path
import tempfile
import unittest

from tools.windows_codex_backend import WindowsCodexAppServer


class FakeProcess:
    def __init__(self):
        self.stdin = io.StringIO()

    @staticmethod
    def poll():
        return None


class WindowsStdioCodexTests(unittest.TestCase):
    def test_wifi_setup_keeps_single_com_port_as_array(self):
        script = (
            Path(__file__).parents[1] / "tools" / "windows_wifi_setup.ps1"
        ).read_text(encoding="utf-8-sig")
        self.assertIn(
            "$ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)",
            script,
        )

    def test_package_has_management_and_tray_launchers(self):
        package = Path(__file__).parents[1] / "windows-package"
        self.assertEqual(
            {path.name for path in package.glob("*.bat")},
            {"安装与管理编程伴侣.bat", "启动Codex桥接.bat"},
        )
        manager = (package / "runtime" / "manage_bridge.ps1").read_text(
            encoding="utf-8-sig"
        )
        self.assertIn("一键安装或修复", manager)
        self.assertIn("配置或更换设备 Wi-Fi", manager)
        self.assertIn("codex login status", manager)

    def test_write_uses_one_jsonl_record(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            backend = WindowsCodexAppServer(Path(temp_dir), executable="codex")
            backend.process = FakeProcess()
            backend._write({"id": 3, "method": "thread/list", "params": {}})
            line = backend.process.stdin.getvalue()
            self.assertEqual(line.count("\n"), 1)
            self.assertEqual(json.loads(line)["method"], "thread/list")

    def test_start_command_uses_official_turn_start_shape(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            backend = WindowsCodexAppServer(root, executable="codex")
            backend.thread_id = "thread-1"
            backend.selected_project = {"cwd": str(root)}
            calls = []

            def request(method, params):
                calls.append((method, params))
                return {"turn": {"id": "turn-1"}}

            backend._request = request
            backend._start_command("检查项目")

            self.assertEqual(calls[0][0], "turn/start")
            self.assertEqual(calls[0][1]["threadId"], "thread-1")
            self.assertEqual(calls[0][1]["input"][0]["text"], "检查项目")
            self.assertEqual(calls[0][1]["sandboxPolicy"]["type"], "workspaceWrite")
            self.assertEqual(backend.router.active_turn_id, "turn-1")


if __name__ == "__main__":
    unittest.main()
