param([string]$F = 'C:\Users\Moinu\AppData\Local\Temp\opencode\rex-verify\v4\fps.txt')
$rows = @()
foreach ($l in (Get-Content $F)) {
  if ($l -match 'out=([\d.]+) \| eff=([\d.]+) \| drop=(\d+) \| rep=(\d+) \| wrap=(\d+) \| cons=(\d+) \| decc=(\d+)') {
    $rows += ,@([double]$matches[1],[double]$matches[2],[uint64]$matches[3],[uint64]$matches[4],[uint64]$matches[5],[uint64]$matches[6],[uint64]$matches[7])
  }
}
Write-Output ("samples=" + $rows.Count)
$wrapMax = ($rows | ForEach-Object { $_[4] } | Measure-Object -Maximum).Maximum
Write-Output ("wrapMax=" + $wrapMax)
# steady window: skip first 10s; mindblow never wraps so all clean
$a = $rows[10]; $b = $rows[$rows.Count - 1]
$dt = $rows.Count - 1 - 10
$consRate = ($b[5] - $a[5]) / $dt
$deccRate = ($b[6] - $a[6]) / $dt
Write-Output ("steady-consume-rate=" + $consRate.ToString('F2') + "/s over " + $dt + "s (TRUE unique consume)")
Write-Output ("steady-decode-rate=" + $deccRate.ToString('F2') + "/s over " + $dt + "s")
$effs = @()
for ($i = 10; $i -lt $rows.Count; $i++) { $effs += $rows[$i][1] }
$m = $effs | Measure-Object -Average -Minimum -Maximum
Write-Output ('eff-window avg=' + $m.Average.ToString('F2') + ' min=' + $m.Minimum.ToString('F1') + ' max=' + $m.Maximum.ToString('F1'))
