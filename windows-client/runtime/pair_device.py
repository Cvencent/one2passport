#!/usr/bin/env python3
"""PC 端首次配对：发现局域网上未配对的 AI Passport 并完成 TOFU 配对。

## 为什么需要这个脚本

FoloOS 原版的配对流程是：PC 端把一个 32 位 token 通过 USB 串口写进设备
(`windows_wifi_setup.ps1` 的 `wifi_setup` 事件),设备先把 token 落盘,之后
两边拿同一个 token 做 `bridge_auth`。

本工程基于 LEO RADIO,配网页由 `esp-wifi-connect` 组件提供 —— 那是一套**固定
表单**的 HTML 配网门户(SSID + 密码),无法像 FoloOS 那样在表单里附带一个
自定义 token 字段。因此设备端改成了 **TOFU(首次信任配对)**:

    设备本地 token 为空时,接受电脑端带来的第一个合法 token(长度 >= 16)
    并永久落盘;此后必须严格匹配。

于是 PC 端的流程相应简化为两步:
    1. 用户用手机/电脑连设备的配网热点,填 SSID+密码(esp-wifi-connect 的门户)
    2. 跑本脚本:UDP 广播发现设备 -> TCP 连上 -> 发 bridge_auth -> 设备首次配对
       -> 把 host/token 写进 %LOCALAPPDATA%\\FoloOS\\bridge.json

之后启动 windows_bridge.py 就无需再配对(它读的正是同一个 bridge.json)。

## 与 FoloOS 原版的差异

原版走串口,本脚本走网络。好处是不用插 USB、不占串口、跨平台一致;
代价是设备**必须先联上 WiFi**(未配网的设备 netif 未就绪,不会出现在局域网上,
也就收不到发现广播)。所以首次使用仍需要先过一次 esp-wifi-connect 的配网页。

用法:
    python pair_device.py                # 自动发现并配对
    python pair_device.py --host 1.2.3.4 # 跳过发现,直连指定地址
    python pair_device.py --token <hex>  # 手动指定 token(默认随机生成)
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import socket
import sys
import time
from pathlib import Path

DISCOVERY_PORT = 8764
BRIDGE_PORT = 8765
DISCOVERY_REQUEST = b"FOLOOS_DISCOVER_V1"
# 必须与设备端 wireless_bridge.c 里应答的 device 字段逐字一致。
DEVICE_MARKER = "FoloOS-AI-Passport"
TOKEN_BYTES = 16  # -> 32 个 hex 字符


def config_path() -> Path:
    """与 windows_wifi_setup.ps1 / mac_bridge.py 使用同一路径。"""
    base = os.environ.get("LOCALAPPDATA")
    if base:
        return Path(base) / "FoloOS" / "bridge.json"
    return Path.home() / ".local" / "share" / "foloos" / "bridge.json"


def discover(timeout: float = 3.0) -> tuple[str, int]:
    """UDP 广播发现设备。返回 (ip, tcp_port)。"""
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        udp.settimeout(timeout)
        # 同时发有限广播与 255.255.255.255:部分路由/多网卡环境只吃其中一种。
        for target in ("255.255.255.255", "<broadcast>"):
            try:
                udp.sendto(DISCOVERY_REQUEST, (target, DISCOVERY_PORT))
            except OSError:
                continue
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                payload, source = udp.recvfrom(1024)
            except socket.timeout:
                break
            try:
                reply = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if reply.get("device") == DEVICE_MARKER:
                return source[0], int(reply.get("port") or BRIDGE_PORT)
    finally:
        udp.close()
    raise RuntimeError(
        "未发现设备。请确认:① 设备已通过配网页连上同一个 2.4GHz 局域网;"
        "② 电脑与设备在同一网段(很多路由器默认隔离访客网络);"
        "③ 电脑防火墙允许 Python 收发 UDP 8764。"
    )


def pair(host: str, port: int, token: str) -> None:
    """连上设备并完成 bridge_auth。设备端首次会自动完成 TOFU 配对。"""
    print(f"正在连接 {host}:{port} ...")
    with socket.create_connection((host, port), timeout=5.0) as sock:
        sock.sendall(
            (json.dumps({"type": "bridge_auth", "token": token},
                        separators=(",", ":")) + "\n").encode("utf-8")
        )
        sock.settimeout(5.0)
        response = bytearray()
        while b"\n" not in response:
            chunk = sock.recv(1024)
            if not chunk:
                break
            response.extend(chunk)
    line = bytes(response).partition(b"\n")[0]
    if not line:
        # 设备端鉴权失败时不做任何应答,直接 shutdown+close(见 wireless_bridge.c
        # 的 authenticate_line -> client_replace(-1)),所以这里表现为"连上但没回包"。
        # 最可能的原因是设备已经配对过:**TOFU 只在设备本地 token 为空时生效**,
        # 一旦落盘(无论是之前用本脚本配对过,还是走过 USB 串口配网脚本下发的
        # token),后续必须严格匹配。此时应换用设备端已存的那个 token,
        # 而不是重新生成一个。
        raise RuntimeError(
            "设备连上了但没有回包 —— 通常是它已经配对过,而本次带来的 token "
            "与设备里存的不一致(TOFU 只对未配对设备生效)。请任选其一:\n"
            "  ① 改用设备端已存的 token 重跑:python pair_device.py --token <旧token>\n"
            "  ② 若已找不到旧 token,需要先清掉设备端的配对(NVS 擦除/恢复出厂)再重新配对。\n"
            "  注:若此前用过「2-配置设备WiFi」串口脚本,它下发的 token 就是设备端已存的那个,"
            "直接复用 %LOCALAPPDATA%\\FoloOS\\bridge.json 里的 token 即可,无需重新配对。"
        )
    try:
        reply = json.loads(line.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"设备回包无法解析:{line!r}") from error
    if reply.get("type") != "bridge_auth_ok":
        raise RuntimeError(f"设备拒绝了配对:{reply!r}")


def save(host: str, port: int, token: str) -> Path:
    path = config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps({"host": host, "port": port, "token": token},
                   ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="AI Passport 首次配对(TOFU)")
    parser.add_argument("--host", help="跳过 UDP 发现,直连该地址")
    parser.add_argument("--port", type=int, default=BRIDGE_PORT)
    parser.add_argument("--token", help="手动指定 token(默认随机 32 位 hex)")
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args(argv)

    token = args.token or secrets.token_hex(TOKEN_BYTES)
    if len(token) < 16:
        print("失败:token 至少要 16 个字符(设备端会拒收更短的)。", file=sys.stderr)
        return 1

    try:
        if args.host:
            host, port = args.host, args.port
        else:
            print("正在广播发现设备 ...")
            host, port = discover(args.timeout)
            print(f"发现设备:{host}:{port}")
        pair(host, port, token)
    except (OSError, RuntimeError) as error:
        print(f"配对失败:{error}", file=sys.stderr)
        return 1

    path = save(host, port, token)
    print()
    print("配对成功。")
    print(f"  设备地址 : {host}:{port}")
    print(f"  配对密钥 : {token}")
    print(f"  已写入   : {path}")
    print()
    print("现在可以启动桥接:python windows_bridge.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
