$ps = Get-CimInstance Win32_Process -Filter "Name='rex-wallpaper-runtime.exe'" -ErrorAction SilentlyContinue
foreach ($p in $ps) {
  $r = $p | Invoke-CimMethod -MethodName Terminate -ErrorAction SilentlyContinue
  Write-Output ('pid=' + $p.ProcessId + ' term-rc=' + $r.ReturnValue)
}
Start-Sleep 2
$left = Get-CimInstance Win32_Process -Filter "Name='rex-wallpaper-runtime.exe'" -ErrorAction SilentlyContinue
Write-Output ('remaining=' + (@($left).Count))
