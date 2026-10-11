"""开机自启的写入/读取/删除往返测试(可逆,结束后恢复原状)。"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from tray_app import _RUN_KEY, _RUN_VALUE, autostart_enabled, command_path, set_autostart

import winreg

before = autostart_enabled()
print(f"初始状态: {'已启用' if before else '未启用'}")
print(f"将写入的命令: {command_path()}")

try:
    set_autostart(True)
    enabled = autostart_enabled()
    print(f"设为启用后读取: {enabled}")
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY) as key:
        value, _kind = winreg.QueryValueEx(key, _RUN_VALUE)
    print(f"注册表实际值: {value!r}")
    assert enabled, "写入注册表后未能读到"
    assert value == command_path(), "注册表值与预期命令不一致"

    set_autostart(False)
    after_disable = autostart_enabled()
    print(f"设为禁用后读取: {after_disable}")
    assert not after_disable, "删除注册表项后仍读到"
    print("往返测试 PASS")
finally:
    # 恢复初始状态,不留痕迹
    set_autostart(before)
    print(f"已恢复初始状态: {'已启用' if autostart_enabled() else '未启用'}")
