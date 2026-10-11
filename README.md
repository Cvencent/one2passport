# one2passport

This repository contains the source for the AI Passport device firmware and the
Windows bridge client used to connect it to Codex.

## Layout

- `firmware/` - ESP-IDF firmware source for the AI Passport device.
- `windows-client/` - Windows tray application, bridge runtime, and setup
  scripts.

## Firmware

The firmware targets ESP32-C3 with 8 MB Flash and is built with ESP-IDF 5.5.3.
See `firmware/CODEX-README.md` for the current feature set, controls, flashing
notes, and pairing details.

The repository stores source only. Build output, managed components, serial
logs, firmware binaries, and local configuration are intentionally excluded.

## Windows Client

The Windows client connects a paired device to the local Codex installation.
See `windows-client/README-Windows.md` for setup and usage, and
`windows-client/CodexBridge-托盘程序说明.md` for the tray application.

The source runtime is under `windows-client/runtime/`. The prebuilt
`CodexBridge.exe` is not committed; build it with PyInstaller from the source
when needed.

## Validation

Firmware builds require an activated ESP-IDF 5.5.3 environment. The host-side
Windows bridge tests can be run from the `windows-client` directory with
Python 3.10 or newer:

```sh
python tests/run_runtime_tests.py
```
