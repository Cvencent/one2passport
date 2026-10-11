"""EXE 冒烟测试:启动它,确认进程存活、日志有输出、托盘窗口真的被创建。

## 两个必须注意的测试陷阱

1. **PyInstaller 单文件模式会 fork 子进程。** 父进程只是解包器,真正跑
   Python 代码的是子进程,它的 PID 与 Popen 拿到的不同。所以按 PID 枚举
   窗口会一无所获 —— 必须去找子进程,或干脆按窗口类名特征搜。

2. **pystray 的托盘窗口类名是动态的**,形如
   `<图标名><id>SystemTrayIcon`(见 pystray/_win32.py 的 _register_class)。
   拿它当特征串匹配最稳,不必依赖 PID。
"""
import ctypes
import os
import subprocess
import sys
import time
from pathlib import Path

exe = Path(__file__).resolve().parent / "dist" / "CodexBridge.exe"
log = Path(os.environ["LOCALAPPDATA"]) / "FoloOS" / "CodexBridge" / "bridge.log"

if not exe.is_file():
    print(f"FAIL: 找不到 {exe}")
    sys.exit(1)

if log.is_file():
    log.unlink()

print(f"启动: {exe}")
proc = subprocess.Popen([str(exe)], close_fds=True)
print(f"父进程 PID: {proc.pid}")

# 给解包 + 初始化留足时间
time.sleep(15)

alive = proc.poll() is None
print(f"15 秒后父进程存活: {alive}  returncode={proc.poll()}")

if log.is_file():
    content = log.read_text(encoding="utf-8", errors="replace")
    print(f"日志 {log.stat().st_size} 字节:")
    for line in content.splitlines()[:20]:
        print(f"  | {line}")
else:
    print(f"FAIL: 日志未生成 {log}")

# --- 按窗口类名特征搜索托盘窗口 ---
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

tray_windows = []
all_windows = []
pids_seen = set()


def enum_cb(hwnd, _lparam):
    pid = ctypes.c_ulong()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    cls = ctypes.create_unicode_buffer(512)
    user32.GetClassNameW(hwnd, cls, 512)
    length = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(max(length + 1, 256))
    user32.GetWindowTextW(hwnd, buf, len(buf))
    pids_seen.add(pid.value)
    entry = (pid.value, cls.value, buf.value)
    all_windows.append(entry)
    if "SystemTrayIcon" in cls.value:
        tray_windows.append(entry)
    return True


user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
print(f"枚举到 {len(all_windows)} 个顶层窗口,涉及 {len(pids_seen)} 个进程")
print(f"命中托盘窗口类: {len(tray_windows)}")
for pid, cls, title in tray_windows:
    print(f"  PID={pid} 类={cls!r} 标题={title!r}")

# --- 确认子进程存在(单文件模式的真正执行体) ---
print("相关进程:")
# tasklist 在中文 Windows 上输出 GBK,用 utf-8 解码会抛 UnicodeDecodeError,
# 所以这里显式指定 gbk 并容错。
out = subprocess.run(
    ["tasklist", "/FI", "IMAGENAME eq CodexBridge.exe", "/FO", "CSV", "/NH"],
    capture_output=True, text=True, encoding="gbk", errors="replace",
)
for line in out.stdout.splitlines():
    if line.strip():
        print(f"  {line.strip()}")
child_count = out.stdout.count("CodexBridge.exe")
print(f"CodexBridge.exe 实例数: {child_count}")

print()
ok = bool(tray_windows) and alive
print(f"结论: {'PASS' if ok else 'FAIL'}")

print("终止进程")
subprocess.run(["taskkill", "/F", "/IM", "CodexBridge.exe"],
               capture_output=True, text=True)
time.sleep(1)

# 收尾:确认日志里有退出记录
if log.is_file():
    tail = log.read_text(encoding="utf-8", errors="replace").splitlines()
    print("日志末尾:")
    for line in tail[-4:]:
        print(f"  | {line}")

sys.exit(0 if ok else 1)
