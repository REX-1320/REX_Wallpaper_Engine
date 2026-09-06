$ErrorActionPreference = 'Stop'
$targets = Invoke-RestMethod 'http://127.0.0.1:9222/json' -TimeoutSec 3
$page = $targets | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
if (-not $page) { throw 'no page target' }
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$script:id = 300
function Invoke-Cdp([string]$method, $params = @{}) {
    $script:id += 1
    $msg = @{ id = $script:id; method = $method; params = $params } | ConvertTo-Json -Compress -Depth 6
    $bytes = [Text.Encoding]::UTF8.GetBytes($msg)
    [void]$ws.SendAsync([ArraySegment[byte]]::new($bytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    $sb = New-Object System.Text.StringBuilder
    while ($true) {
        $buf = New-Object byte[] 131072
        $res = $ws.ReceiveAsync([ArraySegment[byte]]::new($buf), [Threading.CancellationToken]::None).GetAwaiter().GetResult()
        [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $res.Count))
        if ($res.EndOfMessage) {
            $obj = $sb.ToString() | ConvertFrom-Json
            if ($obj.id -eq $script:id) { return $obj }
            $sb.Clear()
        }
    }
}
function Eval([string]$expr) {
    $r = Invoke-Cdp 'Runtime.evaluate' @{ expression = $expr; returnByValue = $true; awaitPromise = $true }
    if ($r.result.exceptionDetails) { throw "eval failed: $($r.result.exceptionDetails.text)" }
    return $r.result.result.value
}

# order check
$order = Eval "[...document.querySelectorAll('.titlebar-dot')].map(b => b.className.match(/dot (\w+)/)[1]).join('->')"
Write-Output "order=$order (want yellow->green->red)"

# maximize -> should become maximized with Restore icon
Eval "document.querySelector('.titlebar-dot.green')?.click(); true" | Out-Null
Start-Sleep -Milliseconds 800
$max1 = Eval "window.rexDesktop.isMaximized()"
$label1 = Eval "document.querySelector('.titlebar-dot.green')?.getAttribute('aria-label')"
Write-Output "after maximize click: isMaximized=$max1 label=$label1"

# maximize again -> restore
Eval "document.querySelector('.titlebar-dot.green')?.click(); true" | Out-Null
Start-Sleep -Milliseconds 800
$max2 = Eval "window.rexDesktop.isMaximized()"
$label2 = Eval "document.querySelector('.titlebar-dot.green')?.getAttribute('aria-label')"
Write-Output "after restore click:  isMaximized=$max2 label=$label2"

# minimize -> hidden + restore via green (maximize from minimized)
Eval "document.querySelector('.titlebar-dot.yellow')?.click(); true" | Out-Null
Start-Sleep -Milliseconds 1200
$vis = Eval "document.visibilityState"
Write-Output "after minimize: visibility=$vis"
Eval "document.querySelector('.titlebar-dot.green')?.click(); true" | Out-Null
Start-Sleep -Milliseconds 800
$vis2 = Eval "document.visibilityState"
$max3 = Eval "window.rexDesktop.isMaximized()"
Write-Output "after green restore: visibility=$vis2 isMaximized=$max3"

$ws.Dispose()
