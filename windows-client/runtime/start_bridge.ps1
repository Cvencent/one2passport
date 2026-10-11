$ErrorActionPreference = "Stop"
$installDir = Join-Path $env:LOCALAPPDATA "FoloOS\CodexBridge"
$runner = Join-Path $installDir "run_bridge.ps1"
if (-not (Test-Path $runner)) {
    Write-Host "尚未安装，请先双击 1-安装Codex桥接.bat" -ForegroundColor Red
    exit 1
}
$pidFile = Join-Path $installDir "bridge.pid"
if (Test-Path $pidFile) {
    $oldPid = [int](Get-Content $pidFile -Raw)
    if (Get-Process -Id $oldPid -ErrorAction SilentlyContinue) {
        Write-Host "FoloOS Codex 桥接已经在后台运行。"
        exit 0
    }
}
Start-Process powershell.exe -ArgumentList @(
    "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden",
    "-File", ('"' + $runner + '"')
) -WindowStyle Hidden
Start-Sleep -Seconds 2
Write-Host "FoloOS Codex 桥接已在后台启动。"
Write-Host "日志：$installDir\bridge.log"
