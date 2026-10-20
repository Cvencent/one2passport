"""真机验证 Codex 任务选择/新建(不依赖电脑端 Bridge 是否修好计数)。

流程: 进 Codex(2) -> 点"切换任务"(o 选中第2项) -> 截屏列表 ->
      UP 移到"新建任务" -> OK -> 截屏(看是否卡在加载/是否给出明确提示) ->
      长按UP退出 -> 截屏首页确认收音机恢复。

用法: python tools/fap_verify_codex.py COM4 [outdir]
"""
from __future__ import annotations
import os, sys, time
from fap_verify import Device, distinct_colors, summarize

def main(argv):
    port = argv[1] if len(argv) > 1 else "COM4"
    outdir = argv[2] if len(argv) > 2 else "."
    dev = Device(port)
    try:
        dev.read_for(3)
        log = dev.key("2", gap=1.5)   # 进 Codex
        time.sleep(3)
        summarize(log, "enter codex")
        dev.screenshot(os.path.join(outdir, "10_idle.png"))

        # 第2个动作按钮=切换任务: DOWN 一次选中第2项, OK 进入任务列表
        log = dev.key("d", gap=1.0) + dev.key("o", gap=1.0)
        time.sleep(3)
        summarize(log, "open task picker")
        dev.screenshot(os.path.join(outdir, "11_picker.png"))

        # 列表第1项是"新建任务": OK 创建
        log = dev.key("o", gap=1.0)
        time.sleep(6)
        summarize(log, "create new task")
        dev.screenshot(os.path.join(outdir, "12_after_create.png"))

        log = dev.key("k", gap=1.5)   # 长按 UP 退出
        time.sleep(2)
        log += dev.read_for(10)
        summarize(log, "exit")
        dev.screenshot(os.path.join(outdir, "13_home.png"))
        print("colors:", distinct_colors(os.path.join(outdir, "10_idle.png")),
              distinct_colors(os.path.join(outdir, "11_picker.png")),
              distinct_colors(os.path.join(outdir, "12_after_create.png")),
              distinct_colors(os.path.join(outdir, "13_home.png")))
        for tag in ("insufficient contiguous", "内存不足", "decoder needs"):
            print(f"low-mem[{tag}] = {tag in log}")
    finally:
        dev.close()
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
