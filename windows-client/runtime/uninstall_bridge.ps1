$ErrorActionPreference = "SilentlyContinue"
$installDir = Join-Path $env:LOCALAPPDATA "FoloOS\CodexBridge"
$pidFile = Join-Path $installDir "bridge.pid"
if (Test-Path $pidFile) {
    $bridgePid = [int](Get-Content $pidFile -Raw)
    $process = Get-Process -Id $bridgePid -ErrorAction SilentlyContinue
    if ($null -ne $process -and $process.ProcessName -match "python") {
        Stop-Process -Id $bridgePid
    }
}
Remove-Item (Join-Path ([Environment]::GetFolderPath("Startup")) "FoloOS Codex Bridge.lnk") -Force
Write-Host "已停止桥接并取消开机启动。"
Write-Host "设备 Wi-Fi 配置和单词熊数据仍保留在 %LOCALAPPDATA%\FoloOS。"
