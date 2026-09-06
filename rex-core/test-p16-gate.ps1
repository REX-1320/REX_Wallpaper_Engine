param([string]$ExePath = "D:\REX_doc\rex-core\.build\Release\rex-wallpaper-runtime.exe",
      [string]$VideoPath = "D:\REX_doc\bench-media\mpeg4_3840x2160.mp4")
$ErrorActionPreference = 'Stop'
function Send-Frame($proc, $obj) {
    $json = $obj | ConvertTo-Json -Compress -Depth 6
    $b = [Text.Encoding]::UTF8.GetBytes($json)
    $p = [BitConverter]::GetBytes([uint32]$b.Length)
    $proc.StandardInput.BaseStream.Write($p,0,4); $proc.StandardInput.BaseStream.Write($b,0,$b.Length)
    $proc.StandardInput.BaseStream.Flush()
}
function Read-Frame($proc, $timeoutMs = 15000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $lb = New-Object byte[] 4
        if ($proc.StandardOutput.BaseStream.Read($lb,0,4) -ne 4) { Start-Sleep -Milliseconds 50; continue }
        $len = [BitConverter]::ToUInt32($lb,0)
        $pl = New-Object byte[] $len; $r=0
        while ($r -lt $len) { $n = $proc.StandardOutput.BaseStream.Read($pl,$r,$len-$r); if ($n -le 0){break}; $r+=$n }
        return [Text.Encoding]::UTF8.GetString($pl,0,$r)
    }
    return ''
}
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName=$ExePath; $psi.Arguments="--runtime"; $psi.RedirectStandardInput=$true
$psi.RedirectStandardOutput=$true; $psi.RedirectStandardError=$true; $psi.UseShellExecute=$false
$proc=[System.Diagnostics.Process]::Start($psi)
try {
    Send-Frame $proc @{id=1;cmd='INIT';params=@{maxFps=60;hardwareAcceleration=$true}}
    [void](Read-Frame $proc); $d=[Diagnostics.Stopwatch]::StartNew()
    while ($d.ElapsedMilliseconds -lt 20000) { $f=Read-Frame $proc 3000; if ($f -match 'READY'){break} }
    Send-Frame $proc @{id=2;cmd='LOAD';params=@{path=$VideoPath;type='video';mode='fill';loop=$true}}
    Send-Frame $proc @{id=3;cmd='PLAY';params=@{}}
    Write-Host "playing 4K; warming 12s..."
    Start-Sleep -Seconds 12

    # GPU utilisation samples (3 x 2s apart)
    foreach ($i in 1..3) {
        $c = Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction SilentlyContinue
        $gpu = ($c.CounterSamples | Where-Object { $_.InstanceName -match "pid_$($proc.Id)" } |
                Measure-Object CookedValue -Sum).Sum
        Write-Host ("GPU sample {0}: {1:N1}%" -f $i, $gpu)
        Start-Sleep -Seconds 2
    }

    Send-Frame $proc @{id=4;cmd='GET_STATUS';params=@{}}
    $dl=[Diagnostics.Stopwatch]::StartNew(); $status=''
    while ($dl.ElapsedMilliseconds -lt 15000) {
        $f = Read-Frame $proc 4000
        if ($f -match '"id":4') { $status=$f; break }
    }
    $j = $status | ConvertFrom-Json
    $t = $j.data.telemetry
    Write-Host ("decFps={0:N1} rendFps={1:N1} presentCount={2} dropped={3}" -f `
        $t.decoderFps, $t.renderFps, $t.presentCount, $t.framesDropped)
    Write-Host ("private={0:N1}MB gpuDed={1:N1}MB" -f `
        ($j.data.privateBytes), ($t.gpuDedicated/1MB))
    $proc.Kill(); Wait-Process -Id $proc.Id -ErrorAction SilentlyContinue
    Write-Host "runtime stopped"
} finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}
