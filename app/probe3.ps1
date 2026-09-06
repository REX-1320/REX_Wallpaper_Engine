$ErrorActionPreference = 'Stop'
$targets = Invoke-RestMethod 'http://127.0.0.1:9222/json' -TimeoutSec 3
$page = $targets | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
if (-not $page) { throw 'no page target' }
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$script:id = 200
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
    if ($r.result.exceptionDetails) { throw "eval failed: $($r.result.exceptionDetails.text)" }
    return $r.result.result.value
}
function SetFiles([string[]]$paths) {
    $doc = Invoke-Cdp 'DOM.getDocument' @{ depth = -1 }
    $q = Invoke-Cdp 'DOM.querySelector' @{ nodeId = $doc.result.root.nodeId; selector = 'input[type="file"]' }
    if ($q.result.nodeId -le 0) { throw 'file input not found' }
    $null = Invoke-Cdp 'DOM.setFileInputFiles' @{ nodeId = $q.result.nodeId; files = $paths }
}

Eval "document.querySelector('a.sidebar-link[title=""Wallpapers""]')?.click(); true" | Out-Null
Start-Sleep -Seconds 1
$h1 = Eval "document.querySelector('.wallpaper-grid .w2 .text-sm.font-semibold')?.textContent ?? 'none'"
Write-Output "BEFORE-LOAD header=$h1 title=$(Eval 'document.title')"
SetFiles @('C:\Program Files\Wallpaper Engine\live wallpaper\red-eyes-anime-girl-1-moewalls-com.mp4')
Start-Sleep -Seconds 3
$h2 = Eval "document.querySelector('.wallpaper-grid .w2 .text-sm.font-semibold')?.textContent ?? 'none'"
Write-Output "AFTER-LOAD  header=$h2 title=$(Eval 'document.title')"
Eval "document.querySelector('button.gallery-tile[title*=""red-eyes-anime-girl-1""]')?.click(); true" | Out-Null
Start-Sleep -Seconds 2
$h3 = Eval "document.querySelector('.wallpaper-grid .w2 .text-sm.font-semibold')?.textContent ?? 'none'"
Write-Output "AFTER-CLICK header=$h3 title=$(Eval 'document.title')"
Eval "[...document.querySelectorAll('button')].find(b => b.textContent.includes('Apply to Desktop'))?.click(); true" | Out-Null
Start-Sleep -Seconds 2
Write-Output "AFTER-APPLY title=$(Eval 'document.title')"
$ws.Dispose()
