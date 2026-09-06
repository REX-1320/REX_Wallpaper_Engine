$ErrorActionPreference = 'Stop'
$targets = Invoke-RestMethod 'http://127.0.0.1:9222/json' -TimeoutSec 3
$page = $targets | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
if (-not $page) { Write-Output 'no page target'; exit 1 }
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$id = 100
function Eval([string]$expr) {
    $script:id += 1
    $msg = @{ id = $script:id; method = 'Runtime.evaluate'; params = @{ expression = $expr; returnByValue = $true } } | ConvertTo-Json -Compress -Depth 6
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
$r = Eval "(() => { const vs = [...document.querySelectorAll('video')]; return vs.map(v => ({ cls: v.className, src: (v.src||'').slice(0,60), paused: v.paused, ready: v.readyState, err: v.error ? v.error.code : null })); })()"
$r.result.result.value | ConvertTo-Json -Depth 4
$r2 = Eval "(() => { const b = [...document.querySelectorAll('button')].map(x => x.getAttribute('aria-label')).filter(Boolean); return { playBtn: b.filter(l => l === 'Play' || l === 'Pause'), tiles: document.querySelectorAll('button.gallery-tile').length }; })()"
$r2.result.result.value | ConvertTo-Json -Depth 4
$ws.Dispose()