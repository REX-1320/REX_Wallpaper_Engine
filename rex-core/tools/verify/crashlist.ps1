$ev = Get-WinEvent -FilterHashtable @{LogName='Application'; ID=1000} -MaxEvents 12 -ErrorAction SilentlyContinue
foreach ($e in $ev) {
  $m = $e.Message
  $app = ''; $mod = ''; $code = ''
  foreach ($ln in ($m -split "`r?`n")) {
    if ($ln -match '^Faulting application name:\s*(.*)') { $app = $matches[1] }
    if ($ln -match '^Faulting module name:\s*(.*)') { $mod = $matches[1] }
    if ($ln -match '^Exception code:\s*(.*)') { $code = $matches[1] }
  }
  Write-Output ($e.TimeCreated.ToString('HH:mm:ss') + ' | ' + $app + ' | ' + $mod + ' | ' + $code)
}
