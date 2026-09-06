$ErrorActionPreference = 'Stop'

$runtime = 'D:\REX_doc\rex-core\.build\Release\rex-wallpaper-runtime.exe'
$video60 = 'D:\REX_doc\bench-media\cadence_1080p_60s.mp4'
$video4s = 'D:\REX_doc\bench-media\mpeg4_1920x1080.mp4'
$logDir = 'D:\REX_doc\bench-media\logs\cadence-test'
$outFile = 'D:\REX_doc\bench-media\logs\cadence-test\stdout.bin'
$errFile = 'D:\REX_doc\bench-media\logs\cadence-test\stderr.txt'

Remove-Item -Recurse -Force $logDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $logDir -Force | Out-Null

$env:REX_LOG_DIR = $logDir

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $runtime
$psi.Arguments = '--runtime'
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::Start($psi)

function Send-Cmd([string]$cmd, [hashtable]$params) {
    $obj = @{ cmd = $cmd; id = 0 }
    if ($params) { $obj['params'] = $params }
    $json = $obj | ConvertTo-Json -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $len = [BitConverter]::GetBytes([uint32]$bytes.Length)
    $proc.StandardInput.BaseStream.Write($len, 0, 4)
    $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $proc.StandardInput.BaseStream.Flush()
}

$proc.StandardOutput.ReadToEndAsync() | Out-Null
$proc.StandardError.ReadToEndAsync() | Out-Null

$t0 = Get-Date
Write-Output "[test] INIT"
Send-Cmd 'INIT' @{ maxFps = 60; hardwareAcceleration = $true; applyToDesktop = $false; pauseOnFullscreen = $false }
Start-Sleep -Milliseconds 1500

Write-Output "[test] LOAD 60s clip"
Send-Cmd 'LOAD' @{ path = $video60; type = 'video'; loop = $true; muted = $true }
Start-Sleep -Milliseconds 1000

Write-Output "[test] PLAY"
Send-Cmd 'PLAY' @{}
Start-Sleep -Seconds 62

Write-Output "[test] PAUSE"
Send-Cmd 'PAUSE' @{}
Start-Sleep -Seconds 3

Write-Output "[test] PLAY (resume)"
Send-Cmd 'PLAY' @{}
Start-Sleep -Seconds 6

Write-Output "[test] 10 rapid swaps (60s clip <-> 4s clip)"
for ($i = 0; $i -lt 10; $i++) {
    $v = if ($i % 2 -eq 0) { $video4s } else { $video60 }
    Send-Cmd 'LOAD' @{ path = $v; type = 'video'; loop = $true; muted = $true }
    Send-Cmd 'PLAY' @{}
    Start-Sleep -Milliseconds 900
}

Start-Sleep -Seconds 8

Write-Output "[test] SHUTDOWN"
Send-Cmd 'SHUTDOWN' @{}
$proc.WaitForExit(15000) | Out-Null
if (-not $proc.HasExited) { $proc.Kill() }
$proc.StandardInput.Close()

$elapsed = (Get-Date) - $t0
Write-Output "[test] done in $($elapsed.TotalSeconds.ToString('F1'))s, exit=$($proc.ExitCode)"

$log = Get-Content (Join-Path $logDir 'runtime.log') -ErrorAction SilentlyContinue
Write-Output "--- log lines: $($log.Count) ---"
Write-Output '=== playback stats ==='
$log | Select-String -Pattern 'Playback stats|Video playback stats|frames decoded|Dropped|decoder FPS|Render FPS|swap' | Select-Object -Last 40 | ForEach-Object { $_.Line }
Write-Output '=== stalls / warnings ==='
$log | Select-String -Pattern 'stall|stalled|WARN|\[probe\]|TEMP-' | Select-Object -Last 30 | ForEach-Object { $_.Line }
