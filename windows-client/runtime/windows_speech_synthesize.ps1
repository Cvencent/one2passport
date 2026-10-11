param(
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$Text,
    [string]$Locale = "zh-CN"
)

$ErrorActionPreference = "Stop"
try {
    Add-Type -AssemblyName System.Speech
    $synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $voice = $synth.GetInstalledVoices() |
        Where-Object { $_.Enabled -and $_.VoiceInfo.Culture.Name -eq $Locale } |
        Select-Object -First 1
    if ($null -ne $voice) {
        $synth.SelectVoice($voice.VoiceInfo.Name)
    }
    $format = [System.Speech.AudioFormat.SpeechAudioFormatInfo]::new(
        16000,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono
    )
    $synth.SetOutputToWaveFile($OutputPath, $format)
    $synth.Speak($Text)
    $synth.Dispose()
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
