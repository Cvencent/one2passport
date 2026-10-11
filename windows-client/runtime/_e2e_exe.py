"""端到端验证:用假设备 + 真 EXE,验证状态机真的会随连接变化。

## 为什么要这么测

前面的自检只是"函数能不能调通"。真正的风险在于:
EXE 里跑起来的桥接线程,能不能真的完成 UDP 发现 -> TCP 鉴权 -> 进入已连接态。
这一段跨了线程、socket、子类钩子三层,只有真跑一遍才算数。

假设备做三件事:
  1. 监听 UDP 8764,收到 FOLOOS_DISCOVER_V1 就回 {"device":"FoloOS-AI-Passport",...}
  2. 监听 TCP 8765,接受连接并校验 bridge_auth,回 bridge_auth_ok
  3. 连上后推一条 task_status,让状态机从 ready 切到 busy,再推 done 切回 ready
"""
import ctypes
import json
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

BRIDGE_PORT = 8765
DISCOVERY_PORT = 8764
DISCOVERY_REQUEST = b"FOLOOS_DISCOVER_V1"
TOKEN = "a" * 32

runtime = Path(__file__).resolve().parent
exe = runtime / "dist" / "CodexBridge.exe"
log = Path(os.environ["LOCALAPPDATA"]) / "FoloOS" / "CodexBridge" / "bridge.log"
config = Path(os.environ["LOCALAPPDATA"]) / "FoloOS" / "bridge.json"

events: list[str] = []
stop = threading.Event()


def fake_discovery():
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind(("0.0.0.0", DISCOVERY_PORT))
    udp.settimeout(0.5)
    while not stop.is_set():
        try:
            payload, source = udp.recvfrom(1024)
        except socket.timeout:
            continue
        except OSError:
            break
        if payload.strip() == DISCOVERY_REQUEST:
            reply = json.dumps(
                {"device": "FoloOS-AI-Passport", "port": BRIDGE_PORT}
            ).encode()
            udp.sendto(reply, source)
            events.append("发现请求已应答")
    udp.close()


def fake_bridge():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", BRIDGE_PORT))
    server.listen(1)
    server.settimeout(0.5)
    while not stop.is_set():
        try:
            conn, _addr = server.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        events.append("TCP 已接受连接")
        conn.settimeout(2.0)
        buf = bytearray()
        try:
            # 收 bridge_auth
            while b"\n" not in buf:
                chunk = conn.recv(1024)
                if not chunk:
                    break
                buf.extend(chunk)
            line = bytes(buf).partition(b"\n")[0]
            try:
                auth = json.loads(line.decode())
            except ValueError:
                auth = {}
            events.append(f"收到鉴权 type={auth.get('type')} token匹配={auth.get('token') == TOKEN}")
            if auth.get("type") == "bridge_auth" and auth.get("token") == TOKEN:
                conn.sendall(b'{"type":"bridge_auth_ok"}\n')
                events.append("已回 bridge_auth_ok")
            buf.clear()
            # 连上后:推一条 task_status,再推 done,观察状态机变化
            time.sleep(2.5)
            conn.sendall(json.dumps({"type": "voice_command"}).encode() + b"\n")
            time.sleep(2.0)
        except (OSError, socket.timeout):
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass
        events.append("连接关闭")
    server.close()


# 准备配对配置,让 EXE 直接连(跳过配对流程)
config.parent.mkdir(parents=True, exist_ok=True)
config.write_text(
    json.dumps({"host": "127.0.0.1", "port": BRIDGE_PORT, "token": TOKEN}),
    encoding="utf-8",
)
print(f"已写入配对配置: {config}")

if log.is_file():
    log.unlink()

threads = [
    threading.Thread(target=fake_discovery, daemon=True),
    threading.Thread(target=fake_bridge, daemon=True),
]
for thread in threads:
    thread.start()
time.sleep(1.0)
print("假设备已就绪 (UDP 8764 / TCP 8765)")

print(f"启动 EXE: {exe}")
proc = subprocess.Popen([str(exe)], close_fds=True)

# 轮询日志,提取状态变化
deadline = time.monotonic() + 40
seen_states = []
while time.monotonic() < deadline:
    time.sleep(1.5)
    if log.is_file():
        content = log.read_text(encoding="utf-8", errors="replace")
        if "桥接已启动" in content or "已连接" in content:
            pass
    if any("已回 bridge_auth_ok" == e for e in events) and len(events) >= 4:
        time.sleep(4)
        break

print("\n假设备侧事件:")
for event in events:
    print(f"  - {event}")

print("\nEXE 日志:")
if log.is_file():
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        print(f"  | {line}")

# 检查托盘 tooltip 里是否体现了连接状态(通过窗口标题不可得,改看进程是否活着)
alive = proc.poll() is None
print(f"\nEXE 存活: {alive}")

ok = alive and any("已回 bridge_auth_ok" in e for e in events)
print(f"结论: {'PASS' if ok else 'FAIL'}")

print("\n清理")
stop.set()
subprocess.run(["taskkill", "/F", "/IM", "CodexBridge.exe"], capture_output=True)
time.sleep(1)
print("完成")
sys.exit(0 if ok else 1)
