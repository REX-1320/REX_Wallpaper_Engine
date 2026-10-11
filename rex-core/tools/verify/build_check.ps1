cmake --build rex-core\.build --config Release > C:\Users\Moinu\AppData\Local\Temp\opencode\build_p1.log 2>&1
$errs = Select-String -Path C:\Users\Moinu\AppData\Local\Temp\opencode\build_p1.log -Pattern 'error|warning C' | Select-Object -First 15
if ($errs) { $errs | ForEach-Object { Write-Output $_.Line } }
else { Write-Output 'BUILD CLEAN (no errors)' }
Get-Content C:\Users\Moinu\AppData\Local\Temp\opencode\build_p1.log | Select-Object -Last 3
