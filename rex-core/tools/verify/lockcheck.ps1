try {
  $p = Get-Process -Id 11872 -ErrorAction Stop
  Write-Output ('PID-11872: alive name=' + $p.ProcessName + ' ws=' + $p.WorkingSet64)
} catch {
  Write-Output ('PID-11872: ' + $_.Exception.Message)
}
try {
  $s = [IO.File]::Open('D:\REX_doc\rex-core\.build\Release\rex-wallpaper-runtime.exe', 'Open', 'ReadWrite', 'None')
  Write-Output 'exe-open: UNLOCKED'
  $s.Close()
} catch {
  Write-Output ('exe-open: LOCKED (' + $_.Exception.Message + ')')
}
