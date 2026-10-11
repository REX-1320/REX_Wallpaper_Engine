$list = typeperf -qx '\GPU Engine'
Write-Output ("total=" + $list.Count)
$types = @{}
foreach ($l in $list) {
  if ($l -match 'engtype_([A-Za-z0-9]+)') { $t = $matches[1]; if (!$types.ContainsKey($t)) { $types[$t] = 0 }; $types[$t]++ }
}
foreach ($k in $types.Keys) { Write-Output ($k + ': ' + $types[$k]) }
