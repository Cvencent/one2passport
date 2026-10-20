"""FAP_SCREENSHOT_V1 + serial-key 测试工具(AI Passport 真机自动化验证)。

设备端 main.cc 注册了两个串口测试钩子:
  - FAP_SCREENSHOT_V1  : 请求一帧 RGB565LE 截屏(见 main/fap_screenshot.c)
  - 单字符按键注入     : u/d/o=上下确认点击, l=OK长按, k=UP长按, j=DOWN长按, r=OK释放
  - 数字 1..4          : 直接跳转到 收音机/Codex/小智/设置

用法:
  python tools/fap_device.py shot  COM4 out.png
  python tools/fap_device.py key   COM4 udol
  python tools/fap_device.py watch COM4 30
"""
from __future__ import annotations

import binascii
import struct
import sys
import time
import zlib

import serial

CMD = "FAP_SCREENSHOT_V1"


def open_port(port: str, baud: int = 115200) -> serial.Serial:
    ser = serial.Serial(port, baud, timeout=0.2)
    # 不要拉 DTR/RTS: 否则会复位设备。
    ser.setDTR(False)
    ser.setRTS(False)
    return ser


def _await_header(ser: serial.Serial, deadline: float) -> bytes:
    """Wait for a real "FAP_SCREENSHOT_V1 <w> <h> RGB565LE <n>" header line.

    The boot log also contains the bare command name, so a plain substring
    search would match the wrong line; require the full 5-token form.
    """
    import re
    pattern = re.compile(rb"FAP_SCREENSHOT_V1 (\d+) (\d+) RGB565LE (\d+)\n")
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            m = pattern.search(buf)
            if m:
                return bytes(buf[m.start():])
    return bytes(buf)


def take_screenshot(port: str, out_path: str, timeout: float = 20.0) -> tuple[int, int]:
    ser = open_port(port)
    try:
        ser.reset_input_buffer()
        ser.write((CMD + "\n").encode("ascii"))
        ser.flush()
        head = _await_header(ser, time.time() + timeout)
        line_end = head.find(b"\n")
        header = head[:line_end].decode("ascii", "replace").strip()
        parts = header.split()
        if len(parts) != 5 or parts[0] != CMD:
            raise RuntimeError("bad header: " + header)
        width = int(parts[1])
        height = int(parts[2])
        fmt = parts[3]
        nbytes = int(parts[4])
        if fmt != "RGB565LE":
            raise RuntimeError("unexpected format: " + fmt)
        payload = bytearray(head[line_end + 1:])
        while len(payload) < nbytes:
            chunk = ser.read(nbytes - len(payload))
            if not chunk:
                if time.time() > deadline_guard(timeout):
                    raise TimeoutError(f"pixel timeout {len(payload)}/{nbytes}")
                continue
            payload += chunk
        write_png(out_path, width, height, bytes(payload[:nbytes]))
        return width, height
    finally:
        ser.close()


def deadline_guard(timeout: float) -> float:
    return getattr(deadline_guard, "_deadline", time.time() + timeout)


def write_png(path: str, width: int, height: int, rgb565le: bytes) -> None:
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter type 0
        base = y * width * 2
        for x in range(width):
            lo = rgb565le[base + x * 2]
            hi = rgb565le[base + x * 2 + 1]
            v = lo | (hi << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            rows.append((r << 3) | (r >> 2))
            rows.append((g << 2) | (g >> 4))
            rows.append((b << 3) | (b >> 2))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", binascii.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def inject_keys(port: str, keys: str) -> None:
    ser = open_port(port)
    try:
        for ch in keys:
            ser.write(ch.encode("ascii"))
            ser.flush()
            time.sleep(1.2)
    finally:
        ser.close()


def watch(port: str, seconds: float) -> None:
    ser = open_port(port)
    end = time.time() + seconds
    try:
        while time.time() < end:
            data = ser.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        ser.close()


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 2
    mode = argv[1]
    if mode == "shot" and len(argv) >= 4:
        w, h = take_screenshot(argv[2], argv[3])
        print(f"saved {argv[3]} {w}x{h}")
        return 0
    if mode == "key" and len(argv) >= 4:
        inject_keys(argv[2], argv[3])
        print(f"injected keys {argv[3]}")
        return 0
    if mode == "watch" and len(argv) >= 3:
        watch(argv[2], float(argv[3]) if len(argv) > 3 else 10.0)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
