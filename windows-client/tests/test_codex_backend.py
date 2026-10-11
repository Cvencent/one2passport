import unittest
from pathlib import Path
import json
import socket
import struct
import tempfile
import threading

from tools.codex_backend import (
    CodexAppServer,
    CodexEventRouter,
    DesktopCodexIpc,
    _encode_websocket_frame,
    _take_websocket_frame,
    compact_result,
    compact_text,
)


class CompactTextTests(unittest.TestCase):
    def test_compacts_whitespace_and_limits_device_text(self):
        self.assertEqual(compact_text("第一行\n\n第二行", 20), "第一行 第二行")
        self.assertEqual(compact_text("一二三四五六", 5), "一二三四…")

    def test_removes_unrenderable_emoji_but_keeps_common_punctuation(self):
        self.assertEqual(
            compact_text("🎮《任务》 → “测试” ✅", 30),
            "《任务》 “测试”",
        )

    def test_result_uses_first_complete_line(self):
        result = "结论：主流程可运行。\n\n- 高风险：后续详细说明"
        self.assertEqual(compact_result(result, 54), "结论：主流程可运行。")


class WebSocketTransportTests(unittest.TestCase):
    def test_masked_client_frame_round_trip(self):
        payload = '{"id":1,"method":"thread/list"}'.encode("utf-8")
        buffer = bytearray(
            _encode_websocket_frame(payload, mask_key=b"test")
        )
        opcode, final, decoded = _take_websocket_frame(buffer)
        self.assertEqual(opcode, 0x1)
        self.assertTrue(final)
        self.assertEqual(decoded, payload)
        self.assertEqual(buffer, bytearray())

    def test_incomplete_frame_waits_for_more_bytes(self):
        frame = _encode_websocket_frame(b"x" * 200, mask_key=b"mask")
        buffer = bytearray(frame[:20])
        self.assertIsNone(_take_websocket_frame(buffer))
        buffer.extend(frame[20:])
        self.assertEqual(_take_websocket_frame(buffer)[2], b"x" * 200)


class DesktopCodexIpcTests(unittest.TestCase):
    @staticmethod
    def read_packet(connection):
        header = connection.recv(4)
        length = struct.unpack("<I", header)[0]
        payload = bytearray()
        while len(payload) < length:
            payload.extend(connection.recv(length - len(payload)))
        return json.loads(payload.decode("utf-8"))

    @staticmethod
    def write_packet(connection, message):
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        connection.sendall(struct.pack("<I", len(payload)) + payload)

    def test_sends_turn_to_desktop_owner_without_starting_another_writer(self):
        client_socket, server_socket = socket.socketpair()
        received = []

        def serve():
            try:
                initialize = self.read_packet(server_socket)
                received.append(initialize)
                self.write_packet(
                    server_socket,
                    {
                        "type": "response",
                        "requestId": initialize["requestId"],
                        "resultType": "success",
                        "method": "initialize",
                        "handledByClientId": "bridge-client",
                        "result": {"clientId": "bridge-client"},
                    },
                )

                discovery = self.read_packet(server_socket)
                received.append(discovery)
                self.write_packet(
                    server_socket,
                    {
                        "type": "response",
                        "requestId": discovery["requestId"],
                        "resultType": "success",
                        "method": "thread-owner-discovery",
                        "handledByClientId": "desktop-owner",
                        "result": {},
                    },
                )

                start = self.read_packet(server_socket)
                received.append(start)
                self.write_packet(
                    server_socket,
                    {
                        "type": "response",
                        "requestId": start["requestId"],
                        "resultType": "success",
                        "method": "thread-follower-start-turn",
                        "handledByClientId": "desktop-owner",
                        "result": {"method": "thread-follower-start-turn", "result": {}},
                    },
                )
            finally:
                server_socket.close()

        server = threading.Thread(target=serve)
        server.start()
        client = DesktopCodexIpc(Path("/unused"))
        client._open_socket = lambda: client_socket
        client.send_message("thread-real-id", "只读检查项目")
        server.join(timeout=2)

        self.assertFalse(server.is_alive())
        self.assertEqual(received[0]["method"], "initialize")
        self.assertEqual(received[1]["method"], "thread-owner-discovery")
        self.assertEqual(received[2]["method"], "thread-follower-start-turn")
        self.assertEqual(received[2]["version"], 2)
        self.assertEqual(received[2]["targetClientId"], "desktop-owner")
        turn = received[2]["params"]["turnStart"]
        self.assertEqual(turn["request"]["threadId"], "thread-real-id")
        self.assertEqual(turn["request"]["input"][0]["text"], "只读检查项目")
        self.assertTrue(turn["context"]["inheritThreadSettings"])


