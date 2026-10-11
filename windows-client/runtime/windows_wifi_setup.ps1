param([string]$Port = "")

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

function Select-FoloPort {
    param([string]$Requested)
    if (-not [string]::IsNullOrWhiteSpace($Requested)) { return $Requested }
    $ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    if ($ports.Count -eq 0) { throw "没有检测到 USB 串口，请插好设备并关闭 Chrome 刷机页。" }
    if ($ports.Count -eq 1) { return $ports[0] }
    Write-Host "检测到多个串口："
    for ($i = 0; $i -lt $ports.Count; $i++) { Write-Host "  $($i + 1). $($ports[$i])" }
    $choice = [int](Read-Host "请输入设备串口序号")
    if ($choice -lt 1 -or $choice -gt $ports.Count) { throw "串口序号无效。" }
    return $ports[$choice - 1]
}

function Save-BridgeConfig {
    param([string]$HostAddress, [string]$Token)
    $dir = Join-Path $env:LOCALAPPDATA "FoloOS"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $path = Join-Path $dir "bridge.json"
    $json = @{ host = $HostAddress; port = 8765; token = $Token } |
        ConvertTo-Json -Compress
    [System.IO.File]::WriteAllText(
        $path,
        $json,
        [System.Text.UTF8Encoding]::new($false)
    )
}

$serial = $null
try {
    $selectedPort = Select-FoloPort $Port
    $ssid = (Read-Host "请输入要连接的 2.4GHz Wi-Fi 名称").Trim()
    if ([string]::IsNullOrWhiteSpace($ssid)) { throw "Wi-Fi 名称不能为空。" }
    $secure = Read-Host "请输入 Wi-Fi 密码（开放网络直接回车）" -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
    $bytes = New-Object byte[] 16
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $token = ($bytes | ForEach-Object { $_.ToString("x2") }) -join ""

    $serial = [System.IO.Ports.SerialPort]::new($selectedPort, 115200)
    $serial.Encoding = [System.Text.UTF8Encoding]::new($false)
    $serial.NewLine = "`n"
    $serial.ReadTimeout = 250
    $serial.WriteTimeout = 2000
    $serial.DtrEnable = $false
    $serial.RtsEnable = $false
    $serial.Open()
    Start-Sleep -Milliseconds 500
    $serial.DiscardInBuffer()
    $payload = @{
        type = "wifi_setup"
        ssid = $ssid
        password = $password
        token = $token
    } | ConvertTo-Json -Compress
    $serial.WriteLine($payload)
    Write-Host "已发送配网信息，正在等待设备连接……"

    $saved = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(35)
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $line = $serial.ReadLine() }
        catch [System.TimeoutException] { continue }
        if (-not $line.TrimStart().StartsWith("{")) { continue }
        try { $event = $line | ConvertFrom-Json }
        catch { continue }
        if ($event.type -ne "wifi_status") { continue }
        if ($event.status -eq "error") { throw "设备拒绝配网参数：$($event.text)" }
        if ($event.status -eq "saved") {
            $saved = $true
            Save-BridgeConfig "" $token
            Write-Host "设备已保存 Wi-Fi，正在连接……"
        }
        if ($event.status -eq "connected") {
            Save-BridgeConfig ([string]$event.ip) $token
            Write-Host "配置成功：设备地址 $($event.ip)"
            Write-Host "现在可以拔掉 USB，并从安装与管理程序启动桥接。"
            exit 0
        }
    }
    if ($saved) { throw "设备已保存配置但尚未连上，请检查密码和 2.4GHz 网络后重启设备。" }
    throw "设备没有回应配网。请关闭 Chrome 刷机页、重启设备后重试。"
}
catch {
    Write-Host "配置失败：$($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    if ($null -ne $serial -and $serial.IsOpen) { $serial.Close() }
}
