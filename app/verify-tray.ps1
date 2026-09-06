$ErrorActionPreference = 'Stop'
$exe = 'D:\REX_doc\app\node_modules\electron\dist\electron.exe'
$wd = 'D:\REX_doc\app'
$log = "$env:TEMP\rex-app.log"

Stop-Process -Name electron,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Remove-Item $log -ErrorAction SilentlyContinue

function Main-Log {
    if (Test-Path $log) { Get-Content $log | ForEach-Object { ($_ -split '\] ')[1] } }
}

Write-Output '== 1. normal launch A =='
$pA = Start-Process -FilePath $exe -ArgumentList '.', '--remote-debugging-port=9222' -WorkingDirectory $wd -PassThru
Start-Sleep -Seconds 9
$logA1 = Main-Log
Write-Output ('tray created: ' + [bool]($logA1 -match 'tray: created'))
Write-Output ('window shown : ' + [bool]($logA1 -match 'window: shown'))
$procsBefore = @(Get-Process -Name electron -ErrorAction SilentlyContinue).Count

Write-Output '== 2. close window -> hide, stay alive =='
$t = Invoke-RestMethod 'http://127.0.0.1:9222/json' -TimeoutSec 3
$page = $t | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$id = 0
function E([string]$e) {
    $global:id++
    $m = @{ id = $global:id; method = 'Runtime.evaluate'; params = @{ expression = $e; returnByValue = $true } } | ConvertTo-Json -Compress -Depth 5
    $b = [Text.Encoding]::UTF8.GetBytes($m)
    [void]$ws.SendAsync([ArraySegment[byte]]::new($b), [Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    $sb = New-Object Text.StringBuilder
    while (1) {
        $buf = New-Object byte[] 65536
        $r = $ws.ReceiveAsync([ArraySegment[byte]]::new($buf), [Threading.CancellationToken]::None).GetAwaiter().GetResult()
        [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $r.Count))
        if ($r.EndOfMessage) {
            $o = $sb.ToString() | ConvertFrom-Json
            if ($o.id -eq $global:id) { return $o.result.result.value }
        }
    }
}
E 'window.rexDesktop.close(); true' | Out-Null
Start-Sleep -Seconds 3
$logAfterClose = Main-Log
Write-Output ('window hidden logged: ' + [bool]($logAfterClose -match 'window: hidden'))
Write-Output ('A alive after close  : ' + [bool](Get-Process -Id $pA.Id -ErrorAction SilentlyContinue))

Write-Output '== 3. duplicate launch B -> B exits, A restores =='
Start-Process -FilePath $exe -ArgumentList '.', '--remote-debugging-port=9223' -WorkingDirectory $wd | Out-Null
Start-Sleep -Seconds 6
$logB = Main-Log
Write-Output ('no 2nd tray (B did not init): ' + (-not [bool]($logB -match '(?s)^tray: created$.*^tray: created$')))
Write-Output ('second-instance restore fired: ' + [bool]($logB -match 'second-instance: restoring window'))
$trayCount = (($logB | Select-String 'tray: created').Count)
Write-Output ("tray: created count = $trayCount (want 1)")
$procsAfter = @(Get-Process -Name electron -ErrorAction SilentlyContinue).Count
Write-Output "electron procs before B=$procsBefore after B=$procsAfter (want equal -> B exited)"

$ws.Dispose()
Stop-Process -Name electron,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Output '== 4. background launch (--background) =='
Remove-Item $log -ErrorAction SilentlyContinue
Start-Process -FilePath $exe -ArgumentList '.', '--background', '--remote-debugging-port=9224' -WorkingDirectory $wd | Out-Null
Start-Sleep -Seconds 9
$logBg = Main-Log
Write-Output ('tray created in background : ' + [bool]($logBg -match 'tray: created'))
Write-Output ('window kept hidden        : ' + [bool]($logBg -match 'window: kept hidden'))
Write-Output ('window NOT shown          : ' + (-not [bool]($logBg -match 'window: shown')))

Stop-Process -Name electron,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