class CodexEventRouterTests(unittest.TestCase):
    def setUp(self):
        self.router = CodexEventRouter()

    def test_command_approval_round_trip(self):
        messages = self.router.process(
            {
                "id": 41,
                "method": "item/commandExecution/requestApproval",
                "params": {
                    "command": "npm run build",
                    "cwd": "/tmp/project",
                    "reason": "需要验证构建",
                    "availableDecisions": [
                        "accept",
                        "acceptForSession",
                        "decline",
                        "cancel",
                    ],
                },
            }
        )
        self.assertEqual(messages[0]["type"], "approval")
        self.assertIn("npm run build", messages[0]["detail"])

        status = self.router.submit_approval("approve_session")
        self.assertIn("继续执行", status[0]["text"])
        self.assertEqual(
            self.router.take_rpc_outbox(),
            [{"id": 41, "result": {"decision": "acceptForSession"}}],
        )

    def test_file_change_can_be_declined(self):
        self.router.process(
            {
                "id": "write-1",
                "method": "item/fileChange/requestApproval",
                "params": {"reason": "写入工作区外目录"},
            }
        )
        self.router.submit_approval("deny")
        self.assertEqual(
            self.router.take_rpc_outbox(),
            [{"id": "write-1", "result": {"decision": "decline"}}],
        )

    def test_unknown_interactive_request_is_not_approved(self):
        messages = self.router.process(
            {"id": 9, "method": "item/tool/requestUserInput", "params": {}}
        )
        self.assertIn("安全拒绝", messages[0]["text"])
        response = self.router.take_rpc_outbox()[0]
        self.assertIn("error", response)

    def test_turn_completion_returns_compact_real_result(self):
        self.router.process(
            {
                "method": "turn/started",
                "params": {"turn": {"id": "turn-1"}},
            }
        )
        self.router.process(
            {
                "method": "item/completed",
                "params": {
                    "item": {
                        "type": "agentMessage",
                        "text": "检查完成，没有修改文件。",
                    }
                },
            }
        )
        messages = self.router.process(
            {
                "method": "turn/completed",
                "params": {"turn": {"id": "turn-1", "status": "completed"}},
            }
        )
        self.assertEqual(
            messages,
            [{"type": "done", "text": "检查完成，没有修改文件。"}],
        )
        self.assertIsNone(self.router.active_turn_id)

    def test_turn_completion_keeps_more_than_the_old_54_character_summary(self):
        answer = (
            "结论：主流程可以运行。\n"
            "第二部分：设备会保留更完整的回答，不再只显示第一句。\n"
            "第三部分：内容较长时可以手动翻页查看。"
        )
        self.router.process(
            {"method": "turn/started", "params": {"turn": {"id": "turn-long"}}}
        )
        self.router.process(
            {
                "method": "item/completed",
                "params": {"item": {"type": "agentMessage", "text": answer}},
            }
        )
        messages = self.router.process(
            {
                "method": "turn/completed",
                "params": {"turn": {"id": "turn-long", "status": "completed"}},
            }
        )

        self.assertEqual(messages[0]["type"], "done")
        self.assertIn("第二部分", messages[0]["text"])
        self.assertIn("第三部分", messages[0]["text"])
        self.assertGreater(len(messages[0]["text"]), 54)

    def test_declined_command_is_not_reported_as_completed(self):
        messages = self.router.process(
            {
                "method": "item/completed",
                "params": {
                    "item": {
                        "type": "commandExecution",
                        "status": "declined",
                        "exitCode": None,
                    }
                },
            }
        )
        self.assertIn("已拒绝", messages[0]["text"])


