#!/usr/bin/env python3
"""Codex 桥接托盘程序(Windows)。

把 windows_bridge.py 的桥接逻辑包装成一个常驻系统托盘的 GUI 程序:
图标颜色反映当前状态,鼠标悬停显示详情,右键菜单可配对/重连/查日志/设自启。

## 三个设计要点(都不是随便选的)

1. **托盘图标必须跑在主线程。** pystray 的 Win32 后端要占一个消息循环,
   从子线程调 icon.run() 会直接报错。所以桥接逻辑放后台线程,
   主线程只跑 icon.run(),退出时再 join 桥接线程。

2. **子类挂钩上报状态,而不是改写桥接逻辑。**
   NetworkBridge 的连接/重连/收发全部复用(见 ObservableBridge 的说明),
   这里只覆盖 open/deliver/handle_line 这几个"必经点"来取状态,
   避免把 1600 行的桥接协议复制一份出来——那种复制迟早会和上游走岔。

3. **无控制台进程里 print() 无处可去。**
   --noconsole 打包后 sys.stdout 是 None,任何 print 都会抛异常。
   启动第一件事就是把 stdout/stderr 重定向进日志文件,
   否则出问题时连一行线索都没有,只能干瞪眼。
"""
from __future__ import annotations

import ctypes
import json
import os
import queue
import secrets
import socket
import sys
import threading
import time
import traceback
from pathlib import Path

# ---------------------------------------------------------------------------
# 状态模型
# ---------------------------------------------------------------------------

STATE_STARTING = "starting"      # 正在启动 Codex / 初始化
STATE_CONNECTING = "connecting"  # 正在发现或连接设备
STATE_READY = "ready"            # 已连接,空闲待命
STATE_BUSY = "busy"              # 录音 / 识别 / Codex 任务运行中
STATE_ERROR = "error"            # 连接异常,正在自动重试
STATE_STOPPED = "stopped"        # 已停止
STATE_UNPAIRED = "unpaired"      # 尚未与设备配对

# 状态 -> (标签, 图标底色)。底色是托盘上唯一能一眼看到的信息源,
# 所以用高饱和度的语义色,而不是品牌色。
STATE_STYLE: dict[str, tuple[str, str]] = {
    STATE_STARTING: ("正在启动", "#2D7FF9"),
    STATE_CONNECTING: ("正在连接", "#2D7FF9"),
    STATE_READY: ("已连接", "#12B76A"),
    STATE_BUSY: ("工作中", "#F79009"),
    STATE_ERROR: ("连接异常", "#F04438"),
    STATE_STOPPED: ("已停止", "#667085"),
    STATE_UNPAIRED: ("未配对", "#667085"),
}


def resolve_tools_dir() -> Path:
    """定位随包分发的 .ps1 语音脚本所在目录。

    打包后 __file__ 指向 PyInstaller 的临时解压目录,必须改用 sys._MEIPASS;
    源码运行时 __file__ 就是本目录。这个区别不处理的话,EXE 一旦双击运行,
    语音识别/合成会因为找不到 .ps1 而全部失败。
    """
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
    return Path(__file__).resolve().parent


def log_dir() -> Path:
    base = os.environ.get("LOCALAPPDATA") or str(Path.home())
    return Path(base) / "FoloOS" / "CodexBridge"


class _LogStream:
    """把 print() 重定向进日志文件。"""

    def __init__(self, handle) -> None:
        self._handle = handle
        self._lock = threading.Lock()

    def write(self, text: str) -> int:
        if not text:
            return 0
        with self._lock:
            self._handle.write(text)
            self._handle.flush()
        return len(text)

    def flush(self) -> None:
        with self._lock:
            self._handle.flush()

    def isatty(self) -> bool:
        return False


def setup_logging() -> Path:
    """重定向 stdout/stderr 到日志文件,返回日志路径。"""
    directory = log_dir()
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "bridge.log"
    # 单文件轮转:超过 2MB 就另存一份,避免长期运行把磁盘写穿。
    try:
        if path.is_file() and path.stat().st_size > 2 * 1024 * 1024:
            path.replace(directory / "bridge.log.1")
    except OSError:
        pass
    handle = open(path, "a", encoding="utf-8", buffering=1)
    print(f"\n===== {time.strftime('%Y-%m-%d %H:%M:%S')} 托盘程序启动 =====")
    sys.stdout = _LogStream(handle)  # type: ignore[assignment]
    sys.stderr = sys.stdout  # type: ignore[assignment]
    return path


def show_message(title: str, text: str, error: bool = False) -> None:
    """弹一个原生 MessageBox。

    托盘程序没有控制台,配对失败这类必须让用户看到的信息只能用弹窗。
    调用方都在后台线程,所以不会卡住托盘的消息循环。
    """
    flags = 0x00000010 if error else 0x00000040  # MB_ICONERROR / MB_ICONINFORMATION
    try:
        ctypes.windll.user32.MessageBoxW(None, text, title, flags)
    except Exception:
        print(f"[提示框失败] {title}: {text}")


# ---------------------------------------------------------------------------
# 图标绘制
# ---------------------------------------------------------------------------

def make_tray_image(color: str, busy: bool = False):
    """生成托盘图标:状态色圆角底 + 白色对话气泡 + 气泡内的波形线。

    刻意保持极简 —— 托盘实际显示只有 16x16,任何精细细节都会糊成一团。
    真正传递状态的是底色,图形只负责让人认出"这是语音对话"。
    """
    from PIL import Image, ImageDraw

    size = 64
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((2, 2, 61, 61), radius=15, fill=color)
    # 白色对话气泡(实心,保证缩到 16px 依然清晰)
    draw.rounded_rectangle((12, 14, 52, 42), radius=10, fill="#FFFFFF")
    draw.polygon([(19, 42), (20, 54), (33, 42)], fill="#FFFFFF")
    # 气泡内波形线(用底色画,形成"镂空"效果)
    heights = (8, 15, 8) if not busy else (15, 8, 15)
    for index, height in enumerate(heights):
        x = 26 + index * 6
        half = height // 2
        draw.line((x, 28 - half, x, 28 + half), fill=color, width=3)
    return image


def make_ico(path: Path, color: str = "#2D7FF9") -> None:
    """给 EXE 生成图标文件。"""
    image = make_tray_image(color)
    image.save(
        path,
        format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
    )


# ---------------------------------------------------------------------------
# 可观测的桥接
# ---------------------------------------------------------------------------

class SingleInstance:
    """进程级单实例锁。

    不加这个,用户双击两次就会有两个托盘图标、两套桥接线程去抢同一个设备连接,
    表现是设备侧连接反复被踢、状态在两个图标之间跳,极难自查。
    用 Windows 命名互斥体实现:第二个实例拿到 ERROR_ALREADY_EXISTS 就直接退出,
    并把已有窗口的托盘图标提示出来。
    """

    _ERROR_ALREADY_EXISTS = 183
    _NAME = "Global\\FoloOSCodexBridgeSingleton"

    def __init__(self) -> None:
        self._handle = None
        self.already_running = False
        try:
            kernel32 = ctypes.windll.kernel32
            self._handle = kernel32.CreateMutexW(None, True, self._NAME)
            if not self._handle:
                return
            self.already_running = (
                kernel32.GetLastError() == self._ERROR_ALREADY_EXISTS
            )
        except Exception:
            # 非 Windows 或调用失败时不阻断启动 —— 单实例是便利性优化,
            # 不该因为它拿不到互斥体就让整个程序起不来。
            print(traceback.format_exc())

    def release(self) -> None:
        if self._handle:
            try:
                ctypes.windll.kernel32.ReleaseMutex(self._handle)
                ctypes.windll.kernel32.CloseHandle(self._handle)
            except Exception:
                pass
            self._handle = None


class BridgeStopped(Exception):
    """请求正常停止桥接线程。"""


class BridgeReconnect(RuntimeError):
    """请求立即重连。刻意继承 RuntimeError —— 父类的重连分支正好捕获它,
    这样"手动重连"不需要另写一套重连逻辑。"""


def _import_bridge():
    """延迟导入桥接模块。

    放在函数里而不是模块顶层,是为了让托盘自身的启动失败(比如缺 pystray)
    能报出清晰的错误,而不是先被桥接模块的导入异常盖掉。
    """
    from mac_bridge import NetworkBridge, open_codex_thread  # noqa: F401
    from windows_bridge import WindowsSpeechRecognizer, WindowsSpeechSynthesizer
    from windows_codex_backend import WindowsCodexAppServer

    return {
        "NetworkBridge": NetworkBridge,
        "WindowsSpeechRecognizer": WindowsSpeechRecognizer,
        "WindowsSpeechSynthesizer": WindowsSpeechSynthesizer,
        "WindowsCodexAppServer": WindowsCodexAppServer,
        "open_codex_thread": open_codex_thread,
    }


