param([string]$Secs = 12)
$exe = 'D:\REX_doc\rex-core\.build\Release\rex-wallpaper-runtime.exe'
$media = 'D:\live wallpaper\astral-defender-acheron.3840x2160.mp4'
$dir = 'C:\Users\Moinu\AppData\Local\Temp\opencode\rex-verify\probe2'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$cjs = 'C:\Users\Moinu\AppData\Local\Temp\opencode\rex-verify\fps_log.cjs'
$fp = Join-Path $dir 'fps.txt'
if (Test-Path $fp) { Remove-Item $fp -Force }
$err = Join-Path $dir 'node.err'
& node $cjs $exe $media 'probe2' 60 $fp $Secs 2> $err
Write-Output ("exit=" + $LASTEXITCODE)
Write-Output '--- fps.txt ---'
Get-Content $fp -ErrorAction SilentlyContinue | Select-Object -First 5
Write-Output '--- node.err ---'
Get-Content $err -ErrorAction SilentlyContinue | Select-Object -First 10
