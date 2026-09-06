$ErrorActionPreference = 'Stop'
$targets = Invoke-RestMethod 'http://127.0.0.1:9222/json' -TimeoutSec 3
Write-Output '--- targets ---'
$targets | ForEach-Object { Write-Output ("$($_.type) | $($_.title) | $($_.url)") }
$page = $targets | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$script:id = 100
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
    return $r.result.result.value
}
$r = Eval "(() => {
  document.querySelector('a.sidebar-link[title=""Home""]')?.click();
  return 'home';
})()"
Eval "document.querySelector('a.sidebar-link[title=""Wallpapers""]')?.click(); true" | Out-Null
Start-Sleep -Seconds 2
$header = Eval "document.querySelector('.wallpaper-grid .w2 .text-sm.font-semibold')?.textContent ?? 'none'"
Eval "[...document.querySelectorAll('button')].find(b => b.textContent.includes('Apply to Desktop'))?.click(); true" | Out-Null
Start-Sleep -Seconds 2
$r2 = Eval "(() => ({
  headerBefore: $($header | ConvertTo-Json),
  header: document.querySelector('.wallpaper-grid .w2 .text-sm.font-semibold')?.textContent ?? 'none',
  title: document.title,
}))()"
$r2 | ConvertTo-Json -Depth 4
$ws.Dispose()