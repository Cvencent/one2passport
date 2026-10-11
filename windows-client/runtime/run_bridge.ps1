$ErrorActionPreference = "Stop"
$installDir = Join-Path $env:LOCALAPPDATA "FoloOS\CodexBridge"
$pythonFile = Join-Path $installDir "python-path.txt"
if (-not (Test-Path $pythonFile)) { exit 2 }
$python = (Get-Content $pythonFile -Raw).Trim()
$pidFile = Join-Path $installDir "bridge.pid"
$stdout = Join-Path $installDir "bridge.log"
$stderr = Join-Path $installDir "bridge-error.log"
$process = Start-Process -FilePath $python -ArgumentList @(
    (Join-Path $installDir "windows_bridge.py")
) -WorkingDirectory $installDir -WindowStyle Hidden -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr -PassThru
[System.IO.File]::WriteAllText($pidFile, [string]$process.Id)
$process.WaitForExit()
Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
