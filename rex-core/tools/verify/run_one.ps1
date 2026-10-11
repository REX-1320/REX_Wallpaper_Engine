param([string]$Exe, [string]$Media, [string]$Title, [int]$Cap, [string]$Dir, [int]$Secs = 180, [int]$Loop = 1)
$busy = Get-Process rex-wallpaper-runtime -ErrorAction SilentlyContinue
if ($busy) { Write-Output ("LAYER BUSY pid=" + $busy[0].Id + " abort"); exit 3 }
$cjs = 'C:\Users\Moinu\AppData\Local\Temp\opencode\rex-verify\fps_log.cjs'
$fp = Join-Path $Dir 'fps.txt'
$arg = '"' + $cjs + '" "' + $Exe + '" "' + $Media + '" ' + $Title + ' ' + $Cap + ' "' + $fp + '" ' + $Secs + ' ' + $Loop
$node = Start-Process node -ArgumentList $arg -PassThru
Start-Sleep -Seconds 8
$rtPid = 0
$first = Get-Content $fp -ErrorAction SilentlyContinue | Select-Object -First 1
if ($first -match 'PID=(\d+)') { $rtPid = $matches[1] }
Write-Output ("title=" + $Title + " rtPid=" + $rtPid)
$csv = Join-Path $Dir 'sys.csv'
if ($rtPid -ne 0) {
    $paths = @(
        '\Process(rex-wallpaper-runtime*)\% Processor Time',
        '\Process(rex-wallpaper-runtime*)\Working Set',
        '\Process(rex-wallpaper-runtime*)\Private Bytes',
        '\Process(rex-wallpaper-runtime*)\IO Read Bytes/sec',
        '\Process(rex-wallpaper-runtime*)\ID Process',
        '\GPU Engine(*engtype_3D*)\Utilization Percentage',
        '\GPU Engine(*engtype_Video*)\Utilization Percentage',
        '\GPU Engine(*engtype_Copy*)\Utilization Percentage'
    )
    $n = $Secs - 5
    if ($n -lt 5) { $n = 5 }
    Set-Content (Join-Path $Dir 'rtpid.txt') $rtPid
    typeperf $paths -si 1 -sc $n -o $csv -y
    Write-Output 'sysmon done'
} else {
    Write-Output 'NO_PID skip sysmon'
}
$node | Wait-Process -Timeout 400 -ErrorAction SilentlyContinue
$node.Refresh()
Write-Output ("nodeExited=" + $node.HasExited)
$nlines = (Get-Content $fp -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch '^PID=' } | Measure-Object).Count
if ($nlines -lt ($Secs - 10)) { Write-Output ("nodeSHORT dataLines=" + $nlines + " expected~" + $Secs) }
