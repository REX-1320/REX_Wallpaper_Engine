$ps = Get-CimInstance Win32_Process -Filter "Name='rex-wallpaper-runtime.exe'" -ErrorAction SilentlyContinue
foreach ($p in $ps) {
  $owner = 'n/a'
  try { $o = $p.GetOwner(); $owner = ($o.Domain + '\' + $o.User) } catch { }
  Write-Output ('pid=' + $p.ProcessId + ' ppid=' + $p.ParentProcessId + ' owner=' + $owner + ' cmd=' + $p.CommandLine)
}
