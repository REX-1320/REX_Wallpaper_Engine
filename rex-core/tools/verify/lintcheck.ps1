$out = pnpm lint 2>&1
$hits = $out | Where-Object { $_ -match 'gallery|main\.cjs|preload|error|Found' }
if ($hits.Count -eq 0) { Write-Output 'LINT: no gallery/main/preload/error lines' }
else { $hits | Select-Object -Last 10 }
