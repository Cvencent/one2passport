param(
    [Parameter(Mandatory = $true)][string]$WavPath,
    [string]$Locale = "zh-CN"
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try {
    Add-Type -AssemblyName System.Speech
    $culture = [System.Globalization.CultureInfo]::GetCultureInfo($Locale)
    $recognizer = [System.Speech.Recognition.SpeechRecognitionEngine]::new($culture)
    $recognizer.LoadGrammar([System.Speech.Recognition.DictationGrammar]::new())
    $recognizer.SetInputToWaveFile($WavPath)
    $result = $recognizer.Recognize()
    if ($null -eq $result -or [string]::IsNullOrWhiteSpace($result.Text)) {
        throw "未识别到文字，请靠近设备后重新录音"
    }
    Write-Output $result.Text
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
