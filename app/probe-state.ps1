$t = Invoke-RestMethod 'http://127.0.0.1:9222/json'
$page = $t | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
$i = 0
function E([string]$e) {
    $global:i++
    $m = @{ id = $global:i; method = 'Runtime.evaluate'; params = @{ expression = $e; returnByValue = $true } } | ConvertTo-Json -Compress -Depth 5
    $b = [Text.Encoding]::UTF8.GetBytes($m)
    [void]$ws.SendAsync([ArraySegment[byte]]::new($b), [Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    $sb = New-Object Text.StringBuilder
    while (1) {
        $buf = New-Object byte[] 65536
        $r = $ws.ReceiveAsync([ArraySegment[byte]]::new($buf), [Threading.CancellationToken]::None).GetAwaiter().GetResult()
        [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $r.Count))
        if ($r.EndOfMessage) {
            $o = $sb.ToString() | ConvertFrom-Json
            if ($o.id -eq $global:i) { return $o.result.result.value }
        }
    }
}
$r = E "document.visibilityState + ' | hidden=' + document.hidden + ' | ' + window.outerWidth + 'x' + window.outerHeight + ' | focus=' + document.hasFocus()"
Write-Output "renderer state: $r"
$ws.Dispose()