class CodexCatalogTests(unittest.TestCase):
    def setUp(self):
        self.opened_threads = []
        self.desktop_messages = []
        self.backend = CodexAppServer(
            Path("/tmp"),
            thread_opener=self.opened_threads.append,
            desktop_sender=lambda thread_id, text: self.desktop_messages.append(
                (thread_id, text)
            ),
            desktop_state_path=Path("/tmp/does-not-exist-codex-state.json"),
        )
        self.calls = []

        def request(method, params):
            self.calls.append((method, params))
            if method == "project/list":
                return {
                    "data": [
                        {
                            "id": "project-real-id",
                            "name": "多肉阳台",
                            "roots": [{"path": "/tmp/succulent"}],
                        }
                    ]
                }
            if method == "thread/list":
                return {
                    "data": [
                        {
                            "id": "thread-real-id",
                            "name": "修复商店页面",
                            "preview": "检查商店",
                            "status": {"type": "idle"},
                        }
                    ]
                }
            if method == "thread/read":
                return {
                    "thread": {
                        "id": "thread-real-id",
                        "name": "修复商店页面",
                        "turns": [],
                    }
                }
            if method == "thread/resume":
                return {
                    "thread": {
                        "id": "thread-real-id",
                        "name": "修复商店页面",
                        "turns": [],
                    }
                }
            if method == "thread/queue/add":
                return {
                    "queuedSubmission": {
                        "id": "queued-1",
                        "clientUserMessageId": params["clientUserMessageId"],
                        "input": params["input"],
                    }
                }
            if method == "thread/queue/start":
                return {"turn": {"id": "turn-real-id", "status": "inProgress"}}
            if method == "thread/start":
                return {"thread": {"id": "new-thread-id"}}
            if method == "thread/name/set":
                return {}
            if method == "thread/unsubscribe":
                return {}
            raise AssertionError(method)

        self.backend._request = request

    def test_lists_real_projects_then_real_threads(self):
        projects = self.backend.handle_device_event(
            {"type": "codex_catalog_request"}
        )
        self.assertEqual(projects[1]["title"], "多肉阳台")
        self.assertEqual(projects[-1]["type"], "codex_catalog_end")

        threads = self.backend.handle_device_event(
            {"type": "codex_project_select", "id": "p0"}
        )
        self.assertEqual(threads[1]["title"], "修复商店页面")
        self.assertEqual(self.calls[-1][1]["projectId"], "project-real-id")

    def test_selects_existing_thread_without_loading_it(self):
        self.backend.handle_device_event({"type": "codex_catalog_request"})
        self.backend.handle_device_event(
            {"type": "codex_project_select", "id": "p0"}
        )
        selected = self.backend.handle_device_event(
            {"type": "codex_thread_select", "id": "t0"}
        )
        self.assertEqual(self.backend.thread_id, "thread-real-id")
        self.assertEqual(selected[0]["type"], "codex_selected")
        self.assertEqual(selected[0]["title"], "修复商店页面")
        self.assertEqual(self.calls[-1][0], "thread/list")
        self.assertEqual(self.opened_threads, ["thread-real-id"])

    def test_selecting_thread_does_not_repeat_stale_approval_state(self):
        self.backend.handle_device_event({"type": "codex_catalog_request"})
        self.backend.handle_device_event(
            {"type": "codex_project_select", "id": "p0"}
        )

        messages = self.backend.handle_device_event(
            {"type": "codex_thread_select", "id": "t0"}
        )
        self.assertEqual(messages[0]["type"], "codex_selected")
        self.assertFalse(any(m["type"] == "approval" for m in messages))
        self.assertTrue(all(method != "thread/read" for method, _ in self.calls))

    def test_new_thread_stays_a_desktop_operation(self):
        messages = self.backend.handle_device_event({"type": "codex_thread_new"})
        self.assertEqual(messages[0]["type"], "error")
        self.assertIn("电脑", messages[0]["text"])
        self.assertTrue(all(method != "thread/start" for method, _ in self.calls))

    def test_voice_routes_through_desktop_task_owner(self):
        self.backend.handle_device_event({"type": "codex_catalog_request"})
        self.backend.handle_device_event(
            {"type": "codex_project_select", "id": "p0"}
        )
        self.backend.handle_device_event(
            {"type": "codex_thread_select", "id": "t0"}
        )
        messages = self.backend.handle_device_event(
            {"type": "voice_command", "text": "只读检查项目"}
        )
        self.assertEqual(messages[0]["type"], "task_status")
        self.assertIn("正在处理", messages[0]["text"])
        self.assertEqual(
            self.desktop_messages,
            [("thread-real-id", "只读检查项目")],
        )
        self.assertIsNone(self.backend.queued_submission_id)
        self.assertTrue(
            all(
                method not in {"thread/resume", "thread/queue/add", "thread/queue/start"}
                for method, _ in self.calls
            )
        )
        self.assertEqual(
            self.opened_threads, ["thread-real-id", "thread-real-id"]
        )

    def test_queued_command_prevents_duplicate_submission(self):
        self.backend.thread_id = "thread-real-id"
        self.backend.queued_submission_id = "queued-1"
        messages = self.backend.handle_device_event(
            {"type": "voice_command", "text": "不要重复启动"}
        )
        self.assertIn("上一条", messages[0]["text"])
        self.assertTrue(all(method != "thread/queue/add" for method, _ in self.calls))

    def test_queue_poll_reports_desktop_handoff_without_fake_approval(self):
        self.backend.thread_id = "thread-real-id"
        self.backend.queued_submission_id = "queued-1"

        def request(method, params):
            self.calls.append((method, params))
            if method == "thread/queue/list":
                return {"data": []}
            raise AssertionError(method)

        self.backend._request = request
        self.backend._poll_queued_submission()
        self.assertTrue(
            any(
                "电脑端已接收" in message.get("text", "")
                for message in self.backend._device_outbox
            )
        )
        self.assertTrue(
            all(
                "等待审批" not in message.get("text", "")
                for message in self.backend._device_outbox
            )
        )

    def test_voice_requires_explicit_task_selection(self):
        messages = self.backend.handle_device_event(
            {"type": "voice_command", "text": "检查项目"}
        )
        self.assertIn("先选择", messages[0]["text"])


