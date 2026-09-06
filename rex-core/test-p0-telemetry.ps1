# P0 telemetry smoke test: drives rex-wallpaper-cli.exe --runtime directly.
# Sends INIT -> LOAD -> PLAY -> GET_STATUS over the length-prefixed JSON
# protocol, waits >10s so the 5s TelemetryThread writes >=2 samples, then
# SHUTDOWN. Verifies runtime-telemetry.log lines + GET_STATUS new fields.
param(
    [string]$ExePath = "D:\REX_doc\rex-core\.build\Release\rex-wallpaper-cli.exe",
    [string]$VideoPath = "",
    [switch]$ForceSdr
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $ExePath)) { throw "exe not found: $ExePath" }
if (-not $VideoPath) {
    $cand = Get-ChildItem "D:\REX_doc\bench-media" -Recurse -Include *.mp4,*.mkv,*.webm -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cand) { $VideoPath = $cand.FullName }
}
if (-not $VideoPath -or -not (Test-Path $VideoPath)) { throw "no test video found" }
Write-Host "video: $VideoPath"

$telLog = Join-Path $env:TEMP 'REX\runtime-telemetry.log'
if (Test-Path $telLog) { Remove-Item $telLog -Force }

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
        return [Text.Encoding]::UTF8.GetString($payload, 0, $read)
    }
    throw "timeout waiting for frame"
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ExePath
$runtimeArgs = "--runtime"
if ($ForceSdr) { $runtimeArgs += " --force-sdr" }
$psi.Arguments = $runtimeArgs
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.WorkingDirectory = Split-Path $ExePath

$proc = [System.Diagnostics.Process]::Start($psi)
try {
    Send-Frame $proc @{ id = 1; cmd = 'INIT'; params = @{ maxFps = 60; hardwareAcceleration = $true } }
    $initResp = Read-Frame $proc
    Write-Host "<< $initResp"
    if ($initResp -notmatch '"ok":true') { throw "INIT failed: $initResp" }
    # Runtime emits READY as a separate event frame after the response.
    $readyDeadline = [Diagnostics.Stopwatch]::StartNew()
    $ready = ''
    while ($readyDeadline.ElapsedMilliseconds -lt 20000) {
        try { $ready = Read-Frame $proc 3000 } catch { continue }
        if ($ready -match 'READY|ERROR') { break }
    }
    Write-Host "<< $ready"
    if ($ready -notmatch '"READY"') { throw "no READY event: $ready" }

    Send-Frame $proc @{ id = 2; cmd = 'LOAD'; params = @{ path = $VideoPath; type = 'video'; mode = 'fill'; loop = $true } }
    $loaded = Read-Frame $proc
    Write-Host "<< $loaded"

    Send-Frame $proc @{ id = 3; cmd = 'PLAY'; params = @{} }
    $playing = Read-Frame $proc
    Write-Host "<< $playing"

    # TelemetryThread samples every 5s — wait ~12s for >=2 lines.
    Start-Sleep -Seconds 12

    Send-Frame $proc @{ id = 4; cmd = 'GET_STATUS'; params = @{} }
    # Drain queued events (PLAYING etc.) until the id:4 response arrives.
    $status = ''
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -lt 15000) {
        try { $frame = Read-Frame $proc 5000 } catch { break }
        Write-Host "<< $frame"
        if ($frame -match '"id":4') { $status = $frame; break }
    }
    if (-not $status) { throw "never received GET_STATUS response" }

    foreach ($field in @('gpuDedicated','gpuShared','gpuBudget','decRendSameAdapter','rendDispSameAdapter','decoderLuid','rendererLuid')) {
        if ($status -match [regex]::Escape("`"$field`"")) { Write-Host "OK  status has $field" }
        else { Write-Warning "MISSING status field: $field" }
    }

    Send-Frame $proc @{ id = 5; cmd = 'SHUTDOWN'; params = @{} }
    $bye = Read-Frame $proc
    Write-Host "<< $bye"
} finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}

Write-Host "`n=== runtime-telemetry.log ==="
if (Test-Path $telLog) {
    Get-Content $telLog
    $lines = (Get-Content $telLog).Count
    if ($lines -ge 2) { Write-Host "OK  $lines telemetry lines written" }
    else { Write-Warning "only $lines telemetry line(s)" }
} else {
    Write-Warning "telemetry log NOT created at $telLog"
}
