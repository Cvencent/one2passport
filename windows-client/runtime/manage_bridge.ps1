$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

function Pause-FoloOS {
    Write-Host ""
    Write-Host "按任意键返回菜单……"
    [Console]::ReadKey($true) | Out-Null
}

function Test-Codex {
    $command = Get-Command codex -ErrorAction SilentlyContinue
    if ($null -eq $command) { return $false }
    try {
        & $command.Source --version *> $null
        return ($LASTEXITCODE -eq 0)
    }
    catch { return $false }
}

function Ensure-Codex {
    if (-not (Test-Codex)) {
        Write-Host "没有找到可正常运行的 Codex CLI。" -ForegroundColor Red
        Write-Host "请先按 OpenAI 官方说明安装或修复 Codex，然后重新运行本程序："
        Write-Host "https://learn.chatgpt.com/docs/codex/cli" -ForegroundColor Cyan
        $open = Read-Host "是否现在打开官方安装说明？[Y/n]"
        if ($open -notmatch "^[Nn]") {
            Start-Process "https://learn.chatgpt.com/docs/codex/cli"
        }
        return $false
    }
    & codex login status *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Codex 还没有登录，即将打开登录流程。"
        & codex login
        if ($LASTEXITCODE -ne 0) { return $false }
    }
    return $true
}

function Invoke-Step {
    param([string]$Name)
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $Name)
    return ($LASTEXITCODE -eq 0)
}

function Install-All {
    if (-not (Ensure-Codex)) { return }
    if (-not (Invoke-Step "install_bridge.ps1")) { return }
    $wifi = Read-Host "是否现在通过 USB 配置设备 Wi-Fi？[Y/n]"
    if ($wifi -notmatch "^[Nn]") {
        if (-not (Invoke-Step "windows_wifi_setup.ps1")) { return }
    }
    Invoke-Step "start_bridge.ps1" | Out-Null
    Write-Host "安装完成。以后登录 Windows 会自动启动桥接。" -ForegroundColor Green
}

while ($true) {
    Clear-Host
    Write-Host "FoloOS 编程伴侣安装与管理"
    Write-Host "========================"
    Write-Host "1. 一键安装或修复（推荐）"
    Write-Host "2. 配置或更换设备 Wi-Fi"
    Write-Host "3. 立即启动桥接"
    Write-Host "4. 停止并取消开机启动"
    Write-Host "5. 查看错误日志"
    Write-Host "0. 退出"
    Write-Host ""
    $choice = Read-Host "请选择"
    switch ($choice) {
        "1" { Install-All; Pause-FoloOS }
        "2" { Invoke-Step "windows_wifi_setup.ps1" | Out-Null; Pause-FoloOS }
        "3" { Invoke-Step "start_bridge.ps1" | Out-Null; Pause-FoloOS }
        "4" { Invoke-Step "uninstall_bridge.ps1" | Out-Null; Pause-FoloOS }
        "5" {
            $log = Join-Path $env:LOCALAPPDATA "FoloOS\CodexBridge\bridge-error.log"
            Write-Host "错误日志：$log"
            if (Test-Path $log) { Get-Content $log -Tail 60 }
            else { Write-Host "尚未生成错误日志。" }
            Pause-FoloOS
        }
        "0" { exit 0 }
        default { Write-Host "请输入 0 到 5。"; Pause-FoloOS }
    }
}
