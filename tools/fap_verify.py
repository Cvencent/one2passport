"""AI Passport 真机功能验证(单次串口会话, 避免反复复位)。

流程: 等启动 -> 首页截屏 -> 进 Codex(2) -> 截屏 -> 长按UP退出 Codex ->
      回菜单/首页 -> 截屏 -> 检查收音机是否恢复且无内存不足日志。

用法: python tools/fap_verify.py COM4 [outdir]
"""
from __future__ import annotations

import os
import re
import sys
import time

import serial

from fap_device import write_png

CMD = "FAP_SCREENSHOT_V1"
HEADER = re.compile(rb"FAP_SCREENSHOT_V1 (\d+) (\d+) RGB565LE (\d+)\n")
APP_KEYS = {"1": "收音机", "2": "Codex", "3": "小智", "4": "设置"}


class Device:
    def __init__(self, port: str):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.setDTR(False)
        self.ser.setRTS(False)

    def read_for(self, seconds: float) -> str:
        end = time.time() + seconds
        out = []
        while time.time() < end:
            chunk = self.ser.read(8192)
            if chunk:
                out.append(chunk.decode("utf-8", "replace"))
        return "".join(out)

    def key(self, chars: str, gap: float = 1.5) -> str:
        for ch in chars:
            self.ser.write(ch.encode("ascii"))
            self.ser.flush()
            time.sleep(gap)
        return self.read_for(1.0)

    def screenshot(self, path: str) -> tuple[int, int, int]:
        self.ser.reset_input_buffer()
        self.ser.write((CMD + "\n").encode("ascii"))
        self.ser.flush()
        head = b""
        deadline = time.time() + 20
        m = None
        while time.time() < deadline:
            head += self.ser.read(8192)
            m = HEADER.search(head)
            if m:
                break
        if not m:
            raise TimeoutError("no screenshot header")
        head = head[m.start():]
        nl = head.find(b"\n")
        parts = head[:nl].decode("ascii", "replace").split()
        w, h, nbytes = int(parts[1]), int(parts[2]), int(parts[4])
        payload = bytearray(head[nl + 1:])
        while len(payload) < nbytes:
            chunk = self.ser.read(nbytes - len(payload))
            if chunk:
                payload += chunk
            elif time.time() > deadline + 10:
                raise TimeoutError("pixel timeout")
        write_png(path, w, h, bytes(payload[:nbytes]))
        return w, h, distinct_colors(path)

    def close(self):
        self.ser.close()


def distinct_colors(path: str) -> int:
    import zlib
    buf = open(path, "rb").read()
    off, idat, w, h = 8, b"", 0, 0
    while off < len(buf):
        ln = int.from_bytes(buf[off:off + 4], "big")
        tag = buf[off + 4:off + 8]
        d = buf[off + 8:off + 8 + ln]
        if tag == b"IHDR":
            w, h = int.from_bytes(d[0:4], "big"), int.from_bytes(d[4:8], "big")
        elif tag == b"IDAT":
            idat += d
        off += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    seen = set()
    for y in range(h):
        for x in range(w):
            o = y * stride + 1 + x * 3
            seen.add(raw[o:o + 3])
    return len(seen)


def summarize(log: str, label: str) -> None:
    print(f"--- {label} ---")
    keys = ("enter_app", "app_fw", "heapdiag", "radio pool", "release", "CBDP",
            "SUBSCRIBE", "insufficient contiguous", "内存不足", "decoder needs",
            "radio prewarm", "re-acquire", "got IP", "sta ip", "codex", "catalog")
    for line in log.splitlines():
        if any(k.lower() in line.lower() for k in keys):
            print("   " + line.strip())


def main(argv):
    port = argv[1] if len(argv) > 1 else "COM4"
    outdir = argv[2] if len(argv) > 2 else "."
    dev = Device(port)
    ok = True
    try:
        print("waiting for boot / reconnect ...")
        boot = dev.read_for(25)
        summarize(boot, "boot")
        time.sleep(2)

        w, h, c = dev.screenshot(os.path.join(outdir, "01_home.png"))
        print(f"[1] home        {w}x{h} colors={c}")
        ok &= c > 5

        log = dev.key("2")  # 进入 Codex
        time.sleep(3)
        summarize(log, "enter codex")
        w, h, c = dev.screenshot(os.path.join(outdir, "02_codex.png"))
        print(f"[2] codex page  {w}x{h} colors={c}")
        ok &= c > 5

        log = dev.key("k")  # 长按 UP = 退出 Codex 回菜单
        time.sleep(3)
        summarize(log, "exit codex")
        w, h, c = dev.screenshot(os.path.join(outdir, "03_menu.png"))
        print(f"[3] menu/home   {w}x{h} colors={c}")
        ok &= c > 5

        # 回首页 (菜单长按UP=go_home) 并确认收音机恢复正常
        log = dev.key("k")
        time.sleep(2)
        log += dev.read_for(12)
        summarize(log, "back home + radio")
        w, h, c = dev.screenshot(os.path.join(outdir, "04_home_back.png"))
        print(f"[4] home back   {w}x{h} colors={c}")
        ok &= c > 5

        low_mem = ("insufficient contiguous" in log
                   or "内存不足" in log
                   or "decoder needs" in log)
        print(f"[5] radio low-memory message after Codex round-trip: {low_mem}")

        print("RESULT:", "PASS" if (ok and not low_mem) else "CHECK")
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
