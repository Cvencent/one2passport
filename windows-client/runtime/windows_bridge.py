#!/usr/bin/env python3
"""FoloOS wireless bridge for Windows 10/11."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

# Windows 专用:在无控制台的 GUI 进程里启动子进程时抑制弹出的控制台窗口。
# 非 Windows 上该常量为 0,对 subprocess 无影响。
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

if __package__:
    from .codex_backend import CodexProtocolError
    from .mac_bridge import (
        MockScenario,
        NetworkBridge,
        SPEECH_CHUNK_BYTES,
        load_wireless_config,
        open_codex_thread,
    )
    from .windows_codex_backend import WindowsCodexAppServer
else:
    from codex_backend import CodexProtocolError
    from mac_bridge import (
        MockScenario,
        NetworkBridge,
        SPEECH_CHUNK_BYTES,
        load_wireless_config,
        open_codex_thread,
    )
    from windows_codex_backend import WindowsCodexAppServer


class WindowsSpeechRecognizer:
    #: 最近一次识别失败的原因。托盘程序会读它来提示用户 ——
    #: 默认情况下 "未识别到文字" 只会被打印到日志,用户看不到任何反馈,
    #: 会以为设备坏了。这里把原因留在实例上,由上层决定怎么显示。
    last_error: str = ""

    def __init__(self, tools_dir: Path, locale: str = "zh-CN") -> None:
        self.script = tools_dir / "windows_speech_recognize.ps1"
        self.locale = locale

    def start(self, wav_path: Path) -> subprocess.Popen[str]:
        self.last_error = ""
        return subprocess.Popen(
            [
                "powershell.exe",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(self.script),
                "-WavPath",
                str(wav_path),
                "-Locale",
                self.locale,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            # 托盘 EXE 是无控制台的 GUI 进程,直接起 powershell 会闪一个黑窗口
            # (录音一次闪一次)。CREATE_NO_WINDOW 在无控制台时抑制该窗口;
            # 有控制台时它不生效,CLI 版行为不变。
            creationflags=CREATE_NO_WINDOW,
        )

    def result(self, process: subprocess.Popen[str]) -> tuple[bool, str]:
        stdout, stderr = process.communicate()
        if process.returncode == 0 and stdout.strip():
            self.last_error = ""
            return True, stdout.strip().splitlines()[-1]
        detail = stderr.strip().splitlines()[-1] if stderr.strip() else "未识别到文字"
        # 留在实例上供上层读取(托盘程序据此提示用户),不改变返回值契约。
        self.last_error = detail
        return False, detail


class WindowsSpeechSynthesizer:
    sample_rate = 16000

    def __init__(self, tools_dir: Path) -> None:
        self.script = tools_dir / "windows_speech_synthesize.ps1"

    def _iter(self, text: str, locale: str, chunk_bytes: int):
        spoken = str(text or "").strip()
        if not spoken:
            return
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "speech.wav"
            result = subprocess.run(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(self.script),
                    "-OutputPath",
                    str(output),
                    "-Locale",
                    locale,
                    "-Text",
                    spoken,
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                creationflags=CREATE_NO_WINDOW,
            )
            if result.returncode != 0 or not output.is_file():
                raise RuntimeError(result.stderr.strip() or "Windows 无法生成回答语音")
            with wave.open(str(output), "rb") as wav_file:
                if wav_file.getnchannels() != 1 or wav_file.getsampwidth() != 2:
                    raise RuntimeError("回答语音格式不受设备支持")
                frames_per_chunk = max(1, chunk_bytes // 2)
                while True:
                    chunk = wav_file.readframes(frames_per_chunk)
                    if not chunk:
                        break
                    yield chunk

    def iter_pcm(self, text: str, chunk_bytes: int = SPEECH_CHUNK_BYTES):
        yield from self._iter(text, "zh-CN", chunk_bytes)

    def iter_word_pcm(self, word: str, chunk_bytes: int = SPEECH_CHUNK_BYTES):
        yield from self._iter(word, "en-US", chunk_bytes)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="FoloOS Windows 无线 Codex 桥")
    parser.add_argument("--mock", action="store_true", help="仅运行模拟任务")
    parser.add_argument("--mock-transcript", help="模拟识别文字")
    parser.add_argument("--locale", default="zh-CN")
    parser.add_argument("--workspace", type=Path, default=Path.home())
    parser.add_argument("--codex-bin")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    tools_dir = Path(__file__).resolve().parent
    wireless = load_wireless_config()
    if wireless is None:
        print("启动失败：尚未配置设备 Wi-Fi，请先双击“2-配置设备WiFi.bat”", file=sys.stderr)
        return 1
    host, port, token = wireless
    speech = (
        None
        if args.mock_transcript
        else WindowsSpeechRecognizer(tools_dir, args.locale)
    )
    tts = WindowsSpeechSynthesizer(tools_dir)
    codex = None
    scenario = None
    if args.mock:
        scenario = MockScenario(args.mock_transcript)
    else:
        codex = WindowsCodexAppServer(
            args.workspace,
            executable=args.codex_bin,
            verbose=args.verbose,
            thread_opener=open_codex_thread,
        )
    bridge = NetworkBridge(
        host,
        port,
        token,
        scenario=scenario,
        speech=speech,
        tts=tts,
        codex=codex,
        verbose=args.verbose,
    )
    try:
        bridge.run()
    except KeyboardInterrupt:
        print("\nWindows Bridge 已停止")
        return 0
    except (OSError, RuntimeError, CodexProtocolError) as error:
        print(f"启动失败：{error}", file=sys.stderr)
        print("请确认设备和电脑连接同一个 2.4GHz 局域网。", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
