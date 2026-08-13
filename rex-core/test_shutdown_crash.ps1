# Test script to reproduce Bug #2: fail-fast during D3D12 renderer teardown
# Sends INIT -> LOAD (video) -> PLAY -> SHUTDOWN via the runtime protocol

param(
    [string]$VideoPath = "C:\Users\munna\Videos\Screen Recordings\Screen Recording 2026-07-24 225314.mp4",
    [int]$Runs = 5,
    [switch]$InitOnly
)

$exe = "D:\REX_doc\rex-core\build\Debug\rex-wallpaper-runtime.exe"
$tempDir = Join-Path $env:TEMP "REX"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
$crashLog = Join-Path $tempDir "crash.log"
$runtimeLog = Join-Path $tempDir "runtime.log"

# Clear old logs
if (Test-Path $crashLog) { Remove-Item $crashLog -Force }
if (Test-Path $runtimeLog) { Remove-Item $runtimeLog -Force }

function Send-Frame($proc, $json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $len = [BitConverter]::GetBytes([uint32]$bytes.Length)
    $proc.StandardInput.BaseStream.Write($len, 0, 4)
    $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $proc.StandardInput.BaseStream.Flush()
}

function Read-Frame($proc) {
    $lenBytes = New-Object byte[] 4
    $read = $proc.StandardOutput.BaseStream.Read($lenBytes, 0, 4)
    if ($read -ne 4) { return $null }
    $len = [BitConverter]::ToUInt32($lenBytes, 0)
    if ($len -eq 0 -or $len -gt 16MB) { return $null }
    $data = New-Object byte[] $len
    $total = 0
    while ($total -lt $len) {
        $r = $proc.StandardOutput.BaseStream.Read($data, $total, $len - $total)
        if ($r -eq 0) { return $null }
        $total += $r
    }
    return [System.Text.Encoding]::UTF8.GetString($data)
}

# Read frames until we get a response with the given id
function Wait-ForResponse($proc, $id, $timeoutMs = 10000) {
    $deadline = [DateTime]::Now.AddMilliseconds($timeoutMs)
    while ([DateTime]::Now -lt $deadline) {
        $frame = Read-Frame $proc
        if ($null -eq $frame) { return $null }
        $json = $frame | ConvertFrom-Json
        if ($json.type -eq "response" -and $json.id -eq $id) {
            return $json
        }
        # Skip events
    }
    return $null
}

$results = @()
for ($run = 1; $run -le $Runs; $run++) {
    Write-Host "=== Run $run/$Runs ===" -ForegroundColor Cyan
    
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = "--runtime"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    
    $proc = [System.Diagnostics.Process]::Start($psi)
    
    try {
        # INIT
        Send-Frame $proc '{"id":1,"cmd":"INIT","params":{"maxFps":60,"hardwareAcceleration":true}}'
        $resp = Wait-ForResponse $proc 1
        if ($null -eq $resp) { Write-Host "  INIT TIMEOUT" -ForegroundColor Red; $results += "INIT_TIMEOUT"; continue }
        Write-Host "  INIT: ok=$($resp.ok)"
        
        if (-not $InitOnly) {
            # LOAD video
            $loadJson = '{"id":2,"cmd":"LOAD","params":{"path":"' + $VideoPath.Replace('\','/') + '","type":"video","loop":true,"muted":true,"volume":1.0,"speed":1.0,"mode":"fill"}}'
            Send-Frame $proc $loadJson
            $resp = Wait-ForResponse $proc 2
            if ($null -eq $resp) { Write-Host "  LOAD TIMEOUT" -ForegroundColor Red; $results += "LOAD_TIMEOUT"; continue }
            Write-Host "  LOAD: ok=$($resp.ok)"
            
            # PLAY
            Send-Frame $proc '{"id":3,"cmd":"PLAY","params":{}}'
            $resp = Wait-ForResponse $proc 3
            if ($null -eq $resp) { Write-Host "  PLAY TIMEOUT" -ForegroundColor Red; $results += "PLAY_TIMEOUT"; continue }
            Write-Host "  PLAY: ok=$($resp.ok)"
            
            # Let it run briefly to start decoding
            Start-Sleep -Milliseconds 1000
        }
        
        # SHUTDOWN
        Send-Frame $proc '{"id":99,"cmd":"SHUTDOWN","params":{}}'
        $resp = Wait-ForResponse $proc 99
        if ($null -eq $resp) { Write-Host "  SHUTDOWN TIMEOUT" -ForegroundColor Yellow }
        else { Write-Host "  SHUTDOWN: ok=$($resp.ok)" }
        
        # Wait for exit
        if (-not $proc.WaitForExit(10000)) {
            Write-Host "  TIMEOUT waiting for exit" -ForegroundColor Yellow
            $proc.Kill()
            $results += "TIMEOUT"
        } else {
            $code = $proc.ExitCode
            Write-Host "  Exit code: $code" -ForegroundColor $(if ($code -eq 0) { "Green" } else { "Red" })
            $results += "exit=$code"
        }
    } catch {
        Write-Host "  ERROR: $_" -ForegroundColor Red
        $results += "ERROR"
    } finally {
        if (-not $proc.HasExited) {
            $proc.Kill()
        }
        $proc.Dispose()
    }
    
    # Check crash.log
    if (Test-Path $crashLog) {
        $content = Get-Content $crashLog -Raw
        if ($content -match "TERMINATE|ABORT|UNHANDLED|VEH") {
            Write-Host "  CRASH LOG DETECTED!" -ForegroundColor Red
            # Show the last 40 lines
            $content -split "`n" | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
        }
    }
}

Write-Host ""
Write-Host "=== Results ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }