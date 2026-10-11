param([string]$Base = 'C:\Users\Moinu\AppData\Local\Temp\opencode\rex-verify')
foreach ($v in @('v1','v2','v3','v4','v5')) {
  $f = Join-Path $Base "$v\fps.txt"
  if (!(Test-Path $f)) { Write-Output ($v + ': NO FILE'); continue }
  $rows = @(); $last = ''; $n = 0
  foreach ($l in (Get-Content $f)) {
    if ($l -match '^PID=') { continue }
    $n++; $last = $l
    if ($l -match 'dec=([\d.]+) \| rend=([\d.]+) \| out=([\d.]+)') {
      if ([double]$matches[3] -gt 0) { $rows += ,@([double]$matches[1],[double]$matches[2],[double]$matches[3]) }
    }
  }
  if ($rows.Count -eq 0) { Write-Output ($v + ': lines=' + $n + ' last=[' + $last + ']'); continue }
  $da = ($rows | ForEach-Object { $_[0] } | Measure-Object -Average).Average
  $ra = ($rows | ForEach-Object { $_[1] } | Measure-Object -Average).Average
  $oa = ($rows | ForEach-Object { $_[2] } | Measure-Object -Average).Average
  $dr = '?'; $rp = '?'
  if ($last -match 'drop=(\d+) \| rep=(\d+)') { $dr = $matches[1]; $rp = $matches[2] }
  $title = ($last -split ' \| ')[0]
  Write-Output ($v + ' ' + $title + ' n=' + $n + ' valid=' + $rows.Count + ' decAvg=' + $da.ToString('F1') + ' rendAvg=' + $ra.ToString('F1') + ' outAvg=' + $oa.ToString('F1') + ' drops=' + $dr + ' reps=' + $rp)
}
$s = Join-Path $Base 'v2\sys.csv'
if (Test-Path $s) {
  $csv = Import-Csv $s
  $cpuC = ($csv[0].PSObject.Properties.Name | Where-Object { $_ -match 'Processor Time' } | Select-Object -First 1)
  $gpuC = ($csv[0].PSObject.Properties.Name | Where-Object { $_ -match 'Utilization Percentage' } | Select-Object -First 1)
  $wsC = ($csv[0].PSObject.Properties.Name | Where-Object { $_ -match 'Working Set' } | Select-Object -First 1)
  $pvC = ($csv[0].PSObject.Properties.Name | Where-Object { $_ -match 'Private Bytes' } | Select-Object -First 1)
  $ioC = ($csv[0].PSObject.Properties.Name | Where-Object { $_ -match 'IO Read' } | Select-Object -First 1)
  $cpu = $csv | ForEach-Object { [double]$_.$cpuC } | Measure-Object -Average -Maximum
  $ws = $csv | ForEach-Object { [double]$_.$wsC } | Measure-Object -Average -Maximum
  $pv = $csv | ForEach-Object { [double]$_.$pvC } | Measure-Object -Average -Maximum
  $io = $csv | ForEach-Object { [double]$_.$ioC } | Measure-Object -Average -Maximum
  Write-Output ('astral-sysmon: samples=' + $csv.Count + ' cpuAvg=' + $cpu.Average.ToString('F1') + '% cpuMax=' + $cpu.Maximum.ToString('F1') + '% wsAvgMB=' + ($ws.Average/1MB).ToString('F0') + ' wsMaxMB=' + ($ws.Maximum/1MB).ToString('F0') + ' privAvgMB=' + ($pv.Average/1MB).ToString('F0') + ' ioAvgKBs=' + ($io.Average/1KB).ToString('F0'))
  if ($gpuC) { $g = $csv | ForEach-Object { [double]$_.$gpuC } | Measure-Object -Average -Maximum; Write-Output ('astral-gpu3d: avg=' + $g.Average.ToString('F1') + '% max=' + $g.Maximum.ToString('F1') + '%') }
  else { Write-Output 'astral-gpu3d: NO engtype counter found' }
}
