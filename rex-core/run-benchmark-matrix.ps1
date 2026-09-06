# P13 Cross-GPU benchmark matrix runner.
#
# REQUIRES real hardware-codec clips in -MediaRoot (MPEG-4 ASP will abort
# with "hardware decode unavailable" — D3D11VA does not accelerate mp4v):
#   h264_1080p30.mp4  h264_1080p60.mp4  h264_1440p60.mp4
#   h264_4k30.mp4     h264_4k60.mp4
#   hevc_<same resolutions>.mp4        av1_<resolutions>.mp4
#
# Usage (both machines):
#   powershell -File run-benchmark-matrix.ps1 -MachineTag amd6800
#   powershell -File run-benchmark-matrix.ps1 -MachineTag rtx4050
# Results land in .\bench-results\<MachineTag>\<clip>\<hw|sw>\ and a
# summary CSV at .\bench-results\<MachineTag>\summary.csv
param(
    [string]$RuntimePath = "D:\REX_doc\rex-core\.build\Release\rex-wallpaper-runtime.exe",
    [string]$MediaRoot   = "D:\REX_doc\bench-media\matrix",
    [string]$MachineTag  = "amd",
    [int]$MaxFrames      = 600
)
$ErrorActionPreference = 'Continue'

function Get-Mp4Codec([string]$path) {
    $s = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    foreach ($tag in 'avc1','hev1','hvc1','av01','vp09','mp4v') {
        if ($s.Contains($tag)) {
            return @{
                avc1 = 'h264'; hev1 = 'hevc'; hvc1 = 'hevc';
                av01 = 'av1'; vp09 = 'vp9'; mp4v = 'mp4v' }[$tag]
        }
    }
    return 'unknown'
}

$root = Join-Path $PSScriptRoot "bench-results\$MachineTag"
New-Item -ItemType Directory -Force -Path $root | Out-Null
$csv = Join-Path $root 'summary.csv'
"time,machine,clip,codec,res_hw,mode,startup_ms,decoded,errors,fps,vram_mb,private_mb,status" |
    Out-File $csv -Encoding utf8

$clips = Get-ChildItem $MediaRoot -Include *.mp4,*.mkv,*.webm -Recurse -File
if (-not $clips) { Write-Error "no clips under $MediaRoot"; exit 1 }

foreach ($clip in $clips) {
    $codec = Get-Mp4Codec $clip.FullName
    foreach ($mode in @('hw','sw')) {
        $arg = if ($mode -eq 'sw') { '--no-hw' } else { '' }
        $caseDir = Join-Path $root "$($clip.BaseName)\$mode"
        New-Item -ItemType Directory -Force -Path $caseDir | Out-Null

        Write-Host "=== $($clip.Name) [$codec] mode=$mode ==="
        $out = & $RuntimePath --benchmark $arg --frames $MaxFrames `
                --file $clip.FullName 2>&1
        $out | Out-File (Join-Path $caseDir 'console.txt') -Encoding utf8

        # Snapshot logs before the next run overwrites them.
        $tmp = Join-Path $env:TEMP 'REX'
        foreach ($log in 'benchmark.log','runtime-telemetry.log') {
            $src = Join-Path $tmp $log
            if (Test-Path $src) { Copy-Item $src $caseDir -Force }
        }
        $crash = Join-Path $tmp 'crash.log'
        if (Test-Path $crash) { Copy-Item $crash (Join-Path $caseDir 'crash.log') -Force }

        $line = [regex]::Match(($out -join "`n"), '(?ms)input:.*?status:\s*(.+)$').Value
        function Grab([string]$pat) {
            $m = [regex]::Match($line, $pat); if ($m.Success) { $m.Groups[1].Value } else { '' }
        }
        $resHw = Grab ('input:\s*\S+\s+(\S+),')
        $row = '{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12}' -f `
            (Get-Date -Format o), $MachineTag, $clip.Name, $codec, $resHw, $mode,
            (Grab 'startup:\s*([\d.]+)'),          # ms
            (Grab 'frames:\s*(\d+) decoded'),
            (Grab '(\d+) decode errors'),
            (Grab 'throughput:\s*([\d.-]+)'),
            (Grab 'gpu vram:\s*([\d.]+)'),
            (Grab 'process:\s*([\d.]+)'),
            (Grab 'status:\s*(.+?)\r?$')
        $row | Out-File $csv -Append -Encoding utf8
    }
}
Write-Host "DONE -> $csv"
