"""Read Desktop rollouts without loading threads or taking their writer locks."""

from __future__ import annotations

import json
import os
from pathlib import Path
import time
from typing import Any


class TaskMonitor:
    def __init__(self, root: Path, state_path: Path) -> None:
        self.root = root
        self.state_path = state_path
        self.files: dict[str, dict[str, Any]] = {}
        self.tasks: dict[str, dict[str, Any]] = {}
        self.results: dict[str, dict[str, Any]] = {}
        self.seen: set[str] = set()
        self.next_scan = 0.0
        self.next_poll = 0.0
        self.initialized = False
        self._startup_time = time.time()
        self._last_snapshot: dict[str, Any] | None = None
        try:
            saved = json.loads(state_path.read_text(encoding="utf-8"))
            if saved.get("root") == str(root.resolve()):
                self.files = saved.get("files", {})
                self.tasks = saved.get("tasks", {})
                self.results = saved.get("results", {})
                self.seen = set(saved.get("seen", []))
        except (OSError, ValueError, AttributeError):
            pass

    def _save(self) -> None:
        try:
            self.state_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.state_path.with_suffix(".tmp")
            temporary.write_text(json.dumps({
                "root": str(self.root.resolve()), "files": self.files,
                "tasks": self.tasks, "results": self.results,
                "seen": sorted(self.seen),
            }, ensure_ascii=False), encoding="utf-8")
            os.replace(temporary, self.state_path)
        except OSError:
            # Monitoring remains usable when the state directory is read-only.
            pass

    def _record(self, entry: dict[str, Any], record: Any, notify: bool) -> None:
        if not isinstance(record, dict):
            return
        payload = record.get("payload")
        if not isinstance(payload, dict):
            return
        if record.get("type") in ("session_meta", "turn_context"):
            if record.get("type") == "session_meta":
                entry["thread_id"] = str(payload.get("id") or entry.get("thread_id") or "")
            entry["cwd"] = str(payload.get("cwd") or "")
            thread_id = entry["thread_id"]
            if thread_id and entry["cwd"]:
                if thread_id in self.tasks:
                    self.tasks[thread_id]["cwd"] = entry["cwd"]
                for result in self.results.values():
                    if result["thread_id"] == thread_id and not result.get("cwd"):
                        result["cwd"] = entry["cwd"]
            return
        if record.get("type") != "event_msg":
            return
        thread_id = str(payload.get("thread_id") or entry.get("thread_id") or "")
        if not thread_id:
            return
        task = self.tasks.setdefault(thread_id, {
            "running": False, "turn_id": "", "text": "", "cwd": entry.get("cwd", ""),
            "started_at": 0.0,
        })
        kind = payload.get("type")
        turn_id = str(payload.get("turn_id") or task.get("turn_id") or "")
        if kind == "task_started":
            task.update(running=True, turn_id=turn_id, text="", started_at=time.time())
        elif kind in ("agent_message", "item_completed"):
            if not notify and turn_id and not task.get("turn_id"):
                task.update(running=True, turn_id=turn_id)
            item = payload.get("item") if kind == "item_completed" else payload
            if not isinstance(item, dict):
                return
            if kind == "agent_message" or item.get("type") in ("AgentMessage", "agentMessage"):
                text = str(item.get("text") or item.get("message") or "")
                if text and item.get("phase") in ("final_answer", "finalAnswer", "FinalAnswer", None):
                    task["text"] = text[:4000]
        elif kind in ("task_complete", "task_failed", "turn_aborted"):
            # A delayed terminal event must not stop a newer turn.
            if task.get("turn_id") and turn_id != task["turn_id"]:
                return
            task["running"] = False
            failed = kind == "task_failed" or payload.get("status") == "failed"
            # User cancellation is terminal, but is not a failed task.
            if kind == "turn_aborted":
                failed = payload.get("reason") not in ("interrupted", "cancelled", "canceled")
            error = payload.get("error")
            error_text = error.get("message") if isinstance(error, dict) else error
            text = str(payload.get("last_agent_message") or payload.get("message")
                       or error_text or "")
            text = text or task.get("text") or (
                "任务失败，请在电脑端查看" if failed else
                "任务已取消" if kind == "turn_aborted" else "Codex 任务已完成"
            )
            key = f"{thread_id}:{turn_id}"
            started_at = float(task.get("started_at") or 0.0)
            elapsed_ms = max(0, int((time.time() - started_at) * 1000)) if started_at else 0
            if notify and turn_id and key not in self.seen:
                self.results[key] = {
                    "thread_id": thread_id, "turn_id": turn_id,
                    "failed": failed, "text": text[:4000],
                    "cwd": task.get("cwd", ""), "elapsed_ms": elapsed_ms,
                }

    def poll(self, *, force: bool = False) -> list[dict[str, Any]]:
        now = time.monotonic()
        if not force and now < self.next_poll:
            return []
        self.next_poll = now + 0.5
        new_files: set[str] = set()
        if now >= self.next_scan or not self.initialized:
            self.next_scan = now + 2.0
            try:
                for path in self.root.rglob("*.jsonl"):
                    name = str(path)
                    if name not in self.files:
                        self.files[name] = {"offset": 0, "thread_id": ""}
                        new_files.add(name)
            except OSError:
                pass
        changed = False
        for name, entry in self.files.items():
            path = Path(name)
            try:
                size = path.stat().st_size
                entry["modified_at"] = path.stat().st_mtime
                offset = int(entry.get("offset", 0))
                if size < offset:
                    offset = 0
                if size == offset:
                    if not self.initialized and entry["modified_at"] < self._startup_time - 7 * 24 * 3600:
                        task = self.tasks.get(entry.get("thread_id", ""))
                        if task and task.get("running"):
                            task["running"] = False
                            changed = True
                    continue
                with path.open("rb") as file:
                    # Initial history establishes running state, without creating
                    # reminders for old completions. Reads are bounded per poll.
                    if name in new_files and not self.initialized and size > 512 * 1024:
                        try:
                            self._record(entry, json.loads(file.readline()), False)
                        except (ValueError, UnicodeDecodeError):
                            pass
                        file.seek(size - 512 * 1024)
                        file.readline()  # Discard a partial first record.
                        offset = file.tell()
                    file.seek(offset)
                    data = file.read(4 * 1024 * 1024)
                consumed = 0
                for line in data.splitlines(keepends=True):
                    if not line.endswith(b"\n"):
                        break
                    consumed += len(line)
                    try:
                        record = json.loads(line)
                    except (ValueError, UnicodeDecodeError):
                        continue
                    self._record(entry, record, self.initialized and name not in
                                 getattr(self, "_baseline_files", set()))
                if name in new_files and not self.initialized and (
                    entry["modified_at"] < self._startup_time - 7 * 24 * 3600
                ):
                    task = self.tasks.get(entry.get("thread_id", ""))
                    if task:
                        task["running"] = False
                entry["offset"] = offset + consumed
                changed |= consumed > 0
                if name in getattr(self, "_baseline_files", set()) and entry["offset"] >= size:
                    self._baseline_files.discard(name)
            except (OSError, ValueError):
                continue
        if not self.initialized:
            # Large historical files may take several bounded reads.
            self._baseline_files = {
                name for name in new_files
                if Path(name).exists() and self.files[name]["offset"] < Path(name).stat().st_size
            }
        self.initialized = True
        if changed:
            self._save()
        current = self.snapshot()
        if force or current != self._last_snapshot:
            self._last_snapshot = current
            return [current]
        return []

    def snapshot(self) -> dict[str, Any]:
        running = sum(bool(task.get("running")) for task in self.tasks.values())
        failed = sum(bool(result.get("failed")) for result in self.results.values())
        done = len(self.results) - failed
        return {"type": "codex_state",
                "state": "busy" if running else "fail" if failed else "done" if done else "idle",
                "running": running, "done": done, "failed": failed}

    def pending(self, thread_id: str | None = None) -> dict[str, Any] | None:
        candidates = [result for result in self.results.values()
                      if thread_id is None or result["thread_id"] == thread_id]
        return next((r for r in candidates if r["failed"]), candidates[0] if candidates else None)

    def acknowledge(self, thread_id: str, turn_id: str) -> None:
        key = f"{thread_id}:{turn_id}"
        if self.results.pop(key, None) is not None:
            self.seen.add(key)
            self._save()

    def observe_wire(self, message: dict[str, Any]) -> None:
        params = message.get("params")
        if not isinstance(params, dict) or not params.get("threadId"):
            return
        thread_id = str(params["threadId"])
        if message.get("method") == "item/completed":
            self._record({"thread_id": thread_id}, {
                "type": "event_msg", "payload": {
                    "type": "item_completed", "thread_id": thread_id,
                    "turn_id": params.get("turnId"), "item": params.get("item"),
                },
            }, True)
            return
        turn = params.get("turn") or {}
        if not isinstance(turn, dict):
            return
        method = message.get("method")
        if method not in ("turn/started", "turn/completed"):
            return
        failed = turn.get("status") == "failed"
        self._record({"thread_id": thread_id}, {
            "type": "event_msg", "payload": {
                "type": "task_started" if method == "turn/started" else
                        "task_failed" if failed else
                        "turn_aborted" if turn.get("status") == "interrupted" else "task_complete",
                "turn_id": turn.get("id"), "error": turn.get("error"),
                "reason": "interrupted",
            },
        }, True)
        self._save()
