#!/usr/bin/env python3
"""Windows stdio transport for the official Codex app-server protocol."""

from __future__ import annotations

import json
import os
import queue
import shutil
import subprocess
import threading
import time
from pathlib import Path
from typing import Any

if __package__:
    from .codex_backend import (
        CodexAppServer,
        CodexEventRouter,
        CodexProtocolError,
        read_last_message,
    )
else:
    from codex_backend import (
        CodexAppServer,
        CodexEventRouter,
        CodexProtocolError,
        read_last_message,
    )


def _codex_desktop_candidates() -> list[Path]:
    """列出 Codex 桌面端(code++)自带的 codex.exe 候选路径。

    桌面端的布局是:
        %LOCALAPPDATA%\\OpenAI\\Codex\\bin\\<版本哈希>\\codex.exe
    父目录是【哈希】而非固定名,每次应用更新都会新增一个哈希目录,
    因此必须按修改时间倒序扫描、取最新那个,否则升级一次就找不到。
    这里的进程也确实不在 PATH 上,shutil.which 必然落空。
    """
    roots: list[Path] = []
    local_appdata = os.environ.get("LOCALAPPDATA")
    if local_appdata:
        roots.append(Path(local_appdata) / "OpenAI" / "Codex" / "bin")
    # 社区版/绿色版可能落在别处,顺带覆盖几个常见位置。
    home = Path.home()
    roots.extend(
        [
            home / ".codex" / "bin",
            home / "AppData" / "Local" / "Programs" / "Codex",
        ]
    )

    found: list[Path] = []
    for root in roots:
        if not root.is_dir():
            continue
        # 直接命中根目录下的 codex.exe
        direct = root / "codex.exe"
        if direct.is_file():
            found.append(direct)
        # 再扫一层版本哈希目录。只扫一层,避免误抓到无关的深层副本。
        try:
            children = [child for child in root.iterdir() if child.is_dir()]
        except OSError:
            continue
        for child in children:
            candidate = child / "codex.exe"
            if candidate.is_file():
                found.append(candidate)
    return found


def _newest(paths: list[Path]) -> Path | None:
    if not paths:
        return None
    try:
        return max(paths, key=lambda item: item.stat().st_mtime)
    except OSError:
        return paths[0]


def find_codex_executable(explicit: str | None = None) -> str:
    if explicit:
        resolved = shutil.which(explicit)
        if resolved:
            return resolved
        candidate = Path(explicit).expanduser()
        if candidate.is_file():
            return str(candidate)
        # 显式路径无效时继续往下探测,而不是直接报错 —— 用户很可能只是
        # 粘贴了一个旧版本哈希目录,新版本已经换地方了。
    found = shutil.which("codex") or shutil.which("codex.exe")
    if found:
        return found
    desktop = _newest(_codex_desktop_candidates())
    if desktop is not None:
        return str(desktop)
    raise CodexProtocolError(
        "没有找到 Codex CLI。请确认 Codex 桌面端已安装"
        "(%LOCALAPPDATA%\\OpenAI\\Codex\\bin)，或用 --codex-bin 指定 codex.exe 的完整路径。"
    )


