import base64
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "windows-package" / "runtime"))
from codex_backend import CodexAppServer
from mac_bridge import NetworkBridge, SerialBridge


class PendingProcess:
    def __init__(self):
        self.terminated = False

    def poll(self):
        return None

    def terminate(self):
        self.terminated = True

    def wait(self, timeout=None):
        return -1


class VoiceResultTests(unittest.TestCase):
    def test_disconnect_releases_capture_and_partial_packet_for_next_connection(self):
        class Backend:
            polls = 0

            def handle_device_event(self, event):
                return []

            def poll(self):
                self.polls += 1
                return []

        backend = Backend()
        bridge = NetworkBridge("unused", 8765, "unused", codex=backend)
        bridge.capture.start({})
        bridge.capture.pcm.extend(b"\0" * 32000)
        bridge.rx_buffer.extend(b'{"type":"audio_ch')
        bridge.close()
        self.assertFalse(bridge.capture.active)
        self.assertEqual(bridge.capture.pcm, bytearray())
        self.assertEqual(bridge.rx_buffer, bytearray())
        bridge.poll_codex()
        self.assertEqual(backend.polls, 1)
        bridge.handle_line(b'{"type":"audio_chunk","data":"AAAA"}')
        self.assertFalse(bridge.capture.active)

    def test_truncated_recording_never_starts_recognition(self):
        class Recognizer:
            def start(self, path):
                raise AssertionError("partial audio must not be recognized")
        with tempfile.TemporaryDirectory() as temp:
            bridge = SerialBridge("unused", speech=Recognizer(),
                                  recordings_dir=Path(temp))
            sent = []
            bridge.send = sent.append
            for event in (
                {"type": "record_start"},
                {"type": "audio_chunk",
                 "data": base64.b64encode(b"\0" * 16000).decode()},
                {"type": "record_stop", "bytes": 32000},
            ):
                bridge.handle_line(json.dumps(event).encode())
            self.assertEqual(len(sent), 1)
            self.assertEqual(sent[0]["type"], "error")
            self.assertIn("不完整", sent[0]["text"])
            self.assertFalse(bridge.capture.active)
            self.assertFalse((Path(temp) / "last-command.wav").exists())

    def test_device_result_is_short_and_keeps_review_identity(self):
        text = "已修复分页和返回问题。 " + "实现细节和测试步骤。" * 100
        result = CodexAppServer._result_message({
            "text": text, "failed": False, "thread_id": "a", "turn_id": "b",
        })
        self.assertLessEqual(len(result["text"]), 300)
        self.assertTrue(result["text"].startswith("已修复分页和返回问题。"))
        self.assertIn("电脑", result["text"])
        self.assertEqual((result["thread_id"], result["turn_id"]), ("a", "b"))

    def test_catalog_response_keeps_request_identity_on_every_packet(self):
        from mac_bridge import wire_messages
        class Backend:
            def handle_device_event(self, event):
                return [{"type": "codex_selected", "title": "task"},
                        {"type": "done", "text": "result " * 160},
                        {"type": "codex_state", "running": 0}]
        bridge = SerialBridge("unused", codex=Backend())
        sent = []
        bridge.send = lambda message: sent.extend(map(json.loads, wire_messages(message)))
        bridge.handle_line(json.dumps({"type": "codex_thread_select", "id": "t0",
                                       "request_id": 7}).encode())
        self.assertGreater(len(sent), 3)
        self.assertTrue(all(message.get("request_id") == 7 for message in sent))

    def test_cancel_catalog_restores_active_task_and_project(self):
        with tempfile.TemporaryDirectory() as temp:
            backend = CodexAppServer(Path(temp), sessions_root=Path(temp) / "sessions",
                                      task_state_path=Path(temp) / "state.json")
            backend.thread_id = "task-a"
            backend.thread_title = "Task A"
            backend.selected_project = {"cwd": temp, "name": "Project A"}
            # Substitute only desktop catalog I/O; selection/cancellation are real.
            backend._load_desktop_projects = lambda: [
                {"cwd": temp + "/b", "name": "Project B", "roots": [temp + "/b"]}]
            backend._request = lambda method, params: {"data": [
                {"id": "task-b", "name": "Task B", "cwd": temp + "/b"}]}
            backend.handle_device_event({"type": "codex_catalog_request", "request_id": 1})
            backend.handle_device_event({"type": "codex_project_select", "id": "p0"})
            backend.handle_device_event({"type": "codex_thread_select", "id": "t0"})
            backend.handle_device_event({"type": "codex_catalog_cancel"})
            self.assertEqual(backend.thread_id, "task-a")
            self.assertEqual(backend.thread_title, "Task A")
            self.assertEqual(backend.selected_project["name"], "Project A")
            self.assertIsNone(backend._catalog_backup)
            backend._rollout_offset = 123
            backend.handle_device_event({"type": "codex_catalog_request", "request_id": 5})
            backend.handle_device_event({"type": "codex_catalog_cancel"})
            self.assertEqual(backend._rollout_offset, 123)
            backend.handle_device_event({"type": "codex_project_select", "id": "p0",
                                         "request_id": 6})
            backend.handle_device_event({"type": "codex_thread_select", "id": "t0",
                                         "request_id": 7})
            backend.handle_device_event({"type": "codex_selection_confirm", "request_id": 7})
            backend.handle_device_event({"type": "codex_catalog_cancel"})
            self.assertEqual(backend.thread_id, "task-b")
            self.assertEqual(backend.selected_project["name"], "Project B")

    def test_cancel_discards_pending_recognition(self):
        bridge = SerialBridge("unused")
        process = PendingProcess()
        bridge.speech_process = process
        bridge.handle_line(json.dumps({"type": "record_cancel"}).encode())
        self.assertTrue(process.terminated)
        self.assertIsNone(bridge.speech_process)

    def test_canceled_capture_ignores_trailing_audio_and_stop(self):
        class Recognizer:
            def start(self, path):
                raise AssertionError("canceled recording must not be recognized")
        bridge = SerialBridge("unused", speech=Recognizer())
        sent = []
        bridge.send = sent.append
        for event in (
            {"type": "record_start"},
            {"type": "record_cancel"},
            {"type": "audio_chunk", "data": "AAAA"},
            {"type": "record_stop"},
        ):
            bridge.handle_line(json.dumps(event).encode())
        self.assertFalse(bridge.capture.active)
        self.assertEqual(sent, [])

    def test_recognition_timeout_returns_visible_error_once(self):
        now = [0.0]
        bridge = SerialBridge("unused", clock=lambda: now[0])
        process = PendingProcess()
        bridge.speech_process = process
        bridge.speech_started_at = 0.0
        sent = []
        bridge.send = sent.append
        now[0] = 16.0
        bridge.poll_speech()
        bridge.poll_speech()
        self.assertTrue(process.terminated)
        self.assertEqual(len(sent), 1)
        self.assertEqual(sent[0]["type"], "error")
        self.assertIn("超时", sent[0]["text"])

    def test_codex_poll_does_not_delay_pending_recognition(self):
        class Backend:
            def poll(self):
                raise AssertionError("synchronous IPC must not delay recognition")
        bridge = SerialBridge("unused", codex=Backend())
        bridge.speech_process = PendingProcess()
        # An exception swallowed by poll_codex would still send a device error.
        sent = []
        bridge.send = sent.append
        bridge.codex.poll = lambda: iter([{"type": "task_status", "text": "busy"}])
        bridge.poll_codex()
        self.assertEqual(sent, [])


if __name__ == "__main__":
    unittest.main()
