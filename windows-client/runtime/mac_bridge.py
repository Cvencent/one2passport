#!/usr/bin/env python3
"""USB or local Wi-Fi bridge between FoloOS Coding Companion and a Mac.

The device streams 16 kHz mono PCM as small base64 JSON chunks.  This bridge
writes a WAV file, asks macOS Speech for Chinese transcription, then returns the
text to the device for explicit review.  After that review, the bridge can run
either the deterministic mock or a real local Codex task.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import csv
from datetime import date
import getpass
import glob
import heapq
import hashlib
import html
import json
import os
from pathlib import Path
import secrets
import select
import shutil
import socket
import subprocess
import sys
try:
    import termios
except ImportError:  # Windows uses the PowerShell SerialPort provisioning helper.
    termios = None
import tempfile
import threading
import time
import wave
from typing import Any, Callable

if __package__:
    from .codex_backend import CodexAppServer, CodexProtocolError
else:
    from codex_backend import CodexAppServer, CodexProtocolError


HEARTBEAT_SECONDS = 5.0
MAX_RX_BUFFER = 8192
MAX_AUDIO_BYTES = 16000 * 2 * 120
DISCOVERY_PORT = 8764
BRIDGE_PORT = 8765
DISCOVERY_REQUEST = b"FOLOOS_DISCOVER_V1"
if os.name == "nt":
    _FOLOOS_DATA_DIR = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "FoloOS"
else:
    _FOLOOS_DATA_DIR = Path.home() / "Library" / "Application Support" / "FoloOS"
WIRELESS_CONFIG_PATH = _FOLOOS_DATA_DIR / "bridge.json"
WORD_BEAR_DATA_DIR = _FOLOOS_DATA_DIR / "WordBear"
OTA_CHUNK_BYTES = 384
OTA_CHUNK_DELAY_SECONDS = 0.002
OTA_MAX_BYTES = 3 * 1024 * 1024
TEXT_CHUNK_BYTES = 480
SPEECH_CHUNK_BYTES = 384
SPEECH_TIMEOUT_SECONDS = 15.0
LONG_TEXT_TYPES = {"transcript", "done", "error"}


def open_codex_thread(thread_id: str) -> None:
    """Show the same task in Codex Desktop after the device submits speech."""
    if os.name == "nt":
        os.startfile(f"codex://threads/{thread_id}")
        return
    subprocess.Popen(
        ["/usr/bin/open", f"codex://threads/{thread_id}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def encode_message(message: dict[str, Any]) -> bytes:
    """Encode exactly one UTF-8 NDJSON record."""
    return (
        json.dumps(message, ensure_ascii=False, separators=(",", ":"))
        .replace("\n", "\\n")
        .encode("utf-8")
        + b"\n"
    )


def split_utf8_text(text: str, maximum_bytes: int = TEXT_CHUNK_BYTES) -> list[str]:
    """Split without cutting a multi-byte character."""
    chunks: list[str] = []
    current: list[str] = []
    current_bytes = 0
    for char in text:
        width = len(json.dumps(char, ensure_ascii=False)[1:-1].encode("utf-8"))
        if current and current_bytes + width > maximum_bytes:
            chunks.append("".join(current))
            current = []
            current_bytes = 0
        current.append(char)
        current_bytes += width
    if current:
        chunks.append("".join(current))
    return chunks


def wire_messages(message: dict[str, Any]) -> list[bytes]:
    """Keep every NDJSON record below the device line-buffer limit."""
    encoded = encode_message(message)
    message_type = str(message.get("type") or "")
    text = message.get("text")
    if message_type not in LONG_TEXT_TYPES or not isinstance(text, str) or len(encoded) <= 700:
        return [encoded]
    begin = {"type": "text_begin", "target": message_type}
    for key in ("thread_id", "turn_id", "request_id"):
        if key in message:
            begin[key] = message[key]
    records = [encode_message(begin)]
    records.extend(
        encode_message(
            {"type": "text_chunk", "target": message_type, "text": chunk}
        )
        for chunk in split_utf8_text(text)
    )
    records.append(encode_message({"type": "text_end", "target": message_type}))
    if "request_id" in message:
        records = [encode_message({**json.loads(record),
                                   "request_id": message["request_id"]})
                   for record in records]
    return records


class MockScenario:
    """Deterministic task/approval responses used after voice confirmation."""

    def __init__(self, transcript: str | None = None):
        self.transcript = transcript

    def handle(self, event: dict[str, Any]) -> list[tuple[float, dict[str, Any]]]:
        event_type = event.get("type")
        if event_type == "record_stop" and self.transcript:
            return [
                (0.25, {"type": "transcript", "text": self.transcript}),
            ]
        if event_type == "voice_command":
            command = str(event.get("text") or self.transcript or "")
            return [
                (0.0, {"type": "task_status", "text": "已收到指令"}),
                (0.7, {"type": "task_status", "text": f"正在分析：{command}"}),
                (
                    1.5,
                    {
                        "type": "approval",
                        "question": "允许执行构建命令吗？",
                        "detail": "模拟操作：运行项目构建，不会删除或覆盖文件。",
                    },
                ),
            ]
        if event_type == "approval_decision":
            decision = str(event.get("decision") or "deny")
            if decision in {"approve_once", "approve_session"}:
                return [
                    (0.0, {"type": "task_status", "text": "审批已确认，正在执行"}),
                    (0.8, {"type": "done", "text": "模拟任务已完成"}),
                ]
            if decision == "cancel_task":
                return [(0.0, {"type": "done", "text": "任务已取消"})]
            return [(0.0, {"type": "done", "text": "本次操作已拒绝"})]
        return []


class AudioCapture:
    """Collect one bounded PCM stream and persist it as a standard WAV file."""

    def __init__(self, output_dir: Path):
        self.output_dir = output_dir
        self.pcm = bytearray()
        self.sample_rate = 16000
        self.channels = 1
        self.bits = 16
        self.active = False

    def start(self, event: dict[str, Any]) -> None:
        sample_rate = int(event.get("sample_rate") or 16000)
        channels = int(event.get("channels") or 1)
        bits = int(event.get("bits") or 16)
        if not 8000 <= sample_rate <= 48000 or channels != 1 or bits != 16:
            raise ValueError("设备发来了不支持的音频格式")
        self.sample_rate = sample_rate
        self.channels = channels
        self.bits = bits
        self.pcm.clear()
        self.active = True

    def add_chunk(self, event: dict[str, Any]) -> None:
        if not self.active:
            return
        encoded = event.get("data")
        if not isinstance(encoded, str):
            raise ValueError("音频分片缺少数据")
        try:
            chunk = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error) as error:
            raise ValueError("音频分片损坏") from error
        if len(self.pcm) + len(chunk) > MAX_AUDIO_BYTES:
            raise ValueError("录音超过 120 秒限制")
        self.pcm.extend(chunk)

    def finish(self) -> Path:
        self.active = False
        if len(self.pcm) < self.sample_rate * 2 // 5:
            raise ValueError("录音太短，请至少说 1 秒后再按一下结束")
        if len(self.pcm) % 2:
            del self.pcm[-1]
        self.output_dir.mkdir(parents=True, exist_ok=True)
        path = self.output_dir / "last-command.wav"
        with wave.open(str(path), "wb") as wav_file:
            wav_file.setnchannels(self.channels)
            wav_file.setsampwidth(self.bits // 8)
            wav_file.setframerate(self.sample_rate)
            wav_file.writeframes(self.pcm)
        return path

    @property
    def duration_seconds(self) -> float:
        bytes_per_second = self.sample_rate * self.channels * (self.bits // 8)
        return len(self.pcm) / bytes_per_second if bytes_per_second else 0.0

    def cancel(self) -> None:
        self.active = False
        self.pcm.clear()


class MacSpeechRecognizer:
    """Compile and run the small macOS Speech framework helper."""

    def __init__(self, tools_dir: Path, locale: str = "zh-CN"):
        self.tools_dir = tools_dir
        self.locale = locale
        self.source = tools_dir / "mac_speech_helper.m"
        self.info_plist = tools_dir / "mac_speech_helper-Info.plist"
        self.app_bundle = tools_dir.parent / "build" / "FoloOS Speech Helper.app"
        self.binary = self.app_bundle / "Contents" / "MacOS" / "mac_speech_helper"
        self.bundle_info = self.app_bundle / "Contents" / "Info.plist"

    def ensure_helper(self) -> None:
        newest_source = max(self.source.stat().st_mtime, self.info_plist.stat().st_mtime)
        if self.binary.exists() and self.binary.stat().st_mtime >= newest_source:
            verified = subprocess.run(
                ["/usr/bin/codesign", "--verify", "--deep", "--strict", str(self.app_bundle)],
                capture_output=True,
            )
            if verified.returncode == 0:
                return
        self.binary.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(self.info_plist, self.bundle_info)
        command = [
            "/usr/bin/clang",
            "-fobjc-arc",
            str(self.source),
            "-framework",
            "Speech",
            "-framework",
            "Foundation",
            "-fmodules-cache-path=" + str(self.binary.parent / "module-cache"),
            "-o",
            str(self.binary),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            lines = [line for line in result.stderr.strip().splitlines() if line.strip()]
            detail = lines[0] if lines else "未知错误"
            raise RuntimeError(f"无法编译 macOS 语音识别助手：{detail}")
        signed = subprocess.run(
            [
                "/usr/bin/codesign",
                "--force",
                "--deep",
                "--sign",
                "-",
                str(self.app_bundle),
            ],
            capture_output=True,
            text=True,
        )
        if signed.returncode != 0:
            raise RuntimeError("无法为 macOS 语音识别助手完成本地签名")
        launch_services = (
            "/System/Library/Frameworks/CoreServices.framework/Frameworks/"
            "LaunchServices.framework/Support/lsregister"
        )
        subprocess.run([launch_services, "-f", str(self.app_bundle)], capture_output=True)

    def start(self, wav_path: Path) -> subprocess.Popen[str]:
        self.ensure_helper()
        return subprocess.Popen(
            [str(self.binary), str(wav_path), self.locale],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    @staticmethod
    def result(process: subprocess.Popen[str]) -> tuple[bool, str]:
        stdout, stderr = process.communicate()
        if process.returncode == 0 and stdout.strip():
            return True, stdout.strip().splitlines()[-1]
        detail = stderr.strip().splitlines()[-1] if stderr.strip() else "未识别到文字"
        return False, detail


class MacSpeechSynthesizer:
    """Use macOS local voices and return 16 kHz mono PCM for the device."""

    sample_rate = 16000

    def iter_pcm(self, text: str, chunk_bytes: int = SPEECH_CHUNK_BYTES):
        """Yield bounded PCM chunks instead of retaining the answer audio."""
        spoken = str(text or "").strip()
        if not spoken:
            return
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "answer.caf"
            output = Path(temp_dir) / "answer.wav"
            say = subprocess.run(
                [
                    "/usr/bin/say",
                    "-v",
                    "Tingting",
                    "--data-format=LEI16@16000",
                    "-o",
                    str(source),
                    spoken,
                ],
                capture_output=True,
                text=True,
            )
            if say.returncode != 0:
                say = subprocess.run(
                    [
                        "/usr/bin/say",
                        "--data-format=LEI16@16000",
                        "-o",
                        str(source),
                        spoken,
                    ],
                    capture_output=True,
                    text=True,
                )
            if say.returncode != 0:
                raise RuntimeError("macOS 无法生成回答语音")
            converted = subprocess.run(
                [
                    "/usr/bin/afconvert",
                    "-f",
                    "WAVE",
                    "-d",
                    "LEI16@16000",
                    "-c",
                    "1",
                    str(source),
                    str(output),
                ],
                capture_output=True,
                text=True,
            )
            if converted.returncode != 0:
                raise RuntimeError("macOS 无法转换回答语音")
            with wave.open(str(output), "rb") as wav_file:
                if wav_file.getnchannels() != 1 or wav_file.getsampwidth() != 2:
                    raise RuntimeError("回答语音格式不受设备支持")
                frames_per_chunk = max(1, chunk_bytes // 2)
                while True:
                    chunk = wav_file.readframes(frames_per_chunk)
                    if not chunk:
                        break
                    yield chunk

    def iter_word_pcm(self, word: str, chunk_bytes: int = SPEECH_CHUNK_BYTES):
        """Generate one English word and stream it without retaining PCM."""
        spoken = str(word or "").strip()
        if not spoken:
            return
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "word.caf"
            output = Path(temp_dir) / "word.wav"
            say = subprocess.run(
                [
                    "/usr/bin/say",
                    "-v",
                    "Samantha",
                    "--data-format=LEI16@16000",
                    "-o",
                    str(source),
                    spoken,
                ],
                capture_output=True,
                text=True,
            )
            if say.returncode != 0:
                say = subprocess.run(
                    [
                        "/usr/bin/say",
                        "--data-format=LEI16@16000",
                        "-o",
                        str(source),
                        spoken,
                    ],
                    capture_output=True,
                    text=True,
                )
            if say.returncode != 0:
                raise RuntimeError("macOS 无法生成单词发音")
            converted = subprocess.run(
                [
                    "/usr/bin/afconvert",
                    "-f",
                    "WAVE",
                    "-d",
                    "LEI16@16000",
                    "-c",
                    "1",
                    str(source),
                    str(output),
                ],
                capture_output=True,
                text=True,
            )
            if converted.returncode != 0:
                raise RuntimeError("macOS 无法转换单词发音")
            with wave.open(str(output), "rb") as wav_file:
                frames_per_chunk = max(1, chunk_bytes // 2)
                while True:
                    chunk = wav_file.readframes(frames_per_chunk)
                    if not chunk:
                        break
                    yield chunk


class WordBearStore:
    """Durable, dependency-free desktop mirror of device learning progress."""

    CATEGORY_NAMES = {
        "unlearned": "未学习",
        "learning": "学习中",
        "due": "待复习",
        "wrong": "错词",
        "mastered": "已掌握",
    }

    def __init__(self, root: Path | None = None):
        self.root = root or WORD_BEAR_DATA_DIR
        self.data_path = self.root / "word_bear.json"
        self.backup_path = self.root / "word_bear.backup.json"
        self.events_path = self.root / "word_bear_events.jsonl"
        self.report_path = self.root / "word_bear_report.html"
        self.csv_path = self.root / "word_bear_export.csv"
        self.export_json_path = self.root / "word_bear_export.json"
        self.data = self._load_recoverable()
        self.pending: dict[str, Any] | None = None

    @staticmethod
    def _valid(data: Any) -> bool:
        if not isinstance(data, dict) or data.get("schema") != 2:
            return False
        words = data.get("words")
        return isinstance(words, list) and all(
            isinstance(item, dict) and isinstance(item.get("id"), int)
            for item in words
        )

    @staticmethod
    def _read_json(path: Path) -> dict[str, Any] | None:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return None
        return data if WordBearStore._valid(data) else None

    def _load_recoverable(self) -> dict[str, Any]:
        current = self._read_json(self.data_path)
        if current is not None:
            return current
        exported = self._read_json(self.export_json_path)
        if exported is not None:
            return exported
        backup = self._read_json(self.backup_path)
        if backup is not None:
            return backup
        return {
            "schema": 2,
            "revision": -1,
            "day": 0,
            "active_group": 0,
            "words": [],
        }

    @staticmethod
    def _atomic_text(path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8", newline="") as file:
                file.write(text)
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    @classmethod
    def _atomic_json(cls, path: Path, data: dict[str, Any]) -> None:
        cls._atomic_text(
            path,
            json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        )

    @staticmethod
    def _category(item: dict[str, Any], today: int) -> str:
        learn = int(item.get("learn") or 0)
        flags = int(item.get("flags") or 0)
        due = int(item.get("due_day") or 0)
        if learn == 0:
            return "unlearned"
        if flags & 2:
            return "wrong"
        if flags & 1:
            return "mastered"
        if due and due <= today:
            return "due"
        return "learning"

    @staticmethod
    def _day_text(day_number: int) -> str:
        try:
            return date.fromordinal(day_number).isoformat() if day_number else "--"
        except ValueError:
            return "--"

    @classmethod
    def _suggestion(cls, item: dict[str, Any], today: int) -> str:
        category = cls._category(item, today)
        due = int(item.get("due_day") or 0)
        if category == "unlearned":
            return "安排进入新词学习"
        if category == "wrong":
            return "优先错词复习，连续答对3次移出"
        if category == "due":
            return "今天完成复习"
        if category == "mastered" and due and due <= today:
            return "今天做一次掌握复查"
        if due:
            return f"建议 {cls._day_text(due)} 复习"
        return "继续学习，联网后校准日期"

    def _write_csv(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8-sig", newline="") as file:
                writer = csv.writer(file)
                writer.writerow(
                    [
                        "分类", "组", "单词", "词性", "释义", "例句",
                        "学习次数", "正确", "错误", "连续正确", "最后学习",
                        "下次复习", "历史错词", "复习建议",
                    ]
                )
                today = date.today().toordinal()
                for item in self.data.get("words", []):
                    category = self._category(item, today)
                    writer.writerow(
                        [
                            self.CATEGORY_NAMES[category],
                            int(item.get("group") or 0) + 1,
                            item.get("word", ""), item.get("part", ""),
                            item.get("meaning", ""), item.get("example", ""),
                            item.get("learn", 0), item.get("correct", 0),
                            item.get("wrong", 0), item.get("streak", 0),
                            self._day_text(int(item.get("last_day") or 0)),
                            self._day_text(int(item.get("due_day") or 0)),
                            "是" if int(item.get("flags") or 0) & 4 else "否",
                            self._suggestion(item, today),
                        ]
                    )
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def _report_html(self) -> str:
        today = date.today().toordinal()
        groups: dict[str, list[dict[str, Any]]] = {
            key: [] for key in self.CATEGORY_NAMES
        }
        for item in self.data.get("words", []):
            groups[self._category(item, today)].append(item)
        summary = "".join(
            f"<div><b>{len(groups[key])}</b><span>{name}</span></div>"
            for key, name in self.CATEGORY_NAMES.items()
        )
        sections: list[str] = []
        for key, name in self.CATEGORY_NAMES.items():
            rows = []
            for item in groups[key]:
                flags = int(item.get("flags") or 0)
                rows.append(
                    "<tr>"
                    f"<td>{int(item.get('group') or 0) + 1}</td>"
                    f"<td><strong>{html.escape(str(item.get('word') or ''))}</strong>"
                    f"<small>{html.escape(str(item.get('part') or ''))}</small></td>"
                    f"<td>{html.escape(str(item.get('meaning') or ''))}</td>"
                    f"<td>{html.escape(str(item.get('example') or ''))}</td>"
                    f"<td>{item.get('learn', 0)} / {item.get('correct', 0)} / "
                    f"{item.get('wrong', 0)}</td>"
                    f"<td>{item.get('streak', 0)}</td>"
                    f"<td>{self._day_text(int(item.get('last_day') or 0))}</td>"
                    f"<td>{'是' if flags & 4 else '否'}</td>"
                    f"<td>{html.escape(self._suggestion(item, today))}</td>"
                    "</tr>"
                )
            sections.append(
                f"<section><h2>{name}<em>{len(rows)}</em></h2>"
                "<div class=table><table><thead><tr><th>组</th><th>单词</th>"
                "<th>释义</th><th>例句/用法</th><th>学/对/错</th><th>连对</th>"
                "<th>最后学习</th><th>历史错词</th><th>建议</th></tr></thead>"
                f"<tbody>{''.join(rows)}</tbody></table></div></section>"
            )
        return f"""<!doctype html><html lang=zh-CN><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>单词熊学习整理</title><style>