class ObservableBridge:
    """NetworkBridge 的可观测包装:只挂钩,不改协议。

    状态采集点选的是运行循环里**必然经过**的几处:
      open()              -> 连接中 -> 已连接
      handle_line()       -> 设备上行事件(录音/提交/审批/取消)
      deliver()           -> 电脑下行消息(运行中/待审批/完成/出错)
      flush_pending()     -> 每轮循环最先调用,用来实现"停止"与"手动重连"
    """

    def __init__(self, host, port, token, *, on_state, stop_event,
                 reconnect_event, verbose: bool = False) -> None:
        modules = _import_bridge()
        tools_dir = resolve_tools_dir()
        self._on_state = on_state
        self._stop_event = stop_event
        self._reconnect_event = reconnect_event
        self._task_count = 0
        self._lock = threading.Lock()

        speech = modules["WindowsSpeechRecognizer"](tools_dir)
        tts = modules["WindowsSpeechSynthesizer"](tools_dir)
        codex = modules["WindowsCodexAppServer"](
            Path.home(),
            verbose=verbose,
            thread_opener=modules["open_codex_thread"],
        )
        bridge_cls = modules["NetworkBridge"]
        tray_cls = type("TrayBridge", (bridge_cls,), self._hooks(bridge_cls))
        self._bridge = tray_cls(
            host,
            port,
            token,
            scenario=None,
            speech=speech,
            tts=tts,
            codex=codex,
            verbose=verbose,
        )

    # -- 状态上报 ---------------------------------------------------------

    def _notify(self, state: str, detail: str) -> None:
        try:
            self._on_state(state, detail)
        except Exception:
            print(f"[状态回调失败] {state} {detail}")

    def _bump_task(self) -> None:
        with self._lock:
            self._task_count += 1

    @property
    def task_count(self) -> int:
        with self._lock:
            return self._task_count

    def _state_from_uplink(self, event: dict) -> None:
        """设备 -> 电脑 的事件。"""
        kind = event.get("type")
        mapping = {
            "record_start": "正在录音…",
            "record_stop": "正在识别语音…",
            "voice_command": "已提交给 Codex…",
            "approval_decision": "正在发送审批结果…",
            "cancel_task": "正在取消任务…",
            "codex_catalog_request": "正在读取任务列表…",
            "codex_project_select": "正在切换项目…",
            "codex_thread_select": "正在打开会话…",
            "codex_thread_new": "正在新建会话…",
            "wifi_setup": "正在配置网络…",
        }
        detail = mapping.get(kind or "")
        if detail:
            self._notify(STATE_BUSY, detail)

    def _state_from_downlink(self, message: dict) -> None:
        """电脑 -> 设备 的消息。"""
        kind = message.get("type")
        if kind == "codex_state":
            state = message.get("state")
            if state == "busy":
                self._notify(STATE_BUSY, f"Codex 运行中：{message.get('running', 0)} 个任务")
            elif state == "fail":
                self._notify(STATE_ERROR, f"任务失败待查看：{message.get('failed', 0)}")
            else:
                self._notify(STATE_READY, "任务完成待查看" if state == "done" else "Codex 空闲")
        elif kind == "task_status":
            self._notify(STATE_BUSY, str(message.get("text") or "Codex 运行中"))
        elif kind == "approval":
            self._notify(STATE_BUSY, "等待你审批")
        elif kind == "done":
            self._bump_task()
            self._notify(STATE_READY, "任务完成")
        elif kind == "error":
            self._notify(STATE_ERROR, str(message.get("text") or "桥接出错"))
        elif kind == "transcript":
            self._notify(STATE_BUSY, "已识别,等待确认")

    # -- 挂到桥接类上的钩子 -----------------------------------------------

    def _hooks(self, parent) -> dict:
        """构造挂到 NetworkBridge 子类上的钩子。

        parent 显式传入,不用 type(self).__mro__[1] 去反查 —— 后者依赖
        继承链的位置,一旦中间插入基类就会静默调错方法。
        """
        outer = self

        def open_(self) -> None:  # noqa: N805
            outer._notify(STATE_CONNECTING, "正在查找设备…")
            parent.open(self)
            host = getattr(self, "host", "")
            port = getattr(self, "network_port", "")
            outer._notify(STATE_READY, f"已连接 {host}:{port}")

        def handle_line(self, raw_line: bytes) -> None:  # noqa: N805
            try:
                parsed = json.loads(raw_line.strip().decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                parsed = None
            if isinstance(parsed, dict):
                outer._state_from_uplink(parsed)
            parent.handle_line(self, raw_line)
            # 语音识别是异步的(poll_speech 拿到结果后才发 transcript/error),
            # 失败原因留在 recognizer 实例上。这里顺手看一眼,失败就立刻
            # 把原因顶到托盘提示上 —— 否则用户只知道"没反应",不知道是
            # 麦克风没收到声音还是系统缺中文语音包。
            recognizer = getattr(self, "speech", None)
            reason = getattr(recognizer, "last_error", "") if recognizer else ""
            if reason:
                setattr(recognizer, "last_error", "")
                outer._notify(STATE_ERROR, f"语音识别失败：{reason}")

        def deliver(self, message: dict) -> None:  # noqa: N805
            # Update the tray only after the device message has been sent.
            parent.deliver(self, message)
            outer._state_from_downlink(message)

        def flush_pending(self) -> None:  # noqa: N805
            # 每轮循环最先调用且无早退分支 —— 拿它当"心跳点"最稳。
            if outer._stop_event.is_set():
                raise BridgeStopped()
            if outer._reconnect_event.is_set():
                outer._reconnect_event.clear()
                outer._notify(STATE_CONNECTING, "正在重连…")
                raise BridgeReconnect("手动重连")
            parent.flush_pending(self)

        return {
            "open": open_,
            "handle_line": handle_line,
            "deliver": deliver,
            "flush_pending": flush_pending,
        }

    # -- 生命周期 ---------------------------------------------------------

    def run(self) -> None:
        """镜像 NetworkBridge.run() 的重连循环,插入状态上报。

        **为什么这里复制了父类的循环而不调用 super().run():**
        父类 run() 是 while True 且吞掉异常,没有任何退出钩子,
        无法在异常分支上报状态、也无法响应停止请求。复制的是 20 行
        结构稳定的重连外壳;真正的协议逻辑(open/_run_connected_loop/
        handle_line)仍然全部走父类,没有第二份实现。
        """
        bridge = self._bridge
        last_error = ""
        try:
            self._notify(STATE_STARTING, "正在启动 Codex…")
            if bridge.codex is not None:
                bridge.codex.start()
            self._notify(STATE_CONNECTING, "正在查找设备…")
            while not self._stop_event.is_set():
                try:
                    if bridge.sock is None:
                        bridge.open()
                    last_error = ""
                    bridge._run_connected_loop()
                except BridgeStopped:
                    break
                except (OSError, RuntimeError) as error:
                    detail = str(error)
                    if detail != last_error:
                        self._notify(STATE_ERROR, detail)
                        last_error = detail
                    if self._stop_event.wait(1.0):
                        break
                finally:
                    bridge.stop_heartbeat()
                    bridge.close()
        finally:
            self._notify(STATE_STOPPED, "已停止")
            if bridge.codex is not None:
                try:
                    bridge.codex.close()
                except Exception:
                    print(traceback.format_exc())

    def request_stop(self) -> None:
        self._stop_event.set()

    def request_reconnect(self) -> None:
        self._reconnect_event.set()


# ---------------------------------------------------------------------------
# 配对
# ---------------------------------------------------------------------------

def load_pairing() -> tuple[str, int, str] | None:
    """读 %LOCALAPPDATA%\\FoloOS\\bridge.json。与 windows_bridge.py 共用一份。"""
    try:
        from pair_device import config_path
    except ImportError:
        return None
    path = config_path()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    token = str(data.get("token") or "")
    if len(token) < 16:
        return None
    return str(data.get("host") or ""), int(data.get("port") or 8765), token


def pair_now(token: str | None = None) -> tuple[str, int, str]:
    """发现设备并完成 TOFU 配对,返回 (host, port, token)。"""
    from pair_device import BRIDGE_PORT, TOKEN_BYTES, discover, pair, save

    host, port = discover(3.0)
    chosen = token or secrets.token_hex(TOKEN_BYTES)
    pair(host, port, chosen)
    save(host, port, chosen)
    return host, port, chosen


# ---------------------------------------------------------------------------
# 开机自启
# ---------------------------------------------------------------------------

_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
_RUN_VALUE = "FoloOSCodexBridge"


def command_path() -> str:
    """自启要执行的命令:打包后是 EXE 本身,源码运行时是 pythonw + 脚本。"""
    if getattr(sys, "frozen", False):
        return f'"{sys.executable}"'
    script = Path(__file__).resolve()
    pythonw = Path(sys.executable).with_name("pythonw.exe")
    exe = pythonw if pythonw.is_file() else Path(sys.executable)
    return f'"{exe}" "{script}"'


def autostart_enabled() -> bool:
    try:
        import winreg

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY) as key:
            winreg.QueryValueEx(key, _RUN_VALUE)
        return True
    except OSError:
        return False
    except ImportError:
        return False


