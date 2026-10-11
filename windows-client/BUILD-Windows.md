# Building the Windows client

The tray client is a Python application under `runtime/`. It can be run
directly or packaged as a single executable with PyInstaller.

## Run from source

Install Python 3.10 or newer, then install the runtime dependencies:

```powershell
python -m pip install -r runtime/requirements.txt
python runtime/tray_app.py
```

The command-line bridge can be run separately:

```powershell
python runtime/pair_device.py
python runtime/windows_bridge.py
```

## Build CodexBridge.exe

From this directory:

```powershell
python -m pip install pyinstaller
Set-Location runtime
python -m PyInstaller --noconfirm --clean CodexBridge.spec
```

The output is `runtime/dist/CodexBridge.exe`. The executable is generated
locally and is not stored in this repository.
