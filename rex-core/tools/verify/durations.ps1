param([string]$Out = "$env:TEMP\opencode\rex-verify\durations.txt")
$ff = "$env:LOCALAPPDATA\Programs\ffmpeg\ffmpeg-9.0.1-essentials_build\bin\ffprobe.exe"
$files = @(
  'D:\live wallpaper\black-myth-wukong-moewalls-com.mp4',
  'D:\live wallpaper\astral-defender-acheron.3840x2160.mp4',
  'D:\live wallpaper\miles-morales-walking-skyscraper-spiderman-moewalls-com.mp4',
  "$env:TEMP\opencode\rex-verify\blk4k.mp4",
  "$env:USERPROFILE\Downloads\Mind Blowing OLED Dolby Vision - 4K HDR 240 fps.mp4"
)
$r = @()
foreach ($f in $files) {
  $d = & $ff -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$f"
  $r += ($f + ' => ' + $d + 's')
}
$r | Out-File -Encoding utf8 $Out
$r
