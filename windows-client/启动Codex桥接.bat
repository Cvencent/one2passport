@echo off
chcp 65001 >nul
rem 启动 Codex 桥接托盘程序。若已在运行,程序自身会提示,不会重复启动。
start "" "%~dp0CodexBridge.exe"
exit
