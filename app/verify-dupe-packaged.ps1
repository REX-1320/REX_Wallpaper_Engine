$ErrorActionPreference = 'Continue'
$exe = 'D:\REX_doc\app\dist-exe\REX.exe'
$log = "$env:TEMP\rex-app.log"

Stop-Process -Name REX,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Remove-Item $log -ErrorAction SilentlyContinue
Start-Process -FilePath $exe -ArgumentList '--remote-debugging-port=9222' | Out-Null
Start-Sleep -Seconds 14
Write-Output ("after A start: REX procs=" + @(Get-Process -Name REX -ErrorAction SilentlyContinue).Count)

Write-Output '== duplicate launch B =='
Start-Process -FilePath $exe -ArgumentList '--remote-debugging-port=9223' | Out-Null
foreach ($sec in 5, 10, 15, 20) {
    Start-Sleep -Seconds 5
    $c = @(Get-Process -Name REX -ErrorAction SilentlyContinue).Count
    $logTxt = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    $restore = $logTxt -match 'second-instance: restoring window'
    $trays = ([regex]::Matches($logTxt, 'tray: created')).Count
    Write-Output ("t+$sec s: REX procs=$c trays=$trays restore=$restore")
}
Stop-Process -Name REX,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
