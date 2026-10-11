# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller 打包规格:把 Codex 桥接托盘程序打成单文件 EXE。

## 为什么这几个参数是必需的

1. **console=False** —— 双击运行不弹黑窗口。代价是 sys.stdout 变成 None,
   所以 tray_app.py 启动第一件事就是把输出重定向进日志文件。

2. **audio 三个 .ps1 必须显式 datas** —— 语音识别/合成是调 PowerShell 脚本做的,
   PyInstaller 不会自动发现"被字符串拼出来的文件路径"。漏掉的后果很隐蔽:
   EXE 在开发机上能跑(因为脚本还在旁边),拷给别人就语音全废。

3. **hiddenimports 里放 pystray 的后端** —— pystray 用运行时字符串选后端,
   静态分析看不到 `_win32`,不打进去就会报 "no available backend"。

4. **excludes 砍掉用不到的大件** —— 本程序不用 numpy/tkinter 等,
   排掉能省几十 MB。注意不要误排 PIL:pystray 画图标要用它。
"""
from pathlib import Path

runtime = Path(SPECPATH)  # noqa: F821  (SPECPATH 由 PyInstaller 注入)

datas = [
    (str(runtime / "windows_speech_recognize.ps1"), "."),
    (str(runtime / "windows_speech_synthesize.ps1"), "."),
]

hiddenimports = [
    "pystray._win32",
    "PIL.Image",
    "PIL.ImageDraw",
    "PIL._tkinter_finder",
    "winreg",
    "mac_bridge",
    "codex_backend",
    "task_monitor",
    "windows_bridge",
    "windows_codex_backend",
    "pair_device",
]

# 这些都没被引用,排掉能显著减小体积。
# 注意:pystray 的菜单在 _win32 后端下不需要 tkinter,但 PIL 可能会尝试
# 探测 tkinter,故用 _tkinter_finder 占位而不是直接排 PIL。
excludes = [
    "numpy",
    "scipy",
    "pandas",
    "matplotlib",
    "tkinter",
    "test",
    "unittest",
    "pydoc_data",
    "setuptools",
    "pip",
]

a = Analysis(  # noqa: F821
    [str(runtime / "tray_app.py")],
    pathex=[str(runtime)],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=excludes,
    noarchive=False,
    optimize=0,
)

pyz = PYZ(a.pure)  # noqa: F821

exe = EXE(  # noqa: F821
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="CodexBridge",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,          # 关键:GUI 程序,不弹黑窗口
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=str(runtime / "codex_bridge.ico"),
)
