param(
    [Parameter(Mandatory = $true)][ValidateSet('B', 'C', 'Leak')][string]$Mode,
    [int]$Minutes = 10,
    [string]$Clip = 'D:\REX_doc\bench-media\cadence_1080p_60s.mp4'
)

$ErrorActionPreference = 'Stop'
$clipName = [System.IO.Path]::GetFileName($Clip)

function Get-PageWs {
    for ($i = 0; $i -lt 30; $i++) {
        try {
            $targets = Invoke-RestMethod 'http://127.0.0.1:9222/json' -TimeoutSec 2
            $page = $targets | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
            if ($page -and $page.webSocketDebuggerUrl) { return $page.webSocketDebuggerUrl }
        } catch {}
        Start-Sleep -Seconds 2
    }
    throw 'CDP not reachable'
}

$wsUrl = Get-PageWs
Write-Output "ws: $wsUrl"

$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$wsUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()

$script:id = 0
$pending = [System.Collections.Generic.Dictionary[int, object]]::new()

function Invoke-Cdp([string]$method, $params = @{}) {
    $script:id += 1
    $msg = @{ id = $script:id; method = $method; params = $params } | ConvertTo-Json -Compress -Depth 10
    $bytes = [Text.Encoding]::UTF8.GetBytes($msg)
    $seg = [ArraySegment[byte]]::new($bytes)
    [void]$ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    $sb = New-Object System.Text.StringBuilder
    while ($true) {
        $buf = New-Object byte[] 65536
        $resSeg = [ArraySegment[byte]]::new($buf)
        $res = $ws.ReceiveAsync($resSeg, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
        [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $res.Count))
        if ($res.EndOfMessage) {
            try {
                $obj = $sb.ToString() | ConvertFrom-Json
                if ($obj.id -eq $script:id) { return $obj }
                $sb.Clear()
            } catch {
                $sb.Clear()
            }
        }
    }
}

function Eval([string]$expr) {
    $r = Invoke-Cdp 'Runtime.evaluate' @{ expression = $expr; returnByValue = $true }
    return $r.result.result.value
}

function ClickTab([string]$label) {
    [void](Eval "document.querySelector('a.sidebar-link[title=""$label""]')?.click(); true")
    Start-Sleep -Milliseconds 800
}

function SetFiles([string[]]$paths) {
    $doc = Invoke-Cdp 'DOM.getDocument' @{ depth = -1 }
    $q = Invoke-Cdp 'DOM.querySelector' @{ nodeId = $doc.result.root.nodeId; selector = 'input[type="file"]' }
    if ($q.result.nodeId -le 0) { throw 'file input not found' }
    $null = Invoke-Cdp 'DOM.setFileInputFiles' @{ nodeId = $q.result.nodeId; files = $paths }
    Start-Sleep -Seconds 3
}

function LogMark([string]$text) {
    $line = "=== $text ==="
    try { Add-Content -Path (Join-Path $env:TEMP 'REX\electron-memory.log') -Value $line } catch {}
    Write-Output $line
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$totalMs = $Minutes * 60000
$cycle = 0

while ($sw.ElapsedMilliseconds -lt $totalMs) {
    $cycle += 1
    if ($Mode -eq 'B') {
        $tabs = @('Home', 'Widgets', 'Wallpapers', 'shadcn', 'Home', 'Buttons', 'Profile', 'Wallpapers')
        ClickTab $tabs[($cycle - 1) % $tabs.Count]
        [void](Eval "document.querySelector('.sidebar-toggle')?.click(); true")
        Start-Sleep -Milliseconds 700
        [void](Eval "document.querySelector('.sidebar-toggle')?.click(); true")
        Start-Sleep -Milliseconds 700
        [void](Eval "document.querySelector('[role=""switch""]')?.click(); true")
        Start-Sleep -Milliseconds 700
        [void](Eval "window.scrollTo(0, 300); true")
        Start-Sleep -Milliseconds 500
        if ($cycle % 3 -eq 0) {
            [void](Eval "document.querySelector('.search-pill-icon')?.click(); true")
            Start-Sleep -Milliseconds 900
            [void](Eval "document.querySelector('.search-pill-icon')?.click(); true")
            Start-Sleep -Milliseconds 500
        }
    }
    elseif ($Mode -eq 'C') {
        if ($cycle -eq 1) {
            ClickTab 'Wallpapers'
            SetFiles @($Clip)
            Start-Sleep -Seconds 2
            $clicked = Eval "document.querySelector('button.gallery-tile[title*=""$clipName""]')?.click(); !!document.querySelector('button.gallery-tile[title*=""$clipName""]')"
            Write-Output "cycle 1 tile-clicked=$clicked"
            Start-Sleep -Seconds 3
        }
        $playing = Eval "(() => { const v = document.querySelector('video.wallpaper-media'); if (!v) return 'novideo'; return v.paused ? 'paused' : 'playing'; })()"
        $vids = Eval "document.querySelectorAll('video').length"
        if ($playing -eq 'paused' -and $cycle -eq 2) {
            [void](Eval "document.querySelector('button[aria-label=""Play""]')?.click(); true")
            Write-Output 'cycle 2 clicked Play'
            Start-Sleep -Seconds 2
            $playing = Eval "(() => { const v = document.querySelector('video.wallpaper-media'); if (!v) return 'novideo'; return v.paused ? 'paused' : 'playing'; })()"
        }
        Write-Output "cycle $cycle video=$playing vids=$vids"
        Start-Sleep -Milliseconds 2000
    }
    elseif ($Mode -eq 'Leak') {
        foreach ($t in @('Wallpapers', 'Home', 'Widgets', 'shadcn')) { ClickTab $t }
        [void](Eval "document.querySelector('.sidebar-toggle')?.click(); true")
        Start-Sleep -Milliseconds 600
        [void](Eval "document.querySelector('.sidebar-toggle')?.click(); true")
        Start-Sleep -Milliseconds 600
        [void](Eval "document.querySelector('[role=""switch""]')?.click(); true")
        Start-Sleep -Milliseconds 600
        if ($cycle % 5 -eq 0) { LogMark "LEAK cycle $cycle" }
        Start-Sleep -Milliseconds 23500
    }
    Write-Output "cycle $cycle done (elapsed $([math]::Round($sw.Elapsed.TotalMinutes, 1)) min)"
}

$ws.Dispose()
Write-Output "$Mode finished after $cycle cycles"