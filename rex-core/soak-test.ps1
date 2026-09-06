# 15-minute soak test for the native wallpaper runtime.
# Plays bench video continuously while sampling GET_STATUS every 30s:
#   privateBytes, gpuDedicated/gpuShared/gpuBudget, dec/rend FPS, drops.
# Writes CSV rows to soak-log.csv and progress to soak-status.txt.
param(
    [string]$ExePath = "D:\REX_doc\rex-core\.build\Release\rex-wallpaper-runtime.exe",
    [string]$VideoPath = "D:\REX_doc\bench-media\cadence_1080p_60s.mp4",
    [int]$DurationSec = 900,
    [int]$SampleEverySec = 30
)
$ErrorActionPreference = 'Stop'
$dir = "D:\REX_doc\rex-core\.build"
$csv = Join-Path $dir 'soak-log.csv'
$st  = Join-Path $dir 'soak-status.txt'

function Set-Status($msg) { Set-Content -Path $st -Value "$(Get-Date -Format HH:mm:ss) $msg" }
function Send-Frame([System.Diagnostics.Process]$proc, [object]$obj) {
    $json = $obj | ConvertTo-Json -Compress -Depth 6
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $len = [BitConverter]::GetBytes([uint32]$bytes.Length)
    $proc.StandardInput.BaseStream.Write($len, 0, 4)
    $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $proc.StandardInput.BaseStream.Flush()
}
function Read-Frame([System.Diagnostics.Process]$proc, [int]$timeoutMs = 15000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $lenBytes = New-Object byte[] 4
        $n = $proc.StandardOutput.BaseStream.Read($lenBytes, 0, 4)
        if ($n -ne 4) { Start-Sleep -Milliseconds 50; continue }
        $len = [BitConverter]::ToUInt32($lenBytes, 0)
        $payload = New-Object byte[] $len
        $read = 0
        while ($read -lt $len) {
            $r = $proc.StandardOutput.BaseStream.Read($payload, $read, $len - $read)
            if ($r -le 0) { break }
            $read += $r
        }
        return ([Text.Encoding]::UTF8.GetString($payload, 0, $read))
    }
    return ''
}

Set-Status "starting"
if (Test-Path $csv) { Remove-Item $csv -Force }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ExePath
$psi.Arguments = "--runtime"
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.WorkingDirectory = Split-Path $ExePath
Write-Output ("DBG exe='$ExePath' exists=$(Test-Path $ExePath) wd='$($psi.WorkingDirectory)'")
$proc = [System.Diagnostics.Process]::Start($psi)

try {
    Send-Frame $proc @{ id = 1; cmd = 'INIT'; params = @{ maxFps = 60; hardwareAcceleration = $true } }
    $resp = Read-Frame $proc
    if ($resp -notmatch '"ok":true') { throw "INIT failed: $resp" }
    # drain READY event
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -lt 20000) {
        $f = Read-Frame $proc 3000
        if ($f -match 'READY') { break }
    }
    Send-Frame $proc @{ id = 2; cmd = 'LOAD'; params = @{ path = $VideoPath; type = 'video'; mode = 'fill'; loop = $true } }
    Send-Frame $proc @{ id = 3; cmd = 'PLAY'; params = @{} }
    Set-Status "playing; soaking for $DurationSec s"

    "time_s,privateMB,gpuDedMB,gpuSharedMB,gpuBudgetGB,decFps,rendFps,presents,dropped,decoded,consumed,decMsP50,decMsP95,vpMsP50,vpMsP95" | Out-File $csv -Append -Encoding utf8
    $id = 10
    $start = [Diagnostics.Stopwatch]::StartNew()
    while ($start.Elapsed.TotalSeconds -lt $DurationSec) {
        Start-Sleep -Seconds $SampleEverySec
        Send-Frame $proc @{ id = $id; cmd = 'GET_STATUS'; params = @{} }
        $id++
        # drain frames until we get the response with our id range
        $statusJson = ''
        $dl = [Diagnostics.Stopwatch]::StartNew()
        while ($dl.ElapsedMilliseconds -lt 10000) {
            $f = Read-Frame $proc 4000
            if ($f -match '"type":"response"' -and $f -match '"telemetry"') { $statusJson = $f; break }
            if (-not $f) { break }
        }
        if (-not $statusJson) { "$(Get-Date -Format HH:mm:ss)" | Out-File $csv -Append -Encoding utf8; continue }
        $j = $statusJson | ConvertFrom-Json
        $t = $j.data.telemetry
        $row = "{0},{1:N1},{2:N1},{3:N1},{4:N2},{5:N1},{6:N1},{7},{8},{9},{10},{11:N2},{12:N2},{13:N2},{14:N2}" -f `
            [int]$start.Elapsed.TotalSeconds,
            $j.data.privateBytes,
            ($t.gpuDedicated / 1MB),
            ($t.gpuShared / 1MB),
            ($t.gpuBudget / 1GB),
            $t.decoderFps,
            $t.renderFps,
            $t.presentCount,
            $t.framesDropped,
            $t.framesDecoded,
            $t.framesConsumed,
            $t.decodeMsP50,
            $t.decodeMsP95,
            $t.vpMsP50,
            $t.vpMsP95
        $row | Out-File $csv -Append -Encoding utf8
        Set-Status ("soak t={0}s private={1:N1}MB gpuDed={2:N1}MB dropped={3}" -f `
            [int]$start.Elapsed.TotalSeconds, $j.data.privateBytes, ($t.gpuDedicated / 1MB), $t.framesDropped)
    }

    Send-Frame $proc @{ id = 999; cmd = 'SHUTDOWN'; params = @{} }
    Read-Frame $proc 10000 | Out-Null
    Set-Status "done"
} finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}
