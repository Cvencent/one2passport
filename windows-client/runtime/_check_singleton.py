"""单实例锁测试:第一个实例正常,第二个实例应被拒绝并退出。"""
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from tray_app import SingleInstance

print("== 锁语义测试(同进程内两次获取应互斥) ==")
first = SingleInstance()
print(f"第一次获取: already_running={first.already_running}")
assert not first.already_running, "首次获取不应报已运行"

second = SingleInstance()
print(f"第二次获取: already_running={second.already_running}")
assert second.already_running, "第二次获取应报已运行"

second.release()
first.release()
print("释放后可再次获取:")
third = SingleInstance()
print(f"already_running={third.already_running}")
assert not third.already_running, "释放后应可重新获取"
third.release()
print("PASS")

print()
print("== EXE 双开测试 ==")
exe = Path(__file__).resolve().parent / "dist" / "CodexBridge.exe"
if not exe.is_file():
    print("跳过:EXE 尚未打包")
    sys.exit(0)

log = Path(os.environ["LOCALAPPDATA"]) / "FoloOS" / "CodexBridge" / "bridge.log"
if log.is_file():
    log.unlink()

p1 = subprocess.Popen([str(exe)], close_fds=True)
print(f"实例1 PID={p1.pid}")
time.sleep(10)

p2 = subprocess.Popen([str(exe)], close_fds=True)
print(f"实例2 PID={p2.pid}")
# 实例2 应弹窗后自己退出;弹窗会阻塞,所以给它 6 秒再强杀
time.sleep(6)

alive1 = p1.poll() is None
alive2 = p2.poll() is None
print(f"实例1 存活={alive1}  实例2 存活={alive2}")

content = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
print("日志:")
for line in content.splitlines():
    print(f"  | {line}")

detected = "已有实例在运行" in content
print(f"\n检测到重复启动: {detected}")
print(f"结论: {'PASS' if detected and alive1 else 'FAIL'}")

subprocess.run(["taskkill", "/F", "/IM", "CodexBridge.exe"], capture_output=True)
time.sleep(1)
sys.exit(0 if detected and alive1 else 1)