class RolloutSyncTests(unittest.TestCase):
    def setUp(self):
        self.backend = CodexAppServer(Path("/tmp"))
        self.backend.selected_project = {"roots": ["/tmp/project"]}

    @staticmethod
    def append(path, *records):
        with path.open("a", encoding="utf-8") as file:
            for record in records:
                file.write(json.dumps(record, ensure_ascii=False) + "\n")

    def test_ignores_old_turn_then_reports_new_final_answer(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            rollout = Path(temp_dir) / "rollout.jsonl"
            self.append(
                rollout,
                {
                    "type": "event_msg",
                    "payload": {"type": "task_complete", "last_agent_message": "旧回答"},
                },
            )
            self.backend._set_rollout_baseline(rollout)
            self.backend._poll_rollout(10.0)
            self.assertEqual(list(self.backend._device_outbox), [])

            self.append(
                rollout,
                {
                    "type": "event_msg",
                    "payload": {"type": "task_started", "turn_id": "turn-new"},
                },
                {
                    "type": "event_msg",
                    "payload": {
                        "type": "task_complete",
                        "turn_id": "turn-new",
                        "last_agent_message": "结论：真实任务已经完成。\n后续详情",
                    },
                },
            )
            self.backend._poll_rollout(11.0)
            messages = list(self.backend._device_outbox)
            self.assertEqual(messages[0]["type"], "task_status")
            self.assertEqual(
                messages[-1],
                {"type": "done", "text": "结论：真实任务已经完成。 后续详情"},
            )

    def test_reports_slow_external_file_approval_then_completion(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            rollout = Path(temp_dir) / "rollout.jsonl"
            rollout.touch()
            self.backend._set_rollout_baseline(rollout)
            self.append(
                rollout,
                {
                    "type": "response_item",
                    "payload": {
                        "type": "custom_tool_call",
                        "call_id": "call-1",
                        "name": "exec",
                        "input": "*** Begin Patch\n*** Add File: /private/tmp/test.txt\n+x\n*** End Patch",
                    },
                },
            )
            self.backend._poll_rollout(20.0)
            self.backend._poll_rollout(21.1)
            self.assertEqual(self.backend._device_outbox[-1]["type"], "approval")

            self.append(
                rollout,
                {
                    "type": "response_item",
                    "payload": {
                        "type": "custom_tool_call_output",
                        "call_id": "call-1",
                        "output": "success",
                    },
                },
                {
                    "type": "event_msg",
                    "payload": {"type": "task_complete", "last_agent_message": "任务完成"},
                },
            )
            self.backend._poll_rollout(22.0)
            messages = list(self.backend._device_outbox)
            self.assertTrue(any(message["type"] == "task_status" for message in messages))
            self.assertEqual(messages[-1], {"type": "done", "text": "任务完成"})


class DesktopProjectCatalogTests(unittest.TestCase):
    def test_uses_desktop_names_order_and_manual_project_pages(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            state_path = Path(temp_dir) / "state.json"
            local_projects = {}
            order = []
            for index in range(10):
                project_id = f"desktop-{index}"
                order.append(project_id)
                local_projects[project_id] = {
                    "name": f"电脑项目{index + 1}",
                    "rootPaths": [f"/tmp/root-{index}"],
                }
            state_path.write_text(
                json.dumps(
                    {
                        "local-projects": local_projects,
                        "project-order": order,
                        "thread-project-assignments": {},
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )
            backend = CodexAppServer(Path("/tmp"), desktop_state_path=state_path)

            first_page = backend._list_projects()
            self.assertEqual(first_page[1]["title"], "电脑项目1")
            self.assertEqual(first_page[-2]["title"], "下一页 2/2")
            second_page = backend._list_threads("p_more")
            self.assertEqual(second_page[1]["title"], "电脑项目8")
            self.assertEqual(second_page[-2]["title"], "回到第1页")

    def test_desktop_project_filters_threads_by_assignment_and_root(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            state_path = Path(temp_dir) / "state.json"
            state_path.write_text(
                json.dumps(
                    {
                        "local-projects": {
                            "desktop-real": {
                                "name": "多肉休闲手机游戏",
                                "rootPaths": ["/tmp/games"],
                            }
                        },
                        "project-order": ["desktop-real"],
                        "thread-project-assignments": {
                            "assigned-thread": {"projectId": "desktop-real"},
                            "moved-away": {"projectId": "another-project"},
                        },
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )
            backend = CodexAppServer(Path("/tmp"), desktop_state_path=state_path)

            def request(method, params):
                self.assertEqual(method, "thread/list")
                return {
                    "data": [
                        {"id": "assigned-thread", "name": "手动归类", "cwd": "/elsewhere"},
                        {"id": "root-thread", "name": "根目录任务", "cwd": "/tmp/games/client"},
                        {"id": "moved-away", "name": "已移走", "cwd": "/tmp/games"},
                    ],
                    "nextCursor": None,
                }

            backend._request = request
            backend._list_projects()
            messages = backend._list_threads("p0")
            titles = [message.get("title") for message in messages]
            self.assertIn("手动归类", titles)
            self.assertIn("根目录任务", titles)
            self.assertNotIn("已移走", titles)


if __name__ == "__main__":
    unittest.main()
