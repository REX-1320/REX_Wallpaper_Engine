param([string]$Dir)
$csv = Join-Path $Dir 'sys.csv'
$pidFile = Join-Path $Dir 'rtpid.txt'
if (!(Test-Path $csv)) { Write-Output 'NO sys.csv'; exit 1 }
$rtPid = ''
if (Test-Path $pidFile) { $rtPid = (Get-Content $pidFile | Select-Object -First 1).Trim() }
$rows = Import-Csv $csv
$names = $rows[0].PSObject.Properties.Name
$idCols = $names | Where-Object { $_ -match 'ID Process' }
$me = @()
foreach ($c in $idCols) {
  $vals = $rows | ForEach-Object { ($_.$c).ToString().Trim() }
  $hit = ($vals | Where-Object { $_ -eq $rtPid } | Measure-Object).Count
  if ($hit -gt ($vals.Count / 2)) { $me += $c }
}
if ($me.Count -eq 0) { Write-Output ("PID " + $rtPid + " NOT FOUND in sys.csv; idcols=" + $idCols.Count); exit 1 }
$inst = $me[0] -replace 'ID Process.*$',''
Write-Output ("matched instance: " + $inst + " (rtPid=" + $rtPid + ")")
function agg($pat, $lbl, $div) {
  $col = $names | Where-Object { ($_ -match [regex]::Escape($inst)) -and ($_ -match $pat) } | Select-Object -First 1
  if (!$col) { Write-Output ($lbl + ': NO COLUMN'); return }
  $v = @()
  foreach ($r in $rows) { $t = $r.$col.ToString().Trim(); $d = 0; if ([double]::TryParse($t, [ref]$d)) { $v += $d } }
  if ($v.Count -eq 0) { Write-Output ($lbl + ': ALL BLANK'); return }
  $m = $v | Measure-Object -Average -Maximum -Minimum
  Write-Output ($lbl + ': n=' + $v.Count + ' avg=' + ($m.Average/$div).ToString('F1') + ' max=' + ($m.Maximum/$div).ToString('F1') + ' min=' + ($m.Minimum/$div).ToString('F1'))
}
agg 'Processor Time' 'cpu-pct' 1
agg 'Working Set' 'ws-MB' 1048576
agg 'Private Bytes' 'priv-MB' 1048576
agg 'IO Read' 'ioRead-KBs' 1024
$gpuCols = $names | Where-Object { $_ -match 'Utilization Percentage' }
foreach ($eng in @('3D', 'Video', 'Copy')) {
  $myEng = @($gpuCols | Where-Object { ($_ -match ('pid_' + $rtPid + '_')) -and ($_ -match ('engtype_' + $eng)) })
  if ($myEng.Count -eq 0) { Write-Output ("gpu-" + $eng + ": none for pid"); continue }
  $used = @()
  foreach ($r in $rows) {
    $rowSum = 0
    foreach ($c in $myEng) { $t = $r.$c.ToString().Trim(); $d = 0; if ([double]::TryParse($t, [ref]$d)) { $rowSum += $d } }
    $used += $rowSum
  }
  $m = $used | Measure-Object -Average -Maximum
  Write-Output ('gpu-' + $eng + '-pct: avg=' + $m.Average.ToString('F1') + ' max=' + $m.Maximum.ToString('F1') + ' engines=' + $myEng.Count)
}
if ($gpuCols.Count -eq 0) { Write-Output 'gpu: NO COLUMNS' }
else {
  $gmax = @()
  foreach ($r in $rows) {
    $rowMax = 0
    foreach ($c in $gpuCols) { $t = $r.$c.ToString().Trim(); $d = 0; if ([double]::TryParse($t, [ref]$d)) { if ($d -gt $rowMax) { $rowMax = $d } } }
    $gmax += $rowMax
  }
  $m = $gmax | Measure-Object -Average -Maximum
  Write-Output ('gpu3d-maxEng-pct: avg=' + $m.Average.ToString('F1') + ' max=' + $m.Maximum.ToString('F1') + ' engines=' + $gpuCols.Count)
}
