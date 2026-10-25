param([string]$List)
$items = Import-Csv $List
foreach ($it in $items) {
    $dir = "$env:TEMP\opencode\rex-verify\" + $it.tag
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    powershell -NoProfile -ExecutionPolicy Bypass -File "$env:TEMP\opencode\rex-verify\run_one.ps1" -Exe 'D:\REX_doc\rex-core\.build\Release\paiint-wallpaper-runtime.exe' -Media $it.media -Title $it.title -Cap $it.cap -Dir $dir
    Start-Sleep -Seconds 5
}
Write-Output 'batch done'
