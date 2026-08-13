# Run the runtime under cdbX64 to catch the fail-fast 0xC0000409 crash
# Uses -logo to avoid the pipe hang issue

param(
    [string]$VideoPath = "C:\Users\munna\Videos\Screen Recordings\Screen Recording 2026-07-24 225314.mp4",
    [int]$Runs = 5
)

$exe = "D:\REX_doc\rex-core\build\Debug\rex-wallpaper-runtime.exe"
$tempDir = Join-Path $env:TEMP "REX"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
$cdbLog = Join-Path $tempDir "cdb_output.log"
$crashLog = Join-Path $tempDir "crash.log"

# Clear old logs
if (Test-Path $cdbLog) { Remove-Item $cdbLog -Force }
if (Test-Path $crashLog) { Remove-Item $crashLog -Force }

# Create a cdb command script
$cdbScript = Join-Path $tempDir "cdb_commands.txt"
@"
sxe c0000409
sxe av
sxe eh
.logopen $($cdbLog.Replace('\','/'))
g
"@ | Set-Content $cdbScript -Encoding ASCII

$results = @()
for ($run = 1; $run -le $Runs; $run++) {
    Write-Host "=== Run $run/$Runs ===" -ForegroundColor Cyan
    
    # Start cdb with the runtime
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "cdbX64"
    $psi.Arguments = "-logo `"$cdbLog`" -c `"sxe c0000409; sxe av; sxe eh; g`" `"$exe`" --runtime"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    
    $proc = [System.Diagnostics.Process]::Start($psi)
    
    try {
        # Wait for cdb to start the process
        Start-Sleep -Milliseconds 500
        
        # Send INIT
        $initJson = '{"id":1,"cmd":"INIT","params":{"maxFps":60,"hardwareAcceleration":true}}'
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($initJson)
        $len = [BitConverter]::GetBytes([uint32]$bytes.Length)
        $proc.StandardInput.BaseStream.Write($len, 0, 4)
        $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        $proc.StandardInput.BaseStream.Flush()
        Start-Sleep -Milliseconds 500
        
        # Send LOAD
        $loadJson = '{"id":2,"cmd":"LOAD","params":{"path":"' + $VideoPath.Replace('\','/') + '","type":"video","loop":true,"muted":true,"volume":1.0,"speed":1.0,"mode":"fill"}}'
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($loadJson)
        $len = [BitConverter]::GetBytes([uint32]$bytes.Length)
        $proc.StandardInput.BaseStream.Write($len, 0, 4)
        $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        $proc.StandardInput.BaseStream.Flush()
        Start-Sleep -Milliseconds 500
        
        # Send PLAY
        $playJson = '{"id":3,"cmd":"PLAY","params":{}}'
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($playJson)
        $len = [BitConverter]::GetBytes([uint32]$bytes.Length)
        $proc.StandardInput.BaseStream.Write($len, 0, 4)
        $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        $proc.StandardInput.BaseStream.Flush()
        
        # Let it run briefly
        Start-Sleep -Milliseconds 1000
        
        # Send SHUTDOWN
        $shutdownJson = '{"id":99,"cmd":"SHUTDOWN","params":{}}'
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($shutdownJson)
        $len = [BitConverter]::GetBytes([uint32]$bytes.Length)
        $proc.StandardInput.BaseStream.Write($len, 0, 4)
        $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        $proc.StandardInput.BaseStream.Flush()
        
        # Wait for exit
        if (-not $proc.WaitForExit(15000)) {
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
    
    # Check if cdb caught a crash
    if (Test-Path $cdbLog) {
        $content = Get-Content $cdbLog -Raw
        if ($content -match "c0000409|Access violation|Breakpoint") {
            Write-Host "  CDB CAUGHT CRASH!" -ForegroundColor Red
            # Show the crash analysis
            $content -split "`n" | Select-Object -Last 50 | ForEach-Object { Write-Host "    $_" }
            break
        }
    }
}

Write-Host ""
Write-Host "=== Results ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }