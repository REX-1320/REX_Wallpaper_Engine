$ErrorActionPreference = 'Stop'
$targets = Invoke-RestMethod 'http://127.0.0.1:9222/json' -TimeoutSec 3
$page = $targets | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
if (-not $page) { throw 'no page target' }
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$script:id = 900
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
    $r = Invoke-Cdp 'Runtime.evaluate' @{ expression = $expr; returnByValue = $true }
    if ($r.result.exceptionDetails) { return "EVAL-ERR: $($r.result.exceptionDetails.text)" }
    return $r.result.result.value
}
function SetFiles([string[]]$paths) {
    $doc = Invoke-Cdp 'DOM.getDocument' @{ depth = -1 }
    $q = Invoke-Cdp 'DOM.querySelector' @{ nodeId = $doc.result.root.nodeId; selector = 'input[type="file"]' }
    if ($q.result.nodeId -le 0) { throw 'file input not found' }
    $null = Invoke-Cdp 'DOM.setFileInputFiles' @{ nodeId = $q.result.nodeId; files = $paths }
}

Eval "window.__errs = []; window.addEventListener('error', e => __errs.push('ERR: ' + e.message)); window.addEventListener('unhandledrejection', e => __errs.push('REJ: ' + String(e.reason))); true" | Out-Null
Eval "document.querySelector('a.sidebar-link[title=""Wallpapers""]')?.click(); true" | Out-Null
Start-Sleep -Seconds 1
$files = Get-ChildItem 'C:\Program Files\Wallpaper Engine\live wallpaper' -Filter *.mp4 -File | Select-Object -ExpandProperty FullName
Write-Output "importing $($files.Count)"
SetFiles $files

$deadline = (Get-Date).AddSeconds(180)
$crashed = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 8
    $rootLen = Eval "document.querySelector('#root')?.innerHTML.length ?? -1"
    $h2 = Eval "document.querySelector('h2')?.textContent ?? 'none'"
    $tiles = Eval "document.querySelectorAll('.gallery-tile').length"
    $imgs = Eval "document.querySelectorAll('.gallery-tile img').length"
    $alive = Eval '!!document.body'
    Write-Output ("poll rootLen=$rootLen h2=$h2 tiles=$tiles imgs=$imgs alive=$alive")
    if ([int]$rootLen -lt 1000) { $crashed = $true; Write-Output 'WHITE SCREEN DETECTED'; break }
    if ([int]$tiles -eq $files.Count -and [int]$imgs -eq [int]$tiles) { Write-Output 'ALL THUMBNAILED'; break }
}
Write-Output ("ERRS===" + (Eval 'window.__errs.join(" | ")'))
Write-Output "crashed=$crashed"
$ws.Dispose()