def set_autostart(enabled: bool) -> None:
    import winreg

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, _RUN_KEY) as key:
        if enabled:
            winreg.SetValueEx(key, _RUN_VALUE, 0, winreg.REG_SZ, command_path())
        else:
            try:
                winreg.DeleteValue(key, _RUN_VALUE)
            except OSError:
                pass


# ---------------------------------------------------------------------------
# 托盘界面
# ---------------------------------------------------------------------------

class TrayApp:
    def __init__(self) -> None:
        import pystray

        self._pystray = pystray
        self._state_lock = threading.Lock()
        self._state = STATE_UNPAIRED
        self._detail = "尚未配对设备"
        self._stopping = False

        self._stop_event = threading.Event()
        self._reconnect_event = threading.Event()
        self._bridge_queue: queue.Queue = queue.Queue()
        self._bridge: ObservableBridge | None = None
        self._bridge_thread: threading.Thread | None = None

        self._icon = pystray.Icon(
            "folos-codex-bridge",
            make_tray_image("#667085"),
            "Codex 桥接",
            menu=self._build_menu(),
        )

    # -- 状态 -------------------------------------------------------------

    def _apply_state(self, state: str, detail: str = "") -> None:
        with self._state_lock:
            self._state = state
            self._detail = detail
        label, color = STATE_STYLE.get(state, STATE_STYLE[STATE_STOPPED])
        tasks = self._bridge.task_count if self._bridge else 0
        tooltip = f"Codex 桥接 · {label}"
        if detail:
            tooltip += f"\n{detail}"
        tooltip += f"\n已处理 {tasks} 个任务"
        # 托盘 tooltip 上限 127 字符,超了会被系统截断甚至不显示。
        self._icon.title = tooltip[:120]
        try:
            self._icon.icon = make_tray_image(color, busy=state == STATE_BUSY)
        except Exception:
            print(traceback.format_exc())

    def _status_line(self) -> str:
        with self._state_lock:
            state, detail = self._state, self._detail
        label = STATE_STYLE.get(state, STATE_STYLE[STATE_STOPPED])[0]
        return f"{label} · {detail}" if detail else label

    def _address_line(self) -> str:
        pairing = load_pairing()
        if pairing is None:
            return "设备地址：未配对"
        return f"设备地址：{pairing[0] or '自动发现'}:{pairing[1]}"

    # -- 菜单 -------------------------------------------------------------

    def _build_menu(self):
        pystray = self._pystray
        Item = pystray.MenuItem
        return pystray.Menu(
            Item(lambda _item: f"Codex 桥接 · {self._status_line()}", None, enabled=False),
            Item(lambda _item: self._address_line(), None, enabled=False),
            pystray.Menu.SEPARATOR,
            Item("配对设备…", self._on_pair),
            Item("立即重连", self._on_reconnect),
            Item("打开日志目录", self._on_open_logs),
            pystray.Menu.SEPARATOR,
            Item(
                "开机自动启动",
                self._on_toggle_autostart,
                checked=lambda _item: autostart_enabled(),
            ),
            Item("查看当前配置", self._on_show_config),
            pystray.Menu.SEPARATOR,
            Item("退出", self._on_quit, default=True),
        )

    # -- 菜单动作(都在托盘线程里,不能阻塞) -----------------------------

    def _on_quit(self, _icon, _item) -> None:
        self._stopping = True
        if self._bridge is not None:
            self._bridge.request_stop()
        self._icon.stop()

    def _on_reconnect(self, _icon, _item) -> None:
        if self._bridge is None:
            self._start_bridge()
            return
        self._bridge.request_reconnect()

    def _on_open_logs(self, _icon, _item) -> None:
        directory = log_dir()
        directory.mkdir(parents=True, exist_ok=True)
        try:
            os.startfile(str(directory))
        except OSError as error:
            show_message("打开日志目录失败", str(error), error=True)

    def _on_toggle_autostart(self, _icon, _item) -> None:
        try:
            set_autostart(not autostart_enabled())
        except OSError as error:
            show_message("设置开机启动失败", str(error), error=True)

    def _on_show_config(self, _icon, _item) -> None:
        pairing = load_pairing()
        if pairing is None:
            show_message(
                "尚未配对",
                "还没有找到配对信息。\n\n"
                "请先点「配对设备…」：\n"
                "① 确认设备已连上 2.4GHz 局域网（设备端：设置 → Wi-Fi）\n"
                "② 确认电脑与设备在同一网段\n"
                "然后重试配对。",
            )
            return
        host, port, token = pairing
        show_message(
            "当前配置",
            f"设备地址：{host or '自动发现'}:{port}\n"
            f"配对密钥：{token}\n"
            f"Codex 工作目录：{Path.home()}\n"
            f"日志目录：{log_dir()}",
        )

    def _on_pair(self, _icon, _item) -> None:
        # 发现要广播等 3 秒,不能在托盘线程里做,否则菜单期间界面假死。
        threading.Thread(target=self._pair_worker, name="pair", daemon=True).start()

    def _pair_worker(self) -> None:
        self._apply_state(STATE_CONNECTING, "正在广播查找设备…")
        # 配对前先停掉现有桥接,避免它占着 socket 干扰握手。
        if self._bridge is not None:
            self._bridge.request_stop()
            self._join_bridge(timeout=5.0)
            self._bridge = None
        try:
            host, port, _token = pair_now()
        except Exception as error:
            self._apply_state(
                STATE_UNPAIRED if load_pairing() is None else STATE_ERROR,
                "配对失败",
            )
            print(f"[配对失败] {error}")
            show_message("配对失败", str(error), error=True)
            return
        self._apply_state(STATE_CONNECTING, f"配对成功 {host}:{port}，正在启动桥接…")
        self._start_bridge()

    # -- 桥接线程 ---------------------------------------------------------

    def _start_bridge(self) -> None:
        if self._bridge_thread is not None and self._bridge_thread.is_alive():
            return
        pairing = load_pairing()
        if pairing is None:
            self._apply_state(STATE_UNPAIRED, "尚未配对设备，请点「配对设备…」")
            return
        self._stop_event.clear()
        self._reconnect_event.clear()
        host, port, token = pairing
        self._bridge = ObservableBridge(
            host,
            port,
            token,
            on_state=self._apply_state,
            stop_event=self._stop_event,
            reconnect_event=self._reconnect_event,
        )
        thread = threading.Thread(target=self._bridge_worker, name="bridge", daemon=True)
        self._bridge_thread = thread
        thread.start()

    def _bridge_worker(self) -> None:
        try:
            self._bridge.run()  # type: ignore[union-attr]
        except BridgeStopped:
            pass
        except Exception:
            print(traceback.format_exc())
            self._apply_state(STATE_ERROR, "桥接异常退出")

    def _join_bridge(self, timeout: float) -> None:
        thread = self._bridge_thread
        if thread is not None and thread.is_alive():
            thread.join(timeout=timeout)
        self._bridge_thread = None

    # -- 启动 -------------------------------------------------------------

    def _on_ready(self, icon) -> None:
        icon.visible = True
        pairing = load_pairing()
        if pairing is None:
            self._apply_state(STATE_UNPAIRED, "尚未配对设备，请点「配对设备…」")
        else:
            self._start_bridge()

    def run(self) -> None:
        print("托盘程序已启动,正在进入消息循环")
        try:
            self._icon.run(setup=self._on_ready)
        finally:
            if self._bridge is not None:
                self._bridge.request_stop()
            self._join_bridge(timeout=6.0)
            print("托盘程序已退出")


def main() -> int:
    setup_logging()
    instance = SingleInstance()
    if instance.already_running:
        print("检测到已有实例在运行,本次启动退出")
        show_message(
            "Codex 桥接已在运行",
            "托盘里已经有一个 Codex 桥接图标了，请不要重复启动。\n\n"
            "如果找不到那个图标，点击任务栏的 ^ 展开隐藏图标区域看看。",
        )
        return 0
    try:
        app = TrayApp()
    except Exception:
        print(traceback.format_exc())
        show_message(
            "启动失败",
            "桥接托盘程序无法启动。\n\n"
            f"详情见日志：{log_dir() / 'bridge.log'}",
            error=True,
        )
        return 1
    try:
        app.run()
    except KeyboardInterrupt:
        return 0
    finally:
        instance.release()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
