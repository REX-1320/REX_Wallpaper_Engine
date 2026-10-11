$ev = Get-WinEvent -FilterHashtable @{LogName='Application'; ID=1000} -MaxEvents 4 -ErrorAction SilentlyContinue
foreach ($e in $ev) {
  Write-Output ('=== ' + $e.TimeCreated + ' ===')
  $e.Message | Select-Object -First 14
}
