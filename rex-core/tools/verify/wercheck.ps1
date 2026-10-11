$ev = Get-WinEvent -FilterHashtable @{LogName='Application'; ID=1001} -MaxEvents 3 -ErrorAction SilentlyContinue
foreach ($e in $ev) {
  Write-Output ('=== ' + $e.TimeCreated + ' ===')
  $e.Message | Select-Object -First 12
}