*{{box-sizing:border-box}}body{{margin:0;background:#0b1220;color:#edf5ff;
font:15px system-ui,sans-serif}}main{{max-width:1440px;margin:auto;padding:32px}}
h1{{margin:0 0 8px;font-size:30px}}p{{color:#9fb0c6}}.summary{{display:grid;
grid-template-columns:repeat(5,minmax(120px,1fr));gap:12px;margin:24px 0}}
.summary div{{background:#172337;border:1px solid #2a3b55;padding:16px}}
.summary b{{display:block;color:#5fe6c7;font-size:28px}}.summary span{{color:#b8c8da}}
section{{margin:28px 0}}h2{{display:flex;gap:10px;align-items:center}}h2 em{{font-size:13px;
font-style:normal;color:#0b1220;background:#5fe6c7;padding:2px 8px}}.table{{overflow:auto}}
table{{width:100%;border-collapse:collapse;background:#111c2d}}th,td{{padding:11px;
border:1px solid #293a52;text-align:left;vertical-align:top}}th{{color:#5fe6c7;
white-space:nowrap}}td small{{display:block;color:#91a4bc;margin-top:3px}}a{{color:#5fe6c7}}
@media(max-width:760px){{main{{padding:18px}}.summary{{grid-template-columns:repeat(2,1fr)}}}}
</style><main><h1>单词熊学习整理</h1>
<p>设备修订 {self.data.get('revision', 0)} · 最近设备日期
{self._day_text(int(self.data.get('day') or 0))} · 数据保存在本机，可离线打开。</p>
<p><a href="word_bear_export.csv">下载 CSV</a> ·
<a href="word_bear_export.json">下载 JSON</a></p>
<div class=summary>{summary}</div>{''.join(sections)}</main></html>"""

    def _append_event(self, event: dict[str, Any]) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        record = {
            "received_at": int(time.time()),
            "type": event.get("type"),
            "revision": event.get("revision"),
            "id": event.get("id"),
            "payload": event,
        }
        with self.events_path.open("a", encoding="utf-8") as file:
            file.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")))
            file.write("\n")

    def _save(self, event: dict[str, Any]) -> None:
        previous = self._read_json(self.data_path)
        if previous is not None:
            self._atomic_json(self.backup_path, previous)
        self._atomic_json(self.data_path, self.data)
        self._atomic_json(self.export_json_path, self.data)
        self._write_csv(self.csv_path)
        self._atomic_text(self.report_path, self._report_html())
        self._append_event(event)

    def handle(self, event: dict[str, Any]) -> bool:
        event_type = event.get("type")
        if event_type == "word_bear_sync_begin":
            count = int(event.get("count") or 0)
            revision = int(event.get("revision") or 0)
            if event.get("schema") != 2 or count <= 0 or count > 2000:
                self.pending = None
                return False
            self.pending = {
                "schema": 2,
                "revision": revision,
                "day": int(event.get("day") or 0),
                "active_group": int(event.get("active_group") or 0),
                "count": count,
                "items": {},
            }
            return True
        if event_type == "word_bear_sync_item":
            if self.pending is None or int(event.get("revision") or -1) != self.pending["revision"]:
                return False
            item_id = int(event.get("id") or 0)
            if item_id < 0 or item_id >= self.pending["count"]:
                return False
            self.pending["items"][item_id] = dict(event)
            return True
        if event_type == "word_bear_sync_end":
            if self.pending is None or int(event.get("revision") or -1) != self.pending["revision"]:
                return False
            if len(self.pending["items"]) != self.pending["count"]:
                self.pending = None
                return False
            self.data = {
                "schema": 2,
                "revision": self.pending["revision"],
                "day": self.pending["day"],
                "active_group": self.pending["active_group"],
                "words": [
                    self.pending["items"][item_id]
                    for item_id in range(self.pending["count"])
                ],
            }
            self.pending = None
            self._save(event)
            return True
        if event_type == "word_bear_progress":
            revision = int(event.get("revision") or 0)
            if revision <= int(self.data.get("revision") or -1):
                return True
            item_id = int(event.get("id", -1))
            words = self.data.get("words", [])
            if item_id < 0 or item_id >= len(words):
                return False
            words[item_id] = dict(event)
            self.data["revision"] = revision
            self._save(event)
            return True
        return False

    def export_to(self, destination: Path) -> tuple[Path, Path]:
        if not self.data.get("words"):
            raise RuntimeError("尚未收到单词熊设备数据")
        destination.mkdir(parents=True, exist_ok=True)
        json_path = destination / "word_bear_export.json"
        csv_path = destination / "word_bear_export.csv"
        self._atomic_json(json_path, self.data)
        self._write_csv(csv_path)
        return json_path, csv_path


def detect_port() -> str:
    ports = sorted(
        port
        for port in glob.glob("/dev/cu.usbmodem*")
        if "wlan" not in port.lower() and "bluetooth" not in port.lower()
    )
    if not ports:
        raise RuntimeError("没有找到 /dev/cu.usbmodem*，请连接设备或使用 --port 指定串口")
    if len(ports) > 1:
        choices = "\n".join(f"  {port}" for port in ports)
        raise RuntimeError(f"检测到多个 USB 串口，请使用 --port 指定：\n{choices}")
    return ports[0]


def configure_serial(fd: int) -> None:
    if termios is None:
        raise RuntimeError("Windows 请使用配套的 PowerShell 配网工具")
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcflush(fd, termios.TCIOFLUSH)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def save_wireless_config(
    host: str, token: str, port: int = BRIDGE_PORT, path: Path = WIRELESS_CONFIG_PATH
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps({"host": host, "port": port, "token": token}, indent=2),
        encoding="utf-8",
    )
    os.chmod(path, 0o600)


def load_wireless_config(
    path: Path = WIRELESS_CONFIG_PATH,
) -> tuple[str, int, str] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        host = str(data.get("host") or "")
        port = int(data.get("port") or BRIDGE_PORT)
        token = str(data.get("token") or "")
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        return None
    if not 1 <= port <= 65535 or len(token) < 16:
        return None
    return host, port, token


def discover_wireless_device(timeout: float = 2.5) -> tuple[str, int]:
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        udp.settimeout(timeout)
        udp.sendto(DISCOVERY_REQUEST, ("255.255.255.255", DISCOVERY_PORT))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            payload, source = udp.recvfrom(1024)
            try:
                reply = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if reply.get("device") == "FoloOS-AI-Passport":
                return source[0], int(reply.get("port") or BRIDGE_PORT)
    except socket.timeout:
        pass
    finally:
        udp.close()
    raise RuntimeError("局域网内没有发现已配网的 AI Passport")


def validate_ota_image(path: Path) -> bytes:
    try:
        image = path.read_bytes()
    except OSError as error:
        raise RuntimeError(f"无法读取固件：{error}") from error
    if not image or image[0] != 0xE9:
        raise RuntimeError("这不是 ESP32 应用固件；请选择 FoloToy-AI-Passport.bin")
    if len(image) > OTA_MAX_BYTES:
        raise RuntimeError("固件超过 3MB OTA 分区")
    return image


def receive_ota_event(
    sock: socket.socket, buffer: bytearray, timeout: float = 30.0
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        while b"\n" in buffer:
            raw, _, remainder = buffer.partition(b"\n")
            buffer[:] = remainder
            try:
                event = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if isinstance(event, dict) and event.get("type") == "ota_status":
                return event
        sock.settimeout(max(0.1, deadline - time.monotonic()))
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            raise RuntimeError("设备在升级完成前断开连接")
        buffer.extend(chunk)
    raise RuntimeError("等待设备 OTA 响应超时")


def wait_for_wireless_reconnect(
    host: str, port: int, token: str, timeout: float = 40.0
) -> bool:
    """在 OTA 重启丢失最后回执时，用重新认证确认设备已恢复运行。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        probe = NetworkBridge(host, port, token, MockScenario())
        try:
            probe.open()
            return True
        except RuntimeError:
            time.sleep(1.0)
        finally:
            probe.close()
    return False


def wireless_ota_update(path: Path) -> None:
    image = validate_ota_image(path)
    wireless = load_wireless_config()
    if wireless is None:
        raise RuntimeError("没有无线配对信息，请先完成一次无线配置")
    host, port, token = wireless
    bridge = NetworkBridge(host, port, token, MockScenario())
    try:
        bridge.open()
        assert bridge.sock is not None
        buffer = bytearray()
        bridge.send({"type": "ota_begin", "size": len(image)})
        ready = receive_ota_event(bridge.sock, buffer)
        if ready.get("status") == "error":
            raise RuntimeError(str(ready.get("text") or "设备拒绝 OTA"))

        digest = hashlib.sha256(image).hexdigest()
        print(f"正在无线写入 {path.name}（{len(image) / 1024:.1f}KB）")
        for offset in range(0, len(image), OTA_CHUNK_BYTES):
            chunk = image[offset : offset + OTA_CHUNK_BYTES]
            try:
                bridge.send(
                    {
                        "type": "ota_chunk",
                        "data": base64.b64encode(chunk).decode("ascii"),
                    }
                )
            except OSError as error:
                progress = int(offset * 100 / len(image))
                raise RuntimeError(
                    f"设备在接收固件约 {progress}% 时中断：{error}"
                ) from error
            # Give the ESP32-C3 time to decode and commit each flash block.
            # Sending a multi-megabyte image at socket speed can starve the
            # receiver and make the device close the connection mid-update.
            time.sleep(OTA_CHUNK_DELAY_SECONDS)
        bridge.send({"type": "ota_end"})

        shown = -1
        while True:
            try:
                event = receive_ota_event(bridge.sock, buffer, 20.0)
            except RuntimeError:
                if shown < 100:
                    raise
                # ESP32 重启会立即中断 TCP，最后一条 complete
                # 可能已发出但没有到达 Mac。写入 100% 后要求设备
                # 重新完成密钥认证，避免把普通断线当作成功。
                bridge.close()
                print("完成回执在重启时中断，正在确认设备重新上线……")
                if wait_for_wireless_reconnect(
                    bridge.host, bridge.network_port, bridge.token
                ):
                    print(f"无线升级完成，设备已重启上线。SHA-256：{digest}")
                    return
                raise RuntimeError("固件已写入 100%，但设备重启后未恢复无线连接")
            status = str(event.get("status") or "")
            progress = int(event.get("progress") or 0)
            if progress != shown:
                print(f"设备写入进度：{progress}%")
                shown = progress
            if status == "error":
                raise RuntimeError(str(event.get("text") or "OTA 写入失败"))
            if status == "complete":
                print(f"无线升级完成，设备正在重启。SHA-256：{digest}")
                return
    finally:
        bridge.close()


def load_word_bear_restore(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"单词熊恢复文件无效：{error}") from error
    if not WordBearStore._valid(data):
        raise RuntimeError("单词熊恢复文件 schema 或词条结构无效")
    words = data.get("words", [])
    if len(words) != 100 or sorted(item.get("id") for item in words) != list(range(100)):
        raise RuntimeError("首版恢复文件必须完整包含 id 0～99 的 100 个词")
    return data


def wireless_word_bear_restore(path: Path) -> None:
    data = load_word_bear_restore(path)
    config = load_wireless_config()
    if config is None:
        raise RuntimeError("尚未配置无线桥接，不能恢复单词熊数据")
    host, port, token = config
    with tempfile.TemporaryDirectory() as temp_dir:
        confirmation = WordBearStore(Path(temp_dir))
        bridge = NetworkBridge(
            host, port, token, word_bear_store=confirmation
        )
        try:
            bridge.open()
            words = sorted(data["words"], key=lambda item: item["id"])
            bridge.send(
                {
                    "type": "word_bear_restore_begin",
                    "schema": 2,
                    "count": len(words),
                    "day": int(data.get("day") or 0),
                    "active_group": int(data.get("active_group") or 0),
                }
            )
            for item in words:
                bridge.send(
                    {
                        "type": "word_bear_restore_item",
                        "id": item["id"],
                        "learn": int(item.get("learn") or 0),
                        "correct": int(item.get("correct") or 0),
                        "wrong": int(item.get("wrong") or 0),
                        "last_day": int(item.get("last_day") or 0),
                        "due_day": int(item.get("due_day") or 0),
                        "streak": int(item.get("streak") or 0),
                        "flags": int(item.get("flags") or 0),
                    }
                )
            bridge.send({"type": "word_bear_restore_end"})

            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline:
                assert bridge.sock is not None
                readable, _, _ = select.select([bridge.sock], [], [], 0.25)
                if readable:
                    bridge.read_available()
                    if len(confirmation.data.get("words", [])) == len(words):
                        print("单词熊数据已恢复，并收到设备完整回读确认")
                        return
            raise RuntimeError("设备未在 8 秒内回读恢复后的完整数据")
        finally:
            bridge.close()


class SerialBridge:
    def __init__(
        self,
        port: str,
        scenario: MockScenario | None = None,
        speech: MacSpeechRecognizer | None = None,
        tts: MacSpeechSynthesizer | None = None,
        codex: CodexAppServer | None = None,
        recordings_dir: Path | None = None,
        word_bear_store: WordBearStore | None = None,
        verbose: bool = False,
        clock: Callable[[], float] = time.monotonic,
        heartbeat_seconds: float = HEARTBEAT_SECONDS,
    ):
        self.port = port
        self.scenario = scenario
        self.speech = speech
        self.tts = tts
        self.codex = codex
        self.verbose = verbose
        self.clock = clock
        self.heartbeat_seconds = heartbeat_seconds
        self.fd: int | None = None
        self._send_lock = threading.Lock()
        self._heartbeat_stop = threading.Event()
        self._heartbeat_thread: threading.Thread | None = None
        self._heartbeat_error: Exception | None = None
        self.rx_buffer = bytearray()
        self.pending: list[tuple[float, int, dict[str, Any]]] = []
        self.sequence = 0
        default_recordings = (
            Path(os.environ.get("LOCALAPPDATA", Path.home())) / "FoloOS" / "recordings"
            if os.name == "nt"
            else Path.home() / "Library" / "Caches" / "FoloOS"
        )
        self.capture = AudioCapture(recordings_dir or default_recordings)
        self.word_bear = word_bear_store or WordBearStore()
        self.speech_process: subprocess.Popen[str] | None = None
        self.speech_started_at = 0.0
        self.capture_cancelled = False

    def open(self) -> None:
        self.fd = os.open(
            self.port,
            os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK,
        )
        configure_serial(self.fd)

    def close(self) -> None:
        self.cancel_speech()
        self.capture.cancel()
        self.capture_cancelled = True
        self.rx_buffer.clear()
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def send(self, message: dict[str, Any]) -> None:
        if self.fd is None:
            raise RuntimeError("串口尚未打开")
        with self._send_lock:
            for record in wire_messages(message):
                payload = memoryview(record)
                while payload:
                    try:
                        written = os.write(self.fd, payload)
                        payload = payload[written:]
                    except BlockingIOError:
                        select.select([], [self.fd], [], 0.25)
        if self.verbose:
            print(f"Mac → 设备  {message}")

    def deliver(self, message: dict[str, Any]) -> None:
        self.send(message)

    def stream_word_audio(self, word: str) -> None:
        if self.tts is None:
            return
        speech_started = False
        try:
            for pcm in self.tts.iter_word_pcm(word, SPEECH_CHUNK_BYTES):
                if not speech_started:
                    self.send(
                        {"type": "speech_start", "sample_rate": self.tts.sample_rate}
                    )
                    speech_started = True
                self.send(
                    {
                        "type": "speech_chunk",
                        "data": base64.b64encode(pcm).decode("ascii"),
                    }
                )
        except (OSError, RuntimeError, wave.Error) as error:
            print(f"单词发音失败：{error}")
        finally:
            if speech_started:
                self.send({"type": "speech_end"})

    def _heartbeat_loop(self) -> None:
        while not self._heartbeat_stop.wait(self.heartbeat_seconds):
            try:
                self.send({"type": "bridge_ready"})
            except (OSError, RuntimeError) as error:
                self._heartbeat_error = error
                return

    def start_heartbeat(self) -> None:
        """Keep the device alive even while a synchronous Codex RPC is slow."""
        self._heartbeat_error = None
        self._heartbeat_stop.clear()
        self.send({"type": "bridge_ready"})
        if self.codex is not None and hasattr(self.codex, "state_snapshot"):
            self.deliver(self.codex.state_snapshot())
        self.send({"type": "word_bear_day", "day": date.today().toordinal()})
        self.send({"type": "word_bear_sync_request"})
        self._heartbeat_thread = threading.Thread(
            target=self._heartbeat_loop,
            name="FoloOS device heartbeat",
            daemon=True,
        )
        self._heartbeat_thread.start()

    def stop_heartbeat(self) -> None:
        self._heartbeat_stop.set()
        if self._heartbeat_thread is not None:
            self._heartbeat_thread.join(timeout=1.0)
            self._heartbeat_thread = None

    def schedule(self, delay: float, message: dict[str, Any]) -> None:
        self.sequence += 1
        heapq.heappush(
            self.pending,
            (self.clock() + delay, self.sequence, message),
        )

    def handle_line(self, raw_line: bytes) -> None:
        line = raw_line.strip()
        if not line.startswith(b"{"):
            if self.verbose and line:
                print(f"设备日志  {line.decode('utf-8', errors='replace')}")
            return
        try:
            event = json.loads(line.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            if self.verbose:
                print("忽略无法解析的设备消息")
            return
        if not isinstance(event, dict) or not isinstance(event.get("type"), str):
            return

        event_type = event["type"]
        if event_type in {
            "word_bear_sync_begin",
            "word_bear_sync_item",
            "word_bear_sync_end",
            "word_bear_progress",
        }:
            handled = self.word_bear.handle(event)
            if event_type == "word_bear_sync_end" and handled:
                print(f"单词熊数据已整理：{self.word_bear.report_path}")
            if not handled and self.verbose:
                print(f"忽略无效的单词熊同步消息：{event_type}")
            return
        if event_type == "word_bear_audio_request":
            self.stream_word_audio(str(event.get("word") or ""))
            return
        labels = {
            "record_start": "设备开始录音",
            "record_stop": "设备停止录音，正在语音识别",
            "voice_command": "设备确认发送语音指令",
            "approval_decision": "设备确认发送审批结果",
            "cancel_task": "设备取消任务",
        }
        if event_type == "record_start":
            self.cancel_speech()
            self.capture_cancelled = False
            try:
                self.capture.start(event)
            except ValueError as error:
                self.send({"type": "error", "text": str(error)})
                return
        elif event_type == "audio_chunk":
            if self.capture_cancelled:
                return
            try:
                if not self.capture.active:
                    # A brief Wi-Fi reconnect can happen after record_start.
                    # Continue collecting the remaining 16 kHz mono stream
                    # instead of silently discarding every following chunk.
                    self.capture.start({})
                    print("无线录音已恢复，继续接收音频")
                self.capture.add_chunk(event)
            except ValueError as error:
                self.capture.active = False
                self.send({"type": "error", "text": str(error)})
            return
        elif event_type == "capture_error":
            self.capture.active = False
            self.send({"type": "error", "text": str(event.get("message") or "录音失败")})
            return
        elif event_type == "record_cancel":
            self.capture_cancelled = True
            self.capture.cancel()
            self.cancel_speech()
            return
        elif event_type == "record_stop" and self.speech is not None:
            if self.capture_cancelled:
                return
            try:
                expected_bytes = event.get("bytes")
                if expected_bytes is not None and (
                    not isinstance(expected_bytes, int)
                    or expected_bytes != len(self.capture.pcm)
                ):
                    self.capture.cancel()
                    raise ValueError("录音传输不完整，请重新录音")
                duration = self.capture.duration_seconds
                wav_path = self.capture.finish()
                self.cancel_speech()
                self.speech_process = self.speech.start(wav_path)
                self.speech_started_at = self.clock()
                print(f"已收到 {duration:.2f} 秒音频，正在识别")
            except (OSError, RuntimeError, ValueError) as error:
                self.send({"type": "error", "text": str(error)})
            print(labels[event_type])
            return

        if event_type != "audio_chunk":
            print(labels.get(event_type, f"设备事件：{event_type}"))
        if self.codex is not None:
            response_identity = (
                {"request_id": event["request_id"]}
                if event_type in {"codex_catalog_request", "codex_project_select",
                                  "codex_thread_select", "codex_thread_new"}
                and "request_id" in event else {})
            try:
                if event_type == "voice_command":
                    # This receipt is deliberately sent before the synchronous
                    # desktop IPC call. The device can now distinguish "the Mac
                    # received my packet" from "Codex has started working".
                    self.send({"type": "task_queued", "text": "电脑端已收到指令"})
                messages = self.codex.handle_device_event(event)
                if response_identity and any(m.get("type") == "error" for m in messages):
                    if not any(m.get("type") == "codex_selected" for m in messages):
                        self.codex.handle_device_event({"type": "codex_catalog_cancel"})
                for message in messages:
                    self.deliver({**message, **response_identity})
            except CodexProtocolError as error:
                if response_identity:
                    self.codex.handle_device_event({"type": "codex_catalog_cancel"})
                self.deliver({"type": "error", "text": str(error), **response_identity})
            except Exception:
                # ⚠ 不能让任何异常从设备事件处理里冒泡出去: 一旦冒泡到
                # _run_connected_loop, 整条连接会被判为"断开"并重连, 而设备
                # 侧只看到"点了没反应", 也没有任何错误提示 —— 极难排查。
                # 这里兜住并明确回报设备(实测: 之前 OSError 冒泡导致
                # "选会话后设备无任何反应")。
                import traceback
                print("处理设备事件失败:", traceback.format_exc())
                try:
                    if response_identity:
                        self.codex.handle_device_event({"type": "codex_catalog_cancel"})
                    self.deliver({"type": "error",
                                  "text": "电脑端处理该操作失败，详情见桥接日志",
                                  **response_identity})
                except Exception:
                    pass
        elif self.scenario is not None:
            for delay, message in self.scenario.handle(event):
                self.schedule(delay, message)

    def read_available(self) -> None:
        if self.fd is None:
            return
        try:
            chunk = os.read(self.fd, 4096)
        except BlockingIOError:
            return
        if not chunk:
            return
        self.rx_buffer.extend(chunk)
        while b"\n" in self.rx_buffer:
            raw_line, _, remainder = self.rx_buffer.partition(b"\n")
            self.rx_buffer = bytearray(remainder)
            self.handle_line(raw_line)
        if len(self.rx_buffer) > MAX_RX_BUFFER:
            self.rx_buffer.clear()

    def flush_pending(self) -> None:
        now = self.clock()
        while self.pending and self.pending[0][0] <= now:
            _, _, message = heapq.heappop(self.pending)
            self.deliver(message)

    def cancel_speech(self) -> None:
        process = self.speech_process
        self.speech_process = None
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=1.0)

    def poll_speech(self) -> None:
        process = self.speech_process
        if process is None:
            return
        if process.poll() is None:
            if self.clock() - self.speech_started_at >= SPEECH_TIMEOUT_SECONDS:
                self.cancel_speech()
                print("语音识别超时（15秒），已停止后台识别")
                self.deliver({"type": "error", "text": "语音识别超时，请重新录音"})
            return
        self.speech_process = None
        if self.speech is None:
            return
        success, text = self.speech.result(process)
        print(f"语音识别耗时 {self.clock() - self.speech_started_at:.2f} 秒")
        if success:
            print(f"识别结果：{text}")
            self.deliver({"type": "transcript", "text": text})
        else:
            print(f"识别失败：{text}")
            self.deliver({"type": "error", "text": text})

    def poll_codex(self) -> None:
        if self.codex is None:
            return
        # thread/read is synchronous and can occasionally pause for seconds.
        # Audio arrives about 50 times per second; blocking here fills the
        # ESP32 TCP send buffer and makes the Wi-Fi bridge disconnect midway
        # through a recording. Voice capture always takes priority.
        if self.capture.active or self.speech_process is not None:
            return
        try:
            for message in self.codex.poll():
                self.deliver(message)
        except CodexProtocolError as error:
            self.send({"type": "error", "text": str(error)})
        except Exception:
            import traceback
            print("poll_codex 失败:", traceback.format_exc())

    def _run_connected_loop(self) -> None:
        assert self.fd is not None
        self.start_heartbeat()
        print(f"已连接：{self.port}")
        speech_mode = "真实语音识别" if self.speech is not None else "模拟识别"
        task_mode = "真实 Codex" if self.codex is not None else "模拟任务"
        print(
            f"桥接已启动（{speech_mode} + {task_mode}）。"
            "进入设备的“编程伴侣”，按 Ctrl+C 停止。"
        )
        while True:
            if self._heartbeat_error is not None:
                raise RuntimeError(
                    f"设备心跳发送失败：{self._heartbeat_error}"
                )
            self.flush_pending()
            self.poll_speech()
            self.poll_codex()
            select_target = getattr(self, "sock", None) or self.fd
            readable, _, _ = select.select([select_target], [], [], 0.05)
            if readable:
                self.read_available()

    def run(self) -> None:
        try:
            if self.codex is not None:
                self.codex.start()
                print(f"真实 Codex 已连接：{self.codex.workspace}")
            if self.fd is None:
                self.open()
            self._run_connected_loop()
        finally:
            self.stop_heartbeat()
            self.close()
            if self.codex is not None:
                self.codex.close()


class NetworkBridge(SerialBridge):
    """Run the unchanged bridge protocol over an authenticated LAN socket."""

    def __init__(
        self,
        host: str,
        port: int,
        token: str,
        *args: Any,
        config_path: Path = WIRELESS_CONFIG_PATH,
        **kwargs: Any,
    ):
        super().__init__(f"{host or '自动发现'}:{port}", *args, **kwargs)
        self.host = host
        self.network_port = port
        self.token = token
        self.config_path = config_path
        self.sock: socket.socket | None = None

    def open(self) -> None:
        errors: list[str] = []
        # 已知地址时重试几次再退化为广播发现: 设备刚上电/刚重连时,
        # TCP 握手或鉴权应答可能慢于单次 3s 超时(实测会误判为"设备不在")。
        # UDP 广播在很多路由器(隔离/组播抑制)下本就不可靠, 不能当主路径。
        attempts = 3 if self.host else 1
        candidates = [(self.host, self.network_port)] * attempts if self.host else []
        tried_discovery = False
        while candidates or not tried_discovery:
            if not candidates:
                tried_discovery = True
                try:
                    candidates.append(discover_wireless_device())
                except RuntimeError as error:
                    errors.append(str(error))
                    break
            host, port = candidates.pop(0)
            candidate: socket.socket | None = None
            try:
                candidate = socket.create_connection((host, port), timeout=6.0)
                candidate.sendall(
                    encode_message({"type": "bridge_auth", "token": self.token})
                )
                response = bytearray()
                while b"\n" not in response and len(response) <= MAX_RX_BUFFER:
                    chunk = candidate.recv(1024)
                    if not chunk:
                        break
                    response.extend(chunk)
                line = bytes(response).partition(b"\n")[0]
                reply = json.loads(line.decode("utf-8"))
                if reply.get("type") != "bridge_auth_ok":
                    raise RuntimeError("设备拒绝了无线配对密钥")
                candidate.settimeout(None)
                self.host = host
                self.network_port = port
                self.port = f"Wi-Fi {host}:{port}"
                self.sock = candidate
                self.fd = candidate.fileno()
                save_wireless_config(host, self.token, port, self.config_path)
                return
            except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
                errors.append(f"{host}:{port} {error}")
                if candidate is not None:
                    candidate.close()
                # 不清空候选: 让剩余的重试继续消费; 全部耗尽后由循环顶部
                # 自动转入一次广播发现(do not clear, so retries actually run).
        detail = errors[-1] if errors else "没有可连接的地址"
        raise RuntimeError(f"无线连接失败：{detail}")

    def close(self) -> None:
        self.cancel_speech()
        self.capture.cancel()
        self.capture_cancelled = True
        self.rx_buffer.clear()
        if self.codex is not None:
            self.codex.handle_device_event({"type": "codex_catalog_cancel"})
        if self.sock is not None:
            self.sock.close()
            self.sock = None
        self.fd = None

    def send(self, message: dict[str, Any]) -> None:
        if self.sock is None:
            raise RuntimeError("无线连接尚未打开")
        with self._send_lock:
            for record in wire_messages(message):
                self.sock.sendall(record)
        if self.verbose:
            print(f"Mac → 设备  {message}")

    def read_available(self) -> None:
        if self.sock is None:
            return
        chunk = self.sock.recv(4096)
        if not chunk:
            raise RuntimeError("设备无线连接已断开")
        self.rx_buffer.extend(chunk)
        while b"\n" in self.rx_buffer:
            raw_line, _, remainder = self.rx_buffer.partition(b"\n")
            self.rx_buffer = bytearray(remainder)
            self.handle_line(raw_line)
        if len(self.rx_buffer) > MAX_RX_BUFFER:
            self.rx_buffer.clear()

    def run(self) -> None:
        last_error = ""
        try:
            if self.codex is not None:
                self.codex.start()
                print(f"真实 Codex 已连接：{self.codex.workspace}")
            while True:
                try:
                    if self.sock is None:
                        self.open()
                    last_error = ""
                    self._run_connected_loop()
                except (OSError, RuntimeError) as error:
                    message = str(error)
                    if message != last_error:
                        print(f"无线连接中断：{message}，正在自动重连。")
                        last_error = message
                    time.sleep(1.0)
                finally:
                    self.stop_heartbeat()
                    self.close()
        finally:
            if self.codex is not None:
                self.codex.close()


def provision_wireless(port: str, ssid: str | None = None,
                       password: str | None = None) -> None:
    network_name = (ssid or input("请输入要连接的 Wi-Fi 名称：")).strip()
    if password is None:
        password = getpass.getpass("请输入 Wi-Fi 密码（开放网络直接回车）：")
    if not network_name:
        raise RuntimeError("Wi-Fi 名称不能为空")
    token = secrets.token_hex(16)
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    saved = False
    host = ""
    try:
        configure_serial(fd)
        payload = memoryview(
            encode_message(
                {
                    "type": "wifi_setup",
                    "ssid": network_name,
                    "password": password,
                    "token": token,
                }
            )
        )
        while payload:
            try:
                written = os.write(fd, payload)
                payload = payload[written:]
            except BlockingIOError:
                select.select([], [fd], [], 0.25)

        buffer = bytearray()
        deadline = time.monotonic() + 35.0
        while time.monotonic() < deadline:
            readable, _, _ = select.select([fd], [], [], 0.25)
            if not readable:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            buffer.extend(chunk)
            while b"\n" in buffer:
                raw, _, remainder = buffer.partition(b"\n")
                buffer = bytearray(remainder)
                if not raw.strip().startswith(b"{"):
                    continue
                try:
                    event = json.loads(raw.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if event.get("type") != "wifi_status":
                    continue
                if event.get("status") == "error":
                    raise RuntimeError(f"设备拒绝配网参数：{event.get('text') or '未知错误'}")
                if event.get("status") == "saved":
                    saved = True
                    print("配网信息已安全写入设备，正在等待连接……")
                if event.get("status") == "connected":
                    host = str(event.get("ip") or "")
                    save_wireless_config(host, token)
                    print(f"无线连接配置成功：{host}")
                    print("现在可以拔掉 USB，今后直接运行“启动编程伴侣桥接”。")
                    return
    finally:
        os.close(fd)
    if saved:
        save_wireless_config(host, token)
        raise RuntimeError("配置已保存，但 35 秒内未连上 Wi-Fi；请检查密码或 2.4GHz 网络")
    raise RuntimeError("设备没有确认配网，请确认刷入的是无线版固件")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="FoloOS Mac 无线/USB 语音桥")
    parser.add_argument("--port", help="例如 /dev/cu.usbmodem1101")
    parser.add_argument("--usb", action="store_true", help="跳过无线配置，强制使用 USB")
    parser.add_argument(
        "--setup-wifi",
        action="store_true",
        help="通过 USB 为设备写入一次 Wi-Fi 配置",
    )
    parser.add_argument("--ota", type=Path, help="通过已配对的无线连接升级应用固件")
    parser.add_argument(
        "--word-bear-restore",
        type=Path,
        help="把显式指定的单词熊 JSON 备份恢复到设备",
    )
    parser.add_argument("--ssid", help="配网时使用的 Wi-Fi 名称；省略则现场输入")
    parser.add_argument("--password-stdin", action="store_true",
                        help="从标准输入读取 Wi-Fi 密码")
    parser.add_argument(
        "--mock-transcript",
        help="跳过真实语音识别，回传指定的测试文字",
    )
    parser.add_argument("--locale", default="zh-CN", help="macOS Speech 语言，默认 zh-CN")
    parser.add_argument("--recordings-dir", type=Path, help="最后一次 WAV 的保存目录")
    parser.add_argument(
        "--word-bear-export",
        type=Path,
        help="把最近一次单词熊备份导出为 CSV 和 JSON 后退出",
    )
    parser.add_argument(
        "--real-codex",
        action="store_true",
        help="将设备确认后的指令发送给真实 Codex，而不是模拟任务",
    )
    parser.add_argument(
        "--workspace",
        type=Path,
        help="真实 Codex 操作的项目目录；默认是本仓库根目录",
    )
    parser.add_argument("--codex-bin", help="Codex CLI 路径，默认自动查找")
    parser.add_argument("--verbose", action="store_true", help="显示协议和设备日志")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.word_bear_export is not None:
            json_path, csv_path = WordBearStore().export_to(args.word_bear_export)
            print(f"单词熊 JSON 已导出：{json_path}")
            print(f"单词熊 CSV 已导出：{csv_path}")
            return 0
        if args.word_bear_restore is not None:
            wireless_word_bear_restore(args.word_bear_restore)
            return 0
        if args.ota is not None:
            wireless_ota_update(args.ota)
            return 0
        if args.setup_wifi:
            password = sys.stdin.readline().rstrip("\r\n") if args.password_stdin else None
            provision_wireless(args.port or detect_port(), args.ssid, password)
            return 0
        tools_dir = Path(__file__).resolve().parent
        speech = None if args.mock_transcript else MacSpeechRecognizer(tools_dir, args.locale)
        tts = MacSpeechSynthesizer()
        codex = None
        scenario = None
        if args.real_codex:
            codex = CodexAppServer(
                args.workspace or tools_dir.parent,
                executable=args.codex_bin,
                verbose=args.verbose,
                thread_opener=open_codex_thread,
            )
        else:
            scenario = MockScenario(args.mock_transcript)
        bridge_options = {
            "scenario": scenario,
            "speech": speech,
            "tts": tts,
            "codex": codex,
            "recordings_dir": args.recordings_dir,
            "verbose": args.verbose,
        }
        bridge: SerialBridge | NetworkBridge | None = None
        wireless = None if args.usb or args.port else load_wireless_config()
        if wireless is not None:
            host, network_port, token = wireless
            bridge = NetworkBridge(host, network_port, token, **bridge_options)
        if bridge is None:
            bridge = SerialBridge(args.port or detect_port(), **bridge_options)
        bridge.run()
    except KeyboardInterrupt:
        print("\nMac Bridge 已停止")
        return 0
    except (OSError, RuntimeError) as error:
        print(f"启动失败：{error}", file=sys.stderr)
        print(
            "若使用 USB，请先让 Chrome 刷机工具断开设备；若使用无线，请确认设备已连接 2.4GHz，且 Mac 在同一局域网。",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
