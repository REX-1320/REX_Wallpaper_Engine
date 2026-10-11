param([string]$F = 'C:\Users\Moinu\AppData\Local\Temp\opencode\rex-verify\v2\sys.csv')
$l = Get-Content $F
$l | Select-Object -Last 3
$have = ($l | Where-Object { $_ -match '"\d' } | Measure-Object).Count
Write-Output ("rows-with-data=" + $have + " total=" + $l.Count)
