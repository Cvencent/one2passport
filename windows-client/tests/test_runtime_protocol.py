import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "windows-package" / "runtime"))
from mac_bridge import wire_messages


class ResultProtocolTests(unittest.TestCase):
    def test_chunked_result_preserves_identity_for_review_ack(self):
        records = [json.loads(wire) for wire in wire_messages({
            "type": "done", "text": "long result " * 150,
            "thread_id": "thread-a", "turn_id": "turn-a",
        })]
        self.assertEqual(records[0].get("thread_id"), "thread-a")
        self.assertEqual(records[0].get("turn_id"), "turn-a")
        self.assertEqual("".join(r.get("text", "") for r in records), "long result " * 150)

    def test_long_error_is_chunked_without_dropping_review_content(self):
        wires = wire_messages({"type": "error", "text": "failure " * 300,
                               "thread_id": "thread-b", "turn_id": "turn-b"})
        self.assertTrue(all(len(wire) <= 700 for wire in wires))
        records = [json.loads(wire) for wire in wires]
        self.assertEqual("".join(r.get("text", "") for r in records), "failure " * 300)

    @unittest.skipUnless(os.name == "nt", "Windows tray integration")
    def test_initial_state_updates_tray_on_connection(self):
        from tray_app import ObservableBridge
        from windows_codex_backend import WindowsCodexAppServer

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sessions = root / "sessions"
            sessions.mkdir()
            (sessions / "rollout-a.jsonl").write_text(
                json.dumps({"type": "session_meta", "payload": {"id": "a"}}) + "\n" +
                json.dumps({"type": "event_msg", "payload": {
                    "type": "task_started", "turn_id": "a1"}}) + "\n", encoding="utf-8")
            backend = WindowsCodexAppServer(root, sessions_root=sessions,
                                            task_state_path=root / "state.json")
            states = []
            observer = ObservableBridge("127.0.0.1", 8765, "test-token",
                                        on_state=lambda state, detail: states.append(state),
                                        stop_event=threading.Event(),
                                        reconnect_event=threading.Event())
            observer._bridge.codex = backend
            observer._bridge.send = lambda event: None  # Device I/O only.
            try:
                observer._bridge.start_heartbeat()
            finally:
                observer._bridge.stop_heartbeat()
            self.assertEqual(states[-1:], ["busy"])


if __name__ == "__main__":
    unittest.main()
