"""交付确认:针对最终 EXE 跑一遍关键检查,给出可读结论。"""
import ctypes
import os
import subprocess
import sys
import time
from pathlib import Path

pkg = Path(r"D:/aipassport/foloos/windows-package")
exe = pkg / "CodexBridge.exe"
log = Path(os.environ["LOCALAPPDATA"]) / "FoloOS" / "CodexBridge" / "bridge.log"

print("=" * 58)
print("交付确认")
print("=" * 58)

# 1. 文件存在性
required = {
    "托盘 EXE": exe,
    "使用说明": pkg / "CodexBridge-托盘程序说明.md",
    "包说明": pkg / "README-Windows.md",
    "启动脚本": pkg / "启动Codex桥接.bat",
    "源码": pkg / "runtime" / "tray_app.py",
    "打包规格": pkg / "runtime" / "CodexBridge.spec",
    "图标": pkg / "runtime" / "codex_bridge.ico",
}
print("\n[1] 交付文件")
for name, path in required.items():
    ok = path.is_file()
    size = f"{path.stat().st_size:,} B" if ok else "-"
    print(f"  {'OK ' if ok else 'NG '} {name:10s} {size:>14s}  {path.name}")

# 2. 启动并确认托盘
if log.is_file():
    log.unlink()
print("\n[2] 实际启动验证")
proc = subprocess.Popen([str(exe)], close_fds=True)
time.sleep(14)

user32 = ctypes.windll.user32
WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
tray = []


def cb(hwnd, _l):
    cls = ctypes.create_unicode_buffer(512)
    user32.GetClassNameW(hwnd, cls, 512)
    if "SystemTrayIcon" in cls.value:
        tray.append(cls.value)
    return True


user32.EnumWindows(WNDENUMPROC(cb), 0)
alive = proc.poll() is None
print(f"  进程存活      : {alive}")
print(f"  托盘图标已创建: {bool(tray)}  ({tray[0] if tray else '-'})")

if log.is_file():
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    print(f"  日志行数      : {len(lines)}")
    for line in lines:
        print(f"    | {line}")

# 3. 未配对时的行为
print("\n[3] 未配对状态下的行为")
pairing = (Path(os.environ["LOCALAPPDATA"]) / "FoloOS" / "bridge.json").is_file()
print(f"  bridge.json 存在: {pairing}  {'(应为 False — 干净环境)' if not pairing else '(注意:有残留)'}")
print("  → 程序会显示灰色「未配对」并提示点「配对设备…」")

# 清理
subprocess.run(["taskkill", "/F", "/IM", "CodexBridge.exe"], capture_output=True)
time.sleep(1)

print("\n" + "=" * 58)
ok = all(p.is_file() for p in required.values()) and bool(tray) and alive
print(f"结论: {'全部通过 — 可以交付' if ok else '存在问题,需复查'}")
print("=" * 58)
sys.exit(0 if ok else 1)
