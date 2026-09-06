$ErrorActionPreference = 'Stop'
$exe = 'D:\REX_doc\app\node_modules\electron\dist\electron.exe'
$wd = 'D:\REX_doc\app'
$log = "$env:TEMP\rex-app.log"
$media = 'D:/REX_doc/bench-media/mpeg4_1280x720.mp4'
$crashesDir = "$env:TEMP\REX\crashes"
$beforeCrashes = @(Get-ChildItem $crashesDir -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
$launchStamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss')

Write-Output '== 0. launch dev electron (isolated userData) =='
$env:REX_TEST_USER_DATA = "$env:TEMP\rex-ud-test3"
$p = Start-Process -FilePath $exe -ArgumentList @('.', '--remote-debugging-port=9222') -WorkingDirectory $wd -PassThru
Start-Sleep -Seconds 10

$t = $null
for ($i = 0; $i -lt 10 -and -not $t; $i++) {
    try { $t = Invoke-RestMethod 'http://127.0.0.1:9222/json' -TimeoutSec 2 } catch { Start-Sleep -Seconds 1 }
}
if (-not $t) { throw 'CDP unreachable' }
Write-Output ('electron main pid: ' + $p.Id)
$page = $t | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$global:id = 0
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

function My-Runtime-Id {
    $procs = Get-CimInstance Win32_Process -Filter "Name='rex-wallpaper-runtime.exe'" -ErrorAction SilentlyContinue
    @($procs | Where-Object { $_.ParentProcessId -eq $p.Id } | Select-Object -First 1 -ExpandProperty ProcessId)
}

Write-Output '== 1. apply video wallpaper =='
$apply = E "window.rexDesktop.setDesktopVideo({ path: '$media', mode: 'fill', loop: true, muted: true }).then(r => JSON.stringify(r)); 'applied'"
Write-Output ('apply result: ' + $apply)
Start-Sleep -Seconds 4
$runtimeId = My-Runtime-Id
Write-Output ('my runtime running: ' + [bool]$runtimeId + ' (pid ' + $runtimeId + ')')
if (-not $runtimeId) { throw 'runtime did not spawn' }

Write-Output '== 2. kill runtime (simulated crash) =='
Stop-Process -Id $runtimeId -Force
Start-Sleep -Seconds 12

$logR = Get-Content $log -Raw -ErrorAction SilentlyContinue
$mine = $logR.Split("`n") | Where-Object { $_ -match $launchStamp -or $_ -gt $launchStamp }
$mineText = $mine -join "`n"
Write-Output ('supervisor:start logged     : ' + [bool]($mineText -match 'supervisor:start'))
Write-Output ('runtime:exit detected       : ' + [bool]($mineText -match 'runtime:exit'))
Write-Output ('runtime:recovery start      : ' + [bool]($mineText -match 'runtime:recovery start'))
Write-Output ('runtime:recovery ok         : ' + [bool]($mineText -match 'runtime:recovery ok'))
Write-Output ('reapplied line              : ' + [bool]($mineText -match 'runtime:recovery reapplied last wallpaper'))
$newId = My-Runtime-Id
Write-Output ('runtime restarted           : ' + [bool]$newId + ' (pid ' + $newId + ')')

Write-Output '== 3. crash report written =='
$newDirs = @(Get-ChildItem $crashesDir -Directory -ErrorAction SilentlyContinue | Where-Object { $beforeCrashes -notcontains $_.Name })
Write-Output ('new report dirs: ' + $newDirs.Count)
foreach ($d in $newDirs) {
    $files = @(Get-ChildItem $d.FullName | Select-Object -ExpandProperty Name)
    Write-Output ('  ' + $d.Name + ': ' + ($files -join ', '))
    $rep = Get-Content (Join-Path $d.FullName 'report.json') -Raw | ConvertFrom-Json
    Write-Output ('  reason: ' + $rep.reason + ' exitCode: ' + $rep.exitCode + ' state: ' + ($rep.recoveryState.lastWallpaper -split '\\')[-1])
}

Write-Output ''
Write-Output '--- my supervisor/recovery lines ---'
$mineText.Split("`n") | Select-String 'supervisor:|runtime:|crash-reporter:' | ForEach-Object { ($_ -split '\] ')[1] }

$ws.Dispose()
Write-Output '== cleanup =='
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$orphan = My-Runtime-Id
if ($orphan) { Stop-Process -Id $orphan -Force -ErrorAction SilentlyContinue }
'cleanup done'