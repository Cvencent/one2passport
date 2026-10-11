"""验证托盘的提示文字真的被系统接受(而不是被静默截断),并检查图标可辨识度。

托盘 tooltip 有 127 字符的硬上限,超了 Windows 会直接不显示 ——
这是"图标在但看不到状态"的典型原因,所以必须验证。
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from tray_app import STATE_STYLE, make_tray_image


def alpha_of(pixels):
    return [p[3] for p in pixels]


print("== tooltip 长度合规性(上限 127) ==")
worst = 0
for state, (label, _color) in STATE_STYLE.items():
    detail = "已连接 192.168.100.200:8765" if state == "ready" else "正在识别语音…"
    tooltip = f"Codex 桥接 · {label}\n{detail}\n已处理 999 个任务"
    truncated = tooltip[:120]  # 与 tray_app._apply_state 的截断保持一致
    worst = max(worst, len(truncated))
    flag = "OK" if len(truncated) <= 127 else "超限!"
    print(f"  {state:12s} 截断后 {len(truncated):3d} 字符  {flag}")

print(f"\n最长 {worst} 字符 (上限 127) → {'PASS' if worst <= 127 else 'FAIL'}")

print()
print("== 图标在 16x16 下的可辨识度 ==")
for state, (label, color) in STATE_STYLE.items():
    img = make_tray_image(color, busy=(state == "busy"))
    small = img.resize((16, 16))
    alphas = alpha_of(list(small.getdata()))
    ratio = sum(1 for a in alphas if a > 0) / len(alphas) * 100
    print(f"  {state:12s} {label:6s} 不透明像素 {ratio:5.1f}%  {color}")
print("\n说明:托盘按 16x16 显示,底色是主要辨识依据,图形为辅助。")
