import io
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "windows-package" / "runtime"))
from windows_codex_backend import WindowsCodexAppServer


class RunningProcess:
    stdin = io.StringIO()

    def poll(self):
        return None


class TaskMonitorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.backend = WindowsCodexAppServer(
            self.root, sessions_root=self.root / "sessions",
            task_state_path=self.root / "state.json",
        )
        self.backend.process = RunningProcess()
        self.backend.socket = self.backend
        self.backend.poll()

    def tearDown(self):
        self.temp.cleanup()

    def append(self, thread, *payloads):
        path = self.root / "sessions" / f"rollout-{thread}.jsonl"
        path.parent.mkdir(exist_ok=True)
        if not path.exists():
            path.write_text(json.dumps({
                "type": "session_meta", "payload": {"id": thread, "cwd": str(self.root)}
            }) + "\n", encoding="utf-8")
        with path.open("a", encoding="utf-8") as file:
            for payload in payloads:
                file.write(json.dumps({"type": "event_msg", "payload": payload}) + "\n")
        self.backend.task_monitor.next_scan = 0
        self.backend.task_monitor.next_poll = 0
        return path

    def state(self):
        states = [m for m in self.backend.poll() if m["type"] == "codex_state"]
        return states[-1] if states else self.backend.task_monitor.snapshot()

    def test_windows_polls_desktop_logs_without_selected_thread(self):
        self.append("a", {"type": "task_started", "turn_id": "a1"})
        self.assertEqual(self.state()["state"], "busy")
        self.append("a", {"type": "task_complete", "turn_id": "a1",
                          "last_agent_message": "finished"})
        self.assertEqual(self.state()["state"], "done")

    def test_other_running_thread_keeps_pet_busy(self):
        self.append("a", {"type": "task_started", "turn_id": "a1"})
        self.append("b", {"type": "task_started", "turn_id": "b1"})
        self.assertEqual(self.state()["running"], 2)
        self.append("a", {"type": "task_complete", "turn_id": "a1"})
        self.assertEqual(self.state()["state"], "busy")
        self.append("b", {"type": "task_failed", "turn_id": "b1", "message": "failed"})
        self.assertEqual(self.state()["state"], "fail")

    def test_view_ack_clears_only_matching_result(self):
        for thread in ("a", "b"):
            self.append(thread, {"type": "task_started", "turn_id": thread + "1"},
                        {"type": "task_complete", "turn_id": thread + "1"})
        self.assertEqual(self.state()["done"], 2)
        messages = self.backend.handle_device_event({"type": "codex_review_request"})
        result = next(m for m in messages if m["type"] == "done")
        self.backend.handle_device_event({"type": "codex_result_seen",
                                         "thread_id": result["thread_id"],
                                         "turn_id": "wrong"})
        self.assertEqual(self.state()["done"], 2)
        self.backend.handle_device_event({"type": "codex_result_seen",
                                         "thread_id": result["thread_id"],
                                         "turn_id": result["turn_id"]})
        self.assertEqual(self.state()["done"], 1)

    def test_baseline_detects_already_running_without_replaying_old_completion(self):
        self.append("old", {"type": "task_started", "turn_id": "old1"},
                    {"type": "task_complete", "turn_id": "old1"})
        self.append("active", {"type": "task_started", "turn_id": "active1"})
        backend = WindowsCodexAppServer(
            self.root, sessions_root=self.root / "sessions",
            task_state_path=self.root / "fresh.json",
        )
        backend.process = RunningProcess()
        state = next(m for m in backend.poll() if m["type"] == "codex_state")
        self.assertEqual((state["running"], state["done"]), (1, 0))

    def test_current_agent_message_schema_and_restart_preserve_unread(self):
        self.append("a", {"type": "task_started", "turn_id": "a1"},
                    {"type": "item_completed", "turn_id": "a1",
                     "item": {"type": "AgentMessage", "text": "current result",
                              "phase": "final_answer"}},
                    {"type": "task_complete", "turn_id": "a1"})
        self.assertEqual(self.state()["done"], 1)
        backend = WindowsCodexAppServer(
            self.root, sessions_root=self.root / "sessions",
            task_state_path=self.root / "state.json",
        )
        backend.process = RunningProcess()
        backend.poll()
        result = next(m for m in backend.handle_device_event(
            {"type": "codex_review_request"}) if m["type"] == "done")
        self.assertEqual(result["text"], "current result")

    def test_partial_line_does_not_generate_completion_until_finished(self):
        path = self.append("a", {"type": "task_started", "turn_id": "a1"})
        self.state()
        with path.open("ab") as file:
            file.write(b'{"type":"event_msg","payload":{"type":"task_complete","turn_id":"a1"}}')
        self.backend.task_monitor.next_poll = 0
        self.assertEqual(self.state()["state"], "busy")
        with path.open("ab") as file:
            file.write(b"\n")
        self.backend.task_monitor.next_poll = 0
        self.assertEqual(self.state()["state"], "done")

    def test_abandoned_old_rollout_does_not_keep_pet_busy_forever(self):
        path = self.append("old", {"type": "task_started", "turn_id": "old1"})
        old = time.time() - 30 * 24 * 3600
        os.utime(path, (old, old))
        backend = WindowsCodexAppServer(
            self.root, sessions_root=self.root / "sessions",
            task_state_path=self.root / "fresh.json",
        )
        backend.process = RunningProcess()
        backend.poll()
        self.assertEqual(backend.task_monitor.snapshot()["state"], "idle")

    def test_app_server_result_can_be_acknowledged_and_duplicate_stays_seen(self):
        self.backend.thread_id = "wire-thread"
        for method, status in (("turn/started", "inProgress"), ("turn/completed", "completed")):
            self.backend._process_wire_message(json.dumps({
                "method": method, "params": {"threadId": "wire-thread",
                                            "turn": {"id": "wire-turn", "status": status}}
            }).encode())
        result = next(m for m in self.backend.poll() if m["type"] == "done")
        self.assertEqual(result.get("thread_id"), "wire-thread")
        self.assertEqual(result.get("turn_id"), "wire-turn")
        self.backend.handle_device_event({"type": "codex_result_seen",
                                         "thread_id": "wire-thread", "turn_id": "wire-turn"})
        self.backend._process_wire_message(json.dumps({
            "method": "turn/completed", "params": {"threadId": "wire-thread",
                "turn": {"id": "wire-turn", "status": "completed"}}
        }).encode())
        self.assertEqual(self.backend.task_monitor.snapshot()["state"], "idle")

    def test_review_switches_workspace_and_clears_old_turn(self):
        self.append("new", {"type": "task_started", "turn_id": "new1"},
                    {"type": "task_complete", "turn_id": "new1"})
        self.state()
        self.backend.thread_id = "old"
        self.backend.selected_project = {"cwd": str(self.root / "wrong")}
        self.backend.router.active_turn_id = "old-turn"
        self.backend.handle_device_event({"type": "codex_review_request"})
        self.assertEqual(self.backend.selected_project["cwd"], str(self.root))
        self.assertIsNone(self.backend.router.active_turn_id)

    def test_opening_unread_result_commits_selection_over_old_catalog(self):
        self.append("new", {"type": "task_started", "turn_id": "new1"},
                    {"type": "task_complete", "turn_id": "new1"})
        self.state()
        self.backend.thread_id = "old"
        self.backend._load_desktop_projects = lambda: [
            {"cwd": str(self.root), "roots": [str(self.root)], "name": "fixture"}]
        self.backend.handle_device_event({"type": "codex_catalog_request", "request_id": 1})
        self.backend.handle_device_event({"type": "codex_review_request"})
        self.backend.handle_device_event({"type": "codex_selection_confirm", "request_id": 1})
        self.backend.handle_device_event({"type": "codex_catalog_cancel"})
        self.assertEqual(self.backend.thread_id, "new")
        self.assertEqual(self.backend.selected_project["cwd"], str(self.root))

    def test_rollout_metadata_restores_project_after_wire_start(self):
        self.backend._process_wire_message(json.dumps({
            "method": "turn/started", "params": {"threadId": "a",
                "turn": {"id": "a1", "status": "inProgress"}}
        }).encode())
        self.append("a", {"type": "task_started", "turn_id": "a1"},
                    {"type": "task_complete", "turn_id": "a1"})
        self.state()
        self.assertEqual(self.backend.task_monitor.pending()["cwd"], str(self.root))

    def test_wire_final_answer_is_available_when_opening_unread_result(self):
        for message in (
            {"method": "turn/started", "params": {"threadId": "a",
                "turn": {"id": "a1", "status": "inProgress"}}},
            {"method": "item/completed", "params": {"threadId": "a", "turnId": "a1",
                "item": {"type": "agentMessage", "text": "wire final answer",
                         "phase": "final_answer"}}},
            {"method": "turn/completed", "params": {"threadId": "a",
                "turn": {"id": "a1", "status": "completed"}}},
        ):
            self.backend._process_wire_message(json.dumps(message).encode())
        self.assertEqual(self.backend.task_monitor.pending()["text"], "wire final answer")


if __name__ == "__main__":
    unittest.main()
