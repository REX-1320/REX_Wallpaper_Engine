$ErrorActionPreference = 'Stop'
$exe = 'D:\REX_doc\app\node_modules\electron\dist\electron.exe'
$wd = 'D:\REX_doc\app'
$log = "$env:TEMP\rex-app.log"
$media = 'D:\REX_doc\bench-media\mpeg4_1280x720.mp4'

Stop-Process -Name electron,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Remove-Item $log -ErrorAction SilentlyContinue

function Main-Log {
    if (Test-Path $log) { Get-Content $log -Raw }
}

Write-Output '== 1. launch + apply video wallpaper =='
$p = Start-Process -FilePath $exe -ArgumentList '.', '--remote-debugging-port=9222' -WorkingDirectory $wd -PassThru
Start-Sleep -Seconds 9

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

$apply = E "window.rexDesktop.setDesktopVideo({ path: 'D:/REX_doc/bench-media/mpeg4_1280x720.mp4', mode: 'fill', loop: true, muted: true }); 'applied'"
Write-Output ('apply result: ' + $apply)
Start-Sleep -Seconds 3
$logA = Main-Log
Write-Output ('diagnostics line logged: ' + [bool]($logA -match 'diagnostics:'))
$runtimePid = @(Get-Process -Name rex-wallpaper-runtime -ErrorAction SilentlyContinue | Select-Object -First 1).Id
Write-Output ('runtime running before kill: ' + [bool]$runtimePid + ' (pid ' + $runtimePid + ')')

Write-Output '== 2. kill the runtime process (simulated crash) =='
Stop-Process -Id $runtimePid -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 12

$logR = Main-Log
Write-Output ('runtime:exit detected   : ' + [bool]($logR -match 'runtime:exit'))
Write-Output ('runtime:recovery start  : ' + [bool]($logR -match 'runtime:recovery start'))
Write-Output ('runtime:recovery ok     : ' + [bool]($logR -match 'runtime:recovery ok'))
Write-Output ('reapplied log line      : ' + [bool]($logR -match 'runtime:recovery reapplied last wallpaper'))
$newPid = @(Get-Process -Name rex-wallpaper-runtime -ErrorAction SilentlyContinue | Select-Object -First 1).Id
Write-Output ('runtime restarted       : ' + [bool]$newPid + ' (pid ' + $newPid + ')')

Write-Output ''
Write-Output '--- recovery lines from app log ---'
(Get-Content $log | Select-String 'runtime:|diagnostics:' | ForEach-Object { ($_ -split '\] ')[1] })

$ws.Dispose()
Stop-Process -Name electron,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
