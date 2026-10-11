param([string]$Md = 'D:\REX_doc\fps.md')
$lines = Get-Content $Md
$inV5 = $false
$rows = @()
foreach ($l in $lines) {
  if ($l -match '^### v5') { $inV5 = $true; continue }
  if ($inV5 -and $l -match '^## Raw data') { break }
  if ($inV5 -and $l -match '^\s*skyscraper.*out=([\d.]+) \| eff=([\d.]+)') {
    $rows += ,@([double]$matches[1], [double]$matches[2])
  }
}
Write-Output ("total=" + $rows.Count)
function avgIdx($idx, $skip, $excl) {
  $v = @()
  for ($i = 0; $i -lt $rows.Count; $i++) {
    $s = $i + 1
    if ($s -le $skip) { continue }
    $hit = $false
    foreach ($w in $excl) { if ($s -ge ($w[0]-2) -and $s -le ($w[1]+2)) { $hit = $true } }
    if ($hit) { continue }
    $v += $rows[$i][$idx]
  }
  $m = $v | Measure-Object -Average -Minimum
  return 'n=' + $v.Count + ' avg=' + $m.Average.ToString('F2') + ' min=' + $m.Minimum.ToString('F1')
}
$wraps = @(@(55,57), @(109,113), @(163,168))
Write-Output ('steady-out (excl s1-30 + wraps): ' + (avgIdx 0 30 $wraps))
Write-Output ('steady-eff (excl s1-30 + wraps): ' + (avgIdx 1 30 $wraps))
Write-Output ('all-out: ' + (($rows | ForEach-Object { $_[0] } | Measure-Object -Average).Average).ToString('F2'))
