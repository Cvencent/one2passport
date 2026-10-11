"""静态验证:确认 plays/438 首页的显示链路是完整的。

## 为什么要有这个脚本

我连续两次修了"闪烁",两次都没修到真因。真因是:
**首页建好了但从来没被 lv_screen_load() 过**,所以屏幕上一直是 LEO 界面,
而每次 UI 状态更新调 radio_ui_show_main() 就重新 load 一次 -> 白屏。

代码审查已经很难看出这种"少一行"的问题,所以写脚本做**路径完整性检查**:
从"建屏函数"到"实际显示"这条链上,该有的调用是否都在。
"""
import re
import sys
from pathlib import Path

MAIN = Path(__file__).resolve().parent / "main"

problems = []
notes = []


def read(name):
    return (MAIN / name).read_text(encoding="utf-8")


home = read("radio_home.cc")
ui = read("radio_ui.cc")
cc = read("main.cc")
apps = read("app_apps.cc")

# ── 检查 1: 建屏函数的首次创建路径必须有 lv_screen_load ──
print("[检查1] radio_home_screen_create 的首次创建路径")
# 取函数体
m = re.search(r"lv_obj_t \*radio_home_screen_create\(\)\s*\{(.*?)\n\}", home, re.S)
if not m:
    problems.append("找不到 radio_home_screen_create 函数")
else:
    body = m.group(1)
    # 拆成"已存在则 return"的分支与"首次创建"分支
    early_return_idx = body.find("return s_screen;")
    load_idx = body.rfind("lv_screen_load(s_screen);")
    if load_idx == -1:
        problems.append("首次创建路径没有 lv_screen_load(s_screen) —— 页面建好但不会显示")
    elif load_idx < early_return_idx:
        problems.append("lv_screen_load 只在早退分支里,首次创建不会显示页面")
    else:
        notes.append(f"首次创建路径含 lv_screen_load(位置 {load_idx} > 早退 {early_return_idx})")

# ── 检查 2: 早退分支必须用 lv_screen_active() 判断 ──
print("[检查2] 幂等分支的正确性")
if "lv_screen_active()" not in home:
    problems.append("幂等分支没有用 lv_screen_active() 判断,会反复重载")
else:
    notes.append("已用 lv_screen_active() 做真幂等判断")

# ── 检查 3: show_main / show_home 里不能有多余的 lv_screen_load ──
print("[检查3] 入口函数是否有多余的重复 load")
# 用行区间精确截取函数体,不用正则贪婪匹配 —— 后者会跨到下一个函数,
# 误抓到 radio_ui_show_leo_main() 里的 lv_screen_load(s_screen)(那是 LEO 的屏,同名变量)。
def function_body(src, signature):
    start = src.find(signature)
    if start == -1:
        return None
    brace = src.find("{", start)
    if brace == -1:
        return None
    depth = 0
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[brace:i + 1]
    return None


for fn in ("radio_ui_show_main", "radio_ui_show_home"):
    fbody = function_body(ui, f"void {fn}()")
    if fbody is None:
        problems.append(f"找不到 {fn} 的函数体")
        continue
    # 只统计真正执行的语句,剔除以 // 开头的中文注释行
    code_only = "\n".join(
        line for line in fbody.splitlines() if not line.strip().startswith("//")
    )
    n = code_only.count("lv_screen_load")
    if n > 0:
        problems.append(f"{fn} 里有 {n} 处实际执行的 lv_screen_load —— 会重复重载")
    else:
        notes.append(f"{fn} 无多余 load(交给建屏函数统一决策)")

# ── 检查 4: 首页确实被 lv_screen_load 过(历史 bug:屏建好了却没 load) ──
print("[检查4] 启动首页的显示调用")
registry = read("app_registry.cc")
if "lv_screen_load(s_home_scr)" not in registry:
    problems.append("app_registry.cc 没有 lv_screen_load(s_home_scr),首页建好但不会被显示")
else:
    notes.append("app_registry.cc 用 lv_screen_load(s_home_scr) 真正显示首页")

# ── 检查 5: 离开 LEO 页面前要停 50ms 定时器 ──
print("[检查5] 50ms 定时器的看门狗")
if "s_page_visible" not in ui:
    problems.append("缺少 s_page_visible,timer_callback 会写已销毁对象")
else:
    if "radio_ui_leave_leo_page" not in apps:
        problems.append("进入 Codex 页前没有调 radio_ui_leave_leo_page()")
    else:
        notes.append("离开 LEO 页面前会停掉 50ms 定时器")

# ── 检查 6: 定时器单例 ──
print("[检查6] 首页定时器单例")
if re.search(r"if \(s_tick == nullptr\)\s*\{", home):
    notes.append("首页定时器已单例化")
else:
    problems.append("首页定时器没有单例保护,反复进出会泄漏")

print()
print("=" * 56)
for n in notes:
    print(f"  OK   {n}")
print("-" * 56)
if problems:
    for p in problems:
        print(f"  NG   {p}")
    print("=" * 56)
    print(f"结论: {len(problems)} 处问题,不能交付")
    sys.exit(1)
print("结论: 全部通过 —— 显示链路完整,可以编译交付")
print("=" * 56)
