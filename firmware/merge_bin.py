"""把 bootloader + 分区表 + app 合并成单个从 0x0 开始的镜像。

## 为什么必须做这个合并

官方 web flasher 的偏移选项是三选一:
    0x0    = 合并固件   <-- 默认选这一项
    0x8000 = 分区表
    0x10000= app

页面文案写的是「支持单个**合并固件**或多个分区 .bin 文件」——
也就是说它默认期望拿到一个**从 0x0 开始、含 bootloader 和分区表**的完整镜像。

上一轮变砖的根因就是这里: 我只给了纯 app 镜像(刷 0x10000 用),
而 web flasher 默认按 0x0 写, 于是 app 镜像被写到 bootloader/分区表位置,
设备启动时找不到有效 app 分区。

## Flash 布局(ESP32-C3, 8MB)

    0x00000  bootloader.bin        (0x0,      21024 B)
    0x00800  partition-table.bin   (0x8000,    3072 B)
    0x01000  app.bin               (0x10000, 1524336 B)

中间的空隙一律填 0xFF(擦除态),这正是 esptool merge_bin 的做法 ——
擦除态字节对 bootloader 和分区表都是安全的,不会误触发什么。
"""
from __future__ import annotations

import argparse
from pathlib import Path

# ESP32-C3 8MB 的关键地址,与 partitions.csv 一致
BOOTLOADER_OFFSET = 0x0
PARTITION_OFFSET = 0x8000
APP_OFFSET = 0x10000
FILL = 0xFF


def merge(bootloader: Path, partition: Path, app: Path, out: Path) -> None:
    chunks: list[tuple[int, Path]] = [
        (BOOTLOADER_OFFSET, bootloader),
        (PARTITION_OFFSET, partition),
        (APP_OFFSET, app),
    ]
    for offset, path in chunks:
        if not path.is_file():
            raise SystemExit(f"缺少文件: {path}")

    end = APP_OFFSET + app.stat().st_size
    buffer = bytearray([FILL]) * end

    for offset, path in chunks:
        data = path.read_bytes()
        if offset + len(data) > end:
            raise SystemExit(f"{path.name} 超出预期范围")
        buffer[offset:offset + len(data)] = data
        print(f"  0x{offset:06X}  {path.name:24s} {len(data):>9,} B")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(buffer))
    print(f"\n合并完成: {out}")
    print(f"  大小    : {len(buffer):,} B ({len(buffer)/1024/1024:.2f} MB)")
    print(f"  写偏移  : 0x0   (web flasher 选第一项「合并固件」即可)")

    # 自检:三个区段都应能在镜像里找到自己的起始魔数
    print("\n自检:")
    for offset, path, name in (
        (BOOTLOADER_OFFSET, bootloader, "bootloader 魔数 0xE9"),
        (PARTITION_OFFSET, partition, "分区表魔数 0xAA"),
        (APP_OFFSET, app, "app 魔数 0xE9"),
    ):
        magic = buffer[offset]
        expect = 0xAA if "分区表" in name else 0xE9
        ok = magic == expect
        print(f"  {name:24s} 实际 0x{magic:02X}  {'OK' if ok else '异常!'}")


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 ESP32 合并固件(0x0 起刷)")
    parser.add_argument("--build-dir", type=Path, default=Path("build"),
                        help="ESP-IDF 构建目录(默认 ./build)")
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args()

    build = args.build_dir
    out = args.out or (build / "AI-Passport-MERGED.bin")
    print(f"合并目标: {out}\n")
    merge(
        build / "bootloader" / "bootloader.bin",
        build / "partition_table" / "partition-table.bin",
        build / "Internet-Radio.bin",
        out,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
