#!/usr/bin/env python3
"""Build a checked, sector-aligned LEO RADIO video resource image."""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


MAGIC = b"LEOVID10"
HEADER_FORMAT = "<8sHHHH11I4s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
JFIF_APP0 = bytes.fromhex("FFE000104A46494600010100000100010000")


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def make_header(
    *,
    width: int,
    height: int,
    fps_milli: int,
    duration_ms: int,
    frame_count: int,
    index_offset: int,
    frame_offset: int,
    audio_offset: int,
    audio_size: int,
    total_size: int,
    max_frame_size: int,
    payload_crc: int,
    header_crc: int,
) -> bytes:
    return struct.pack(
        HEADER_FORMAT,
        MAGIC,
        1,
        HEADER_SIZE,
        width,
        height,
        fps_milli,
        duration_ms,
        frame_count,
        index_offset,
        frame_offset,
        audio_offset,
        audio_size,
        total_size,
        max_frame_size,
        payload_crc,
        header_crc,
        b"\0" * 4,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--fps", type=float, required=True)
    parser.add_argument("--duration-ms", type=int, required=True)
    parser.add_argument("--limit", type=lambda value: int(value, 0), required=True)
    args = parser.parse_args()

    if not 1 <= args.fps <= 20:
        raise SystemExit("FPS must be between 1 and 20")
    if not 1 <= args.width <= 240 or not 1 <= args.height <= 280:
        raise SystemExit("Frame dimensions exceed the firmware limits")

    frame_paths = sorted(args.frames.glob("*.jpg"))
    if not frame_paths:
        raise SystemExit("No JPEG frames found")

    frames: list[bytes] = []
    for path in frame_paths:
        data = path.read_bytes()
        if not data.startswith(b"\xff\xd8"):
            raise SystemExit(f"Not a JPEG: {path}")
        # LVGL's tiny JPEG decoder recognizes in-memory JPEGs by their JFIF
        # APP0 marker. FFmpeg can emit a COM marker first, so add JFIF without
        # re-encoding when needed.
        if not data.startswith(b"\xff\xd8\xff\xe0\x00\x10JFIF"):
            data = data[:2] + JFIF_APP0 + data[2:]
        frames.append(data)

    audio = args.audio.read_bytes()
    index_offset = HEADER_SIZE
    frame_offset = align(index_offset + len(frames) * 8, 16)
    frame_blob = bytearray()
    index_blob = bytearray()
    for frame in frames:
        index_blob += struct.pack("<II", len(frame_blob), len(frame))
        frame_blob += frame
    audio_offset = align(frame_offset + len(frame_blob), 16)
    total_size = audio_offset + len(audio)
    if total_size > args.limit:
        raise SystemExit(
            f"Resource is {total_size} bytes, exceeds safe limit {args.limit}"
        )

    payload = bytearray()
    payload += index_blob
    payload += b"\xff" * (frame_offset - HEADER_SIZE - len(payload))
    payload += frame_blob
    payload += b"\xff" * (audio_offset - HEADER_SIZE - len(payload))
    payload += audio
    if len(payload) != total_size - HEADER_SIZE:
        raise AssertionError("Payload layout mismatch")

    payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
    values = dict(
        width=args.width,
        height=args.height,
        fps_milli=round(args.fps * 1000),
        duration_ms=args.duration_ms,
        frame_count=len(frames),
        index_offset=index_offset,
        frame_offset=frame_offset,
        audio_offset=audio_offset,
        audio_size=len(audio),
        total_size=total_size,
        max_frame_size=max(map(len, frames)),
        payload_crc=payload_crc,
    )
    header = make_header(**values, header_crc=0)
    header = make_header(**values, header_crc=zlib.crc32(header) & 0xFFFFFFFF)

    image = header + payload
    image += b"\xff" * (align(len(image), 4096) - len(image))
    args.output.write_bytes(image)
    print(f"frames={len(frames)} max_frame={max(map(len, frames))}")
    print(f"audio={len(audio)} payload_crc=0x{payload_crc:08x}")
    print(
        f"logical_size={total_size} image_size={len(image)} limit={args.limit}"
    )


if __name__ == "__main__":
    main()
