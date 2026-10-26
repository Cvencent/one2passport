#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""ESP32-C3 (COM4) serial harness.

Two jobs, because the earlier ad-hoc scripts were flaky:
  * capture: stream device output to a log file for N seconds;
  * script:  run a timed sequence of (delay, chars) writes against that same
    open port, so key injection lands at a known offset instead of "whenever
    the previous command finished".

Auto-blank used to eat the first injected key, so `wake_pad` is prepended
unless the caller says the screen is already lit.
"""
import argparse
import sys
import time

import serial


def open_port(port, baud):
    # 同 fap_device.py: 预置 DTR/RTS=False 再 open, 避免 USB-JTAG 复位设备。
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.write_timeout = 2.0
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    return ser


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--log", default="serial_probe.log")
    ap.add_argument("--step", default="",
                    help="semicolon list of delay:chars, e.g. '2:o;3:d;1:l'")
    ap.add_argument("--chars-delay", type=float, default=0.25)
    ap.add_argument("--wake-pad", type=int, default=2,
                    help="pre-click this many times (blank-screen guard)")
    ap.add_argument("--no-wake", action="store_true")
    args = ap.parse_args()

    steps = []
    if args.step:
        for raw in args.step.split(";"):
            raw = raw.strip()
            if not raw:
                continue
            delay, _, chars = raw.partition(":")
            steps.append((float(delay), chars))

    ser = open_port(args.port, args.baud)
    log = open(args.log, "wb")
    start = time.time()

    def pump(until):
        # Drain the port until `until`; keeps the device from blocking on a
        # full UART FIFO while we sit in a sleep.
        while time.time() < until:
            chunk = ser.read(4096)
            if chunk:
                log.write(chunk)
                log.flush()
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()

    try:
        if not args.no_wake:
            pump(time.time() + 1.5)
            for _ in range(max(0, args.wake_pad)):
                ser.write(b"o")
                time.sleep(0.15)
            pump(time.time() + 0.5)

        for delay, chars in steps:
            pump(time.time() + delay)
            for ch in chars:
                ser.write(ch.encode())
                time.sleep(args.chars_delay)
            sys.stderr.write("\n[t=%6.1fs] sent %r\n" % (time.time() - start, chars))

        pump(time.time() + args.seconds)
    finally:
        log.close()
        ser.close()
    sys.stderr.write("\n[captured %.1fs -> %s]\n" % (time.time() - start, args.log))


if __name__ == "__main__":
    main()