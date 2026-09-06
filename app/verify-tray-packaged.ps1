$ErrorActionPreference = 'Stop'
$exe = 'D:\REX_doc\app\dist-exe\REX.exe'
$log = "$env:TEMP\rex-app.log"

Stop-Process -Name REX,rex-wallpaper-runtime,electron -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Remove-Item $log -ErrorAction SilentlyContinue

function Main-Log { if (Test-Path $log) { Get-Content $log | ForEach-Object { ($_ -split '\] ')[1] } } }

Write-Output '== packaged: normal launch =='
$p1 = Start-Process -FilePath $exe -ArgumentList '--remote-debugging-port=9222' -PassThru
Start-Sleep -Seconds 14
$l1 = Main-Log
Write-Output ('tray created: ' + [bool]($l1 -match 'tray: created'))
Write-Output ('window shown : ' + [bool]($l1 -match 'window: shown'))
$procsBefore = @(Get-Process -Name REX -ErrorAction SilentlyContinue).Count

Write-Output '== packaged: wallpaper apply + pause/resume over IPC =='
$t = Invoke-RestMethod 'http://127.0.0.1:9222/json' -TimeoutSec 3
$page = $t | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$id = 0
function E([string]$e) {
    $global:id++
    $m = @{ id = $global:id; method = 'Runtime.evaluate'; params = @{ expression = $e; returnByValue = $true; awaitPromise = $true } } | ConvertTo-Json -Compress -Depth 5
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
E "window.__ev=[]; window.rexDesktop.onWallpaperEvent(e=>window.__ev.push(e.event)); true" | Out-Null
$apply = E "window.rexDesktop.setDesktopVideo({path:'C:/Program Files/Wallpaper Engine/live wallpaper/red-eyes-anime-girl-1-moewalls-com.mp4',mode:'fill',fps:60,loop:true,muted:true,volume:1,speed:1}).then(r=>JSON.stringify(r))"
Start-Sleep -Seconds 3
$pause = E "window.rexDesktop.pauseWallpaper().then(r=>JSON.stringify(r))"
Start-Sleep -Seconds 1
$resume = E "window.rexDesktop.resumeWallpaper().then(r=>JSON.stringify(r))"
Start-Sleep -Seconds 1
$ev = E "JSON.stringify([...new Set(window.__ev)])"
Write-Output "apply=$apply pause=$pause resume=$resume"
Write-Output "events seen: $ev"
Write-Output ('pause in log: ' + [bool]((Main-Log) -match 'wallpaper:pause ok'))
Write-Output ('resume in log: ' + [bool]((Main-Log) -match 'wallpaper:resume ok'))

Write-Output '== packaged: close -> hide, alive =='
E 'window.rexDesktop.close(); true' | Out-Null
Start-Sleep -Seconds 3
Write-Output ('window hidden logged: ' + [bool]((Main-Log) -match 'window: hidden'))
Write-Output ('process alive after close: ' + [bool](Get-Process -Id $p1.Id -ErrorAction SilentlyContinue))
$ws.Dispose()

Write-Output '== packaged: duplicate launch -> B exits, A restores =='
Start-Process -FilePath $exe -ArgumentList '--remote-debugging-port=9223' | Out-Null
Start-Sleep -Seconds 10
$lB = Main-Log
Write-Output ('tray: created count = ' + (($lB | Select-String 'tray: created').Count) + ' (want 1)')
Write-Output ('second-instance restore fired: ' + [bool]($lB -match 'second-instance: restoring window'))
$procsAfter = @(Get-Process -Name REX -ErrorAction SilentlyContinue).Count
Write-Output "REX procs before B=$procsBefore after B=$procsAfter (want equal -> B exited)"

Stop-Process -Name REX,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3

Write-Output '== packaged: background launch =='
Remove-Item $log -ErrorAction SilentlyContinue
Start-Process -FilePath $exe -ArgumentList '--background', '--remote-debugging-port=9224' | Out-Null
Start-Sleep -Seconds 12
$lBg = Main-Log
Write-Output ('tray created: ' + [bool]($lBg -match 'tray: created'))
Write-Output ('window kept hidden: ' + [bool]($lBg -match 'window: kept hidden'))
Write-Output ('window NOT shown: ' + (-not [bool]($lBg -match 'window: shown')))

Stop-Process -Name REX,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
