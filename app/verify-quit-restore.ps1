$ErrorActionPreference = 'Stop'
$exe = 'D:\REX_doc\app\node_modules\electron\dist\electron.exe'
$wd = 'D:\REX_doc\app'
$log = "$env:TEMP\rex-app.log"
$media = 'D:/REX_doc/bench-media/mpeg4_1280x720.mp4'
$image = 'D:/REX_doc/bench-media/desktop_baseline.png'
$frame = "$env:TEMP\REX\wallpaper-frame.png"
$launchStamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss')

function Start-REX {
    $env:REX_TEST_USER_DATA = "$env:TEMP\rex-ud-quitrestore"
    Remove-Item $env:REX_TEST_USER_DATA -Recurse -Force -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe -ArgumentList @('.', '--remote-debugging-port=9223', '--background') -WorkingDirectory $wd -PassThru
    Start-Sleep -Seconds 10
    $t = $null
    for ($i = 0; $i -lt 10 -and -not $t; $i++) {
        try { $t = Invoke-RestMethod 'http://127.0.0.1:9223/json' -TimeoutSec 2 } catch { Start-Sleep -Seconds 1 }
    }
    if (-not $t) { throw 'CDP unreachable' }
    $page = $t | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    $ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    $global:id = 0
    return @{ proc = $p; ws = $ws }
}

function E($ws, [string]$e) {
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

function LogLines {
    $logR = Get-Content $log -Raw -ErrorAction SilentlyContinue
    $rows = @($logR.Split("`n") | Where-Object { $_ -match '^\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}' })
    $from = [datetime]::ParseExact($launchStamp, 'yyyy-MM-ddTHH:mm:ss', $null).ToUniversalTime()
    $sel = @($rows | Where-Object {
        $t = [datetime]::ParseExact($_.Substring(1, 19), 'yyyy-MM-ddTHH:mm:ss', $null)
        $t -ge $from
    })
    ($sel -join "`n")
}

function Wait-AppExit($proc) {
    try {
        Wait-Process -Id $proc.Id -Timeout 25 -ErrorAction Stop
        return $true
    } catch {
        return $false
    }
}

Write-Output '== VIDEO CASE: apply video, then quit =='
$app = Start-REX
$ws = $app.ws
$apply = E $ws "window.rexDesktop.setDesktopVideo({ path: '$media', mode: 'fill', loop: true, muted: true }).then(r => JSON.stringify(r)); 'applied'"
Write-Output ('apply: ' + $apply)
Start-Sleep -Seconds 4
if (Test-Path $frame) { Remove-Item $frame -Force }
E $ws 'window.rexDesktop.quit(); "quit sent"'
$exited = Wait-AppExit $app.proc
if (-not $exited) { throw 'electron did not exit within 25s' }
Write-Output 'electron exited'
$ws.Dispose()

$frameInfo = Get-Item $frame -ErrorAction SilentlyContinue
Write-Output ('wallpaper-frame.png exists   : ' + [bool]$frameInfo)
if ($frameInfo) {
    Write-Output ('frame size                   : ' + $frameInfo.Length)
    Write-Output ('frame fresh (post-launch)    : ' + [bool]($frameInfo.LastWriteTimeUtc -gt [datetime]::Parse($launchStamp)))
}
$mine = LogLines
Write-Output ('quit-restore SAVE_FRAME      : ' + [bool]($mine -match 'wallpaper:quit-restore frame saved'))
Write-Output ('quit-restore SET_WALLPAPER   : ' + [bool]($mine -match 'wallpaper:quit-restore frame set as OS wallpaper'))
$reg = (Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name Wallpaper -ErrorAction SilentlyContinue).Wallpaper
Write-Output ('registry wallpaper now       : ' + $reg)
Write-Output ('registry = frame path        : ' + [bool]($reg -and $reg -eq $frame))

Write-Output ''
Write-Output '== IMAGE CASE: apply image, then quit =='
$env:REX_TEST_USER_DATA = "$env:TEMP\rex-ud-quitrestore"
Remove-Item $env:REX_TEST_USER_DATA -Recurse -Force -ErrorAction SilentlyContinue
$app = Start-REX
$ws = $app.ws
$apply = E $ws "window.rexDesktop.setDesktopWallpaper('$image', 'fill').then(r => JSON.stringify(r)); 'applied'"
Write-Output ('apply: ' + $apply)
Start-Sleep -Seconds 4
E $ws 'window.rexDesktop.quit(); "quit sent"'
$exited = Wait-AppExit $app.proc
if (-not $exited) { throw 'electron did not exit within 25s' }
$ws.Dispose()
$mine = LogLines
Write-Output ('quit-restore image           : ' + [bool]($mine -match 'wallpaper:quit-restore image set as OS wallpaper'))
$reg = (Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name Wallpaper -ErrorAction SilentlyContinue).Wallpaper
Write-Output ('registry wallpaper now       : ' + $reg)
Write-Output ('registry = image path        : ' + [bool]($reg -and $reg -eq ($image -replace '/', '\')))

Write-Output ''
Write-Output '--- restore lines ---'
$mine.Split("`n") | Select-String 'wallpaper:quit-restore' | ForEach-Object { ($_ -split '\] ')[1] }
Write-Output 'done'