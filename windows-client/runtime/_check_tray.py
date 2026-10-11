"""静态自检:验证托盘程序的导入链、状态机与桥接挂钩能正常装配(不启动 GUI)。"""
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

failures = []


def check(name, fn):
    try:
        fn()
        print(f"  OK   {name}")
    except Exception as error:  # noqa: BLE001
        failures.append((name, error))
        print(f"  FAIL {name}: {type(error).__name__}: {error}")


print("== 1. 依赖导入 ==")
check("pystray", lambda: __import__("pystray"))
check("PIL", lambda: __import__("PIL"))
check("tray_app", lambda: __import__("tray_app"))

print("== 2. 桥接模块导入 ==")
check("_import_bridge", lambda: __import__("tray_app")._import_bridge())

print("== 3. tools_dir 解析 ==")


def tools_dir():
    from tray_app import resolve_tools_dir

    d = resolve_tools_dir()
    missing = [
        n
        for n in ("windows_speech_recognize.ps1", "windows_speech_synthesize.ps1")
        if not (d / n).is_file()
    ]
    assert not missing, f"缺少脚本: {missing}"


check("语音脚本存在", tools_dir)

print("== 4. 状态回调链路 ==")


def state_chain():
    from tray_app import ObservableBridge, STATE_BUSY, STATE_READY

    seen = []
    bridge = ObservableBridge.__new__(ObservableBridge)
    bridge._on_state = lambda s, d: seen.append((s, d))
    bridge._stop_event = threading.Event()
    bridge._reconnect_event = threading.Event()
    bridge._task_count = 0
    bridge._lock = threading.Lock()

    bridge._state_from_uplink({"type": "record_start"})
    assert seen[-1][0] == STATE_BUSY, "开始录音应进入 busy"
    bridge._state_from_downlink({"type": "done", "text": "ok"})
    assert seen[-1][0] == STATE_READY, "任务完成应回到 ready"
    bridge._state_from_downlink({"type": "approval"})
    assert seen[-1][0] == STATE_BUSY, "收到审批请求应回到 busy"
    assert bridge.task_count == 1, f"任务计数应为 1,实为 {bridge.task_count}"
    print(f"       触发 {len(seen)} 次状态变更,末态 {seen[-1][0]}")


check("上行/下行状态映射", state_chain)

print("== 5. 配对配置读取 ==")


def pairing():
    from tray_app import load_pairing

    result = load_pairing()
    print(f"       当前配对: {result if result is None else (result[0], result[1], '***')}")


check("load_pairing", pairing)

print("== 6. 开机自启接口 ==")
check("autostart_enabled", lambda: __import__("tray_app").autostart_enabled())
check("command_path", lambda: print(f"       {__import__('tray_app').command_path()}"))

print()
if failures:
    print(f"FAILED: {len(failures)} 项")
    for name, error in failures:
        print(f"  - {name}: {error}")
    sys.exit(1)
print("全部通过")
