$ErrorActionPreference = 'Stop'
Stop-Process -Name electron,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
Remove-Item "$env:TEMP\rex-app.log" -ErrorAction SilentlyContinue
Start-Process -FilePath 'D:\REX_doc\app\node_modules\electron\dist\electron.exe' -ArgumentList '.', '--remote-debugging-port=9222' -WorkingDirectory 'D:\REX_doc\app'
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
Write-Output ('onNavigate exposed: ' + (E 'typeof window.rexDesktop.onNavigate === "function"'))
$ws.Dispose()
Stop-Process -Name electron,rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
