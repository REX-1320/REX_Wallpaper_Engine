$ErrorActionPreference = 'Stop'

$runtime = 'D:\REX_doc\rex-core\.build\Release\rex-wallpaper-runtime.exe'
$video = 'D:\REX_doc\bench-media\cadence_1080p_60s.mp4'
$logDir = 'D:\REX_doc\bench-media\logs\test-d'
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
$proc.StandardOutput.ReadToEndAsync() | Out-Null
$proc.StandardError.ReadToEndAsync() | Out-Null

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

Send-Cmd 'INIT' @{ fps = 60; applyToDesktop = $true; pauseOnFullscreen = $false }
Start-Sleep -Milliseconds 500
Send-Cmd 'LOAD' @{ path = $video; type = 'video'; mode = 'fill'; loop = $true; muted = $true; volume = 1; speed = 1 }
Start-Sleep -Milliseconds 500
Send-Cmd 'PLAY' @{}
Write-Output 'runtime: INIT+LOAD+PLAY sent'
Start-Sleep -Seconds 8
Write-Output ("runtime alive: {0}, ws MB: {1}" -f (-not $proc.HasExited), [math]::Round($proc.WorkingSet64 / 1MB, 0))
$hold = [int](Get-Content Env:REX_D_HOLD -ErrorAction SilentlyContinue)
if ($hold -gt 0) {
    Write-Output "holding stdin open for ${hold}s"
    Start-Sleep -Seconds $hold
}
Write-Output 'test-d done'