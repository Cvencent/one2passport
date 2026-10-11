"""验证 EXE 运行时能否在解包目录里找到 .ps1 语音脚本。

这是打包最容易翻车的一处:源码运行时 __file__ 指向脚本目录,
打包后必须改用 sys._MEIPASS。写错了的典型症状是
"语音识别在开发机正常、拷给别人全废"。

这里直接启动 EXE,然后检查它的临时解包目录里有没有那两个 .ps1。
"""
import os
import subprocess
import sys
import time
from pathlib import Path

exe = Path(__file__).resolve().parent / "dist" / "CodexBridge.exe"
log = Path(os.environ["LOCALAPPDATA"]) / "FoloOS" / "CodexBridge" / "bridge.log"

if log.is_file():
    log.unlink()

print(f"启动 {exe}")
proc = subprocess.Popen([str(exe)], close_fds=True)
time.sleep(12)

# PyInstaller 单文件模式解包到 %TEMP%\_MEIxxxxxx
temp = Path(os.environ.get("TEMP", "/tmp"))
mei_dirs = sorted(
    [d for d in temp.glob("_MEI*") if d.is_dir()],
    key=lambda d: d.stat().st_mtime,
    reverse=True,
)
print(f"临时解包目录候选: {len(mei_dirs)}")

targets = ["windows_speech_recognize.ps1", "windows_speech_synthesize.ps1", "pair_device.py"]
found_any = False
for directory in mei_dirs[:3]:
    hits = [t for t in targets if (directory / t).is_file()]
    if hits:
        found_any = True
        print(f"  {directory.name}: 找到 {hits}")
        for t in targets:
            path = directory / t
            print(f"     {t:38s} {'OK' if path.is_file() else '缺失'} "
                  f"{path.stat().st_size if path.is_file() else ''}")

print(f"\n结论: {'PASS' if found_any else 'FAIL — 解包目录里没找到资源'}")

subprocess.run(["taskkill", "/F", "/IM", "CodexBridge.exe"], capture_output=True)
time.sleep(1)
sys.exit(0 if found_any else 1)
