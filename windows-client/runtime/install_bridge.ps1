$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try {
    $runtimeSource = $PSScriptRoot
    $installDir = Join-Path $env:LOCALAPPDATA "FoloOS\CodexBridge"
    New-Item -ItemType Directory -Force -Path $installDir | Out-Null

    $python = $null
    if (Get-Command py.exe -ErrorAction SilentlyContinue) {
        $python = (& py.exe -3 -c "import sys; print(sys.executable)").Trim()
    }
    elseif (Get-Command python.exe -ErrorAction SilentlyContinue) {
        $python = (& python.exe -c "import sys; print(sys.executable)").Trim()
    }
    if ([string]::IsNullOrWhiteSpace($python) -or -not (Test-Path $python)) {
        throw "未找到 Python 3。请先从 python.org 安装 Python 3.11 或更高版本。"
    }
    & $python -c "import sys; assert sys.version_info >= (3, 10)"
    if ($LASTEXITCODE -ne 0) { throw "Python 版本过低，需要 3.10 或更高版本。" }

    $codex = Get-Command codex -ErrorAction SilentlyContinue
    if ($null -eq $codex) {
        throw "未找到 Codex CLI。请返回安装与管理程序，按提示安装 Codex。"
    }
    & $codex.Source --version *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "Codex CLI 已损坏或安装不完整。请返回安装与管理程序，按提示修复。"
    }

    $files = @(
        "mac_bridge.py", "codex_backend.py", "task_monitor.py", "windows_codex_backend.py",
        "windows_bridge.py", "windows_speech_recognize.ps1",
        "windows_speech_synthesize.ps1", "windows_wifi_setup.ps1",
        "run_bridge.ps1", "start_bridge.ps1"
    )
    foreach ($file in $files) {
        Copy-Item (Join-Path $runtimeSource $file) (Join-Path $installDir $file) -Force
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $installDir "python-path.txt"),
        $python,
        [System.Text.UTF8Encoding]::new($false)
    )

    $startup = [Environment]::GetFolderPath("Startup")
    $shortcutPath = Join-Path $startup "FoloOS Codex Bridge.lnk"
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = "powershell.exe"
    $shortcut.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $installDir "run_bridge.ps1") + '"'
    $shortcut.WorkingDirectory = $installDir
    $shortcut.Save()

    Write-Host "安装成功：$installDir" -ForegroundColor Green
    Write-Host "后台桥接已加入 Windows 登录启动项。"
}
catch {
    Write-Host "安装失败：$($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