class WindowsCodexAppServer(CodexAppServer):
    """Own one app-server stdio session; the desktop can still show its task history."""

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.process: subprocess.Popen[str] | None = None
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._reader: threading.Thread | None = None
        self._stderr_reader: threading.Thread | None = None

    def start(self) -> None:
        if not self.workspace.is_dir():
            raise CodexProtocolError(f"Codex 工作目录不存在：{self.workspace}")
        self.executable = find_codex_executable(self.executable)
        try:
            self.process = subprocess.Popen(
                [self.executable, "app-server", "--listen", "stdio://"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
        except OSError as error:
            raise CodexProtocolError("Windows 无法启动 Codex app-server") from error

        self.socket = self  # Parent helpers only use this as a connected sentinel.
        self._reader = threading.Thread(target=self._read_stdout, daemon=True)
        self._reader.start()
        self._stderr_reader = threading.Thread(target=self._read_stderr, daemon=True)
        self._stderr_reader.start()
        self._request(
            "initialize",
            {
                "clientInfo": {
                    "name": "folos-coding-companion-windows",
                    "title": "FoloOS 编程伴侣 Windows",
                    "version": "1.0",
                },
                "capabilities": {"experimentalApi": True},
            },
        )
        self._write({"method": "initialized", "params": {}})

    def _read_stdout(self) -> None:
        process = self.process
        if process is None or process.stdout is None:
            return
        for line in process.stdout:
            self._lines.put(line)
        self._lines.put(None)

    def _read_stderr(self) -> None:
        process = self.process
        if process is None or process.stderr is None:
            return
        for line in process.stderr:
            if self.verbose and line.strip():
                print(f"Codex 日志  {line.rstrip()}")

    def _write(self, message: dict[str, Any]) -> None:
        process = self.process
        if process is None or process.stdin is None or process.poll() is not None:
            self.socket = None
            raise CodexProtocolError("Codex app-server 未运行")
        if self.verbose:
            print(f"桥接 → Codex  {message}")
        try:
            process.stdin.write(
                json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n"
            )
            process.stdin.flush()
        except (OSError, BrokenPipeError) as error:
            self.socket = None
            raise CodexProtocolError("Codex app-server 连接已断开") from error

    def _read_one(self, timeout: float) -> bool:
        if self.process is None:
            return False
        try:
            line = self._lines.get(timeout=max(0.0, timeout))
        except queue.Empty:
            return False
        if line is None:
            self.socket = None
            raise CodexProtocolError("Codex app-server 已退出")
        return self._process_wire_message(line.encode("utf-8"))

    def _resume_thread(self, thread_key: str) -> list[dict[str, Any]]:
        thread = self.threads.get(thread_key)
        if thread is None or not thread.get("thread_id"):
            return [{"type": "error", "text": "选择的 Codex 任务已经失效"}]
        # ⚠ 刻意**不**调用 `thread/resume` RPC:
        #   1) 它会打开该会话的写锁, 与桌面端 Codex 冲突 ->
        #      "failed to open thread writer lock"(实测);
        #   2) 它会把这整个会话加载进 app-server, 会话很长时开销很大 ——
        #      而用户只要"看最后一段话", 完全不需要整段对话(设备也扛不住)。
        # 因此选择纯本地完成: 只记 id/title + 定位 rollout 文件基线,
        # 需要的内容(最后一段文本)直接读 rollout 文件尾部(见 read_last_message)。
        self.thread_id = str(thread["thread_id"])
        self.thread_title = str(thread["title"])
        self.router = CodexEventRouter()
        self.queued_submission_id = None
        self._observed_turn_id = None
        self._observed_turn_status = ""
        self._observed_thread_flags = ()
        self._set_rollout_baseline(thread.get("path"))
        if self.thread_opener is not None:
            try:
                self.thread_opener(self.thread_id)
            except OSError:
                pass
        # 只下发"最后一段文本"(用户明确要求, 避免设备加载整段对话而卡顿)。
        last_message = read_last_message(thread.get("path"), thread_id=self.thread_id)
        return self._selection_messages(last_message)

    def _start_command(self, command: str) -> None:
        if self.thread_id is None:
            raise CodexProtocolError("Codex 任务尚未初始化")
        cwd = str((self.selected_project or {}).get("cwd") or self.workspace)
        result = self._request(
            "turn/start",
            {
                "threadId": self.thread_id,
                "input": [{"type": "text", "text": command}],
                "cwd": cwd,
                "approvalPolicy": "onRequest",
                "sandboxPolicy": {
                    "type": "workspaceWrite",
                    "writableRoots": [cwd],
                    "networkAccess": False,
                },
            },
        )
        turn = result.get("turn") if isinstance(result.get("turn"), dict) else result
        if isinstance(turn, dict):
            self.router.active_turn_id = str(turn.get("id") or "") or None

    def handle_device_event(self, event: dict[str, Any]) -> list[dict[str, Any]]:
        if event.get("type") == "cancel_task" and self.thread_id:
            turn_id = self.router.active_turn_id
            if turn_id:
                self._request(
                    "turn/interrupt",
                    {"threadId": self.thread_id, "turnId": turn_id},
                )
                return [{"type": "task_status", "text": "正在停止 Codex 任务"}]
        return super().handle_device_event(event)

    def poll(self) -> list[dict[str, Any]]:
        if self.process is None or self.process.poll() is not None:
            return []
        while self._read_one(0.0):
            pass
        self._poll_rollout(time.monotonic())
        self._device_outbox.extend(self.task_monitor.poll(
            force=any(m.get("type") in ("done", "error") for m in self._device_outbox)
        ))
        messages = list(self._device_outbox)
        self._device_outbox.clear()
        return messages

    def close(self) -> None:
        process = self.process
        if process is None:
            return
        self.router.cancel_pending_approval()
        try:
            self._flush_router_rpc()
        except CodexProtocolError:
            pass
        try:
            if process.stdin is not None:
                process.stdin.close()
            process.terminate()
            process.wait(timeout=3)
        except (OSError, subprocess.TimeoutExpired):
            process.kill()
        self.process = None
        self.socket = None
