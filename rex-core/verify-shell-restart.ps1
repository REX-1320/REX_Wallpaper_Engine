$ErrorActionPreference = 'Stop'
$exe = 'D:\REX_doc\rex-core\.build\Release\rex-wallpaper-runtime.exe'
$media = 'D:\REX_doc\bench-media\mpeg4_1280x720.mp4'
$runtimeLog = "$env:TEMP\REX\runtime.log"
$crashLog = "$env:TEMP\REX\crash.log"
$outFile = "$env:TEMP\rex_runtime_frames.bin"

Stop-Process -Name rex-wallpaper-runtime -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Remove-Item $runtimeLog, $crashLog, $outFile -ErrorAction SilentlyContinue

# ── spawn runtime: stdin via pipe, stdout redirected to file by cmd ───────
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = 'cmd.exe'
$psi.Arguments = "/c `"$exe --runtime > `"$outFile`" 2>&1`""
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.CreateNoWindow = $true
$psi.WorkingDirectory = 'D:\REX_doc\rex-core\.build\Release'
$p = [System.Diagnostics.Process]::Start($psi)

$script:fileOffset = 0
$script:frameBuffer = New-Object System.Collections.ArrayList

function Send-Frame([int]$id, [string]$cmd, [hashtable]$params) {
    $o = @{ type = 'command'; id = $id; cmd = $cmd }
    if ($params) { $o.params = $params }
    $json = $o | ConvertTo-Json -Compress -Depth 6
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $len = [BitConverter]::GetBytes([int]$bytes.Length)
    $p.StandardInput.BaseStream.Write($len, 0, 4)
    $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $p.StandardInput.BaseStream.Flush()
}

function Poll-Frames() {
    if (-not (Test-Path $outFile)) { return }
    $fs = [System.IO.File]::Open($outFile, 'Open', 'Read', 'ReadWrite')
    try {
        $fs.Seek($script:fileOffset, 'Begin') | Out-Null
        $buf = New-Object byte[] 1048576
        while ($true) {
            $n = $fs.Read($buf, 0, $buf.Length)
            if ($n -le 0) { break }
            $script:fileOffset += $n
            $data = New-Object byte[] $n
            [Array]::Copy($buf, 0, $data, 0, $n)
            $off = 0
            while ($off + 4 -le $data.Length) {
                $flen = [BitConverter]::ToInt32($data, $off)
                if ($flen -lt 1 -or $flen -gt 16777216) { break }
                if ($off + 4 + $flen -gt $data.Length) { break }
                $json = [Text.Encoding]::UTF8.GetString($data, $off + 4, $flen)
                try { $f = $json | ConvertFrom-Json } catch { $off += 4 + $flen; continue }
                $script:frameBuffer.Add($f) | Out-Null
                $off += 4 + $flen
            }
        }
    } finally { $fs.Dispose() }
}

function Next-Frame() {
    Poll-Frames
    if ($script:frameBuffer.Count -gt 0) {
        $f = $script:frameBuffer[0]
        $script:frameBuffer.RemoveAt(0)
        return $f
    }
    return $null
}

function Wait-Resp([int]$id, [int]$timeoutMs = 30000) {
    $deadline = [Environment]::TickCount + $timeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        $f = Next-Frame
        if ($f -and $f.type -eq 'response' -and $f.id -eq $id) { return $f }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for response $id"
}

function Wait-Event([string]$name, [int]$timeoutMs = 30000) {
    $deadline = [Environment]::TickCount + $timeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        $f = Next-Frame
        if ($f -and $f.type -eq 'event' -and $f.event -eq $name) { return $f }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for event $name"
}

function Get-LogLine([string]$match) {
    if (Test-Path $runtimeLog) {
        return (Get-Content $runtimeLog | Select-String $match | Select-Object -Last 1)
    }
    return $null
}

Write-Output '== 1. INIT =='
Send-Frame 1 'INIT' @{ applyToDesktop = $true; maxFps = 30 }
$r = Wait-Resp 1 60000
if (-not $r.ok) { throw "INIT failed: $($r.error)" }
Wait-Event 'READY' 60000
Write-Output 'READY ok'

Write-Output '== 2. LOAD + SET_WALLPAPER =='
Send-Frame 2 'LOAD' @{ path = $media; type = 'video'; loop = $true; muted = $true; mode = 'fill' }
$r = Wait-Resp 2 60000
if (-not $r.ok) { throw "LOAD failed: $($r.error)" }
Send-Frame 3 'SET_WALLPAPER' @{ path = $media }
$r = Wait-Resp 3 60000
if (-not $r.ok) { throw "SET_WALLPAPER failed: $($r.error)" }
Wait-Event 'WALLPAPER_APPLIED' 30000
Send-Frame 4 'PLAY' @{}
$r = Wait-Resp 4 30000
if (-not $r.ok) { throw "PLAY failed: $($r.error)" }
Wait-Event 'PLAYING' 30000
Write-Output 'PLAYING ok'

Start-Sleep -Seconds 5

# ── baseline: layer state before the shell restart ────────────────────────
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Win32 {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string w);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr c, string cn, string wn);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    public struct RECT { public int L, T, R, B; }
}
"@

# Progman detection via EnumWindows: FindWindow(progman, $null) marshals $null
# as "" (matches only title-less windows) — Progman's title is "Program Manager".
function Find-Progman {
    $h = [IntPtr]::Zero
    $cb = [Win32+EnumProc]{ param($w, $l)
        $sb = New-Object Text.StringBuilder 256
        [void][Win32]::GetClassName($w, $sb, 256)
        if ($sb.ToString() -eq 'Progman') { $script:h = $w; return $false }
        return $true }
    $script:h = [IntPtr]::Zero
    [void][Win32]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:h
}

function Get-RenderWindow() {
    $progman = Find-Progman
    if ($progman -eq [IntPtr]::Zero) { return $null }
    $script:renderHwnd = [IntPtr]::Zero
    $cb = [Win32+EnumProc]{ param($w, $l)
        $sb = New-Object Text.StringBuilder 256
        [void][Win32]::GetClassName($w, $sb, 256)
        if ($sb.ToString() -eq 'REXRenderWindow' -or $sb.ToString() -eq 'REXProgmanRender') {
            $script:renderHwnd = $w
            return $false
        }
        return $true }
    [void][Win32]::EnumChildWindows($progman, $cb, [IntPtr]::Zero)
    if ($script:renderHwnd -eq [IntPtr]::Zero) { return $null }
    return $script:renderHwnd
}

function Test-LayerBelowIcons() {
    $progman = Find-Progman
    if ($progman -eq [IntPtr]::Zero) { return @{ ok = $false; why = 'no Progman' } }
    $render = Get-RenderWindow
    if ($render -eq $null) { return @{ ok = $false; why = 'no render window' } }
    $parent = [Win32]::GetParent($render)
    $rect = New-Object Win32+RECT
    [Win32]::GetWindowRect($render, [ref]$rect) | Out-Null
    $w = $rect.R - $rect.L; $h = $rect.B - $rect.T
    $pRect = New-Object Win32+RECT
    [Win32]::GetWindowRect($progman, [ref]$pRect) | Out-Null
    $pw = $pRect.R - $pRect.L; $ph = $pRect.B - $pRect.T
    $visible = [Win32]::IsWindowVisible($render)
    if (-not $visible) { return @{ ok = $false; why = 'render window hidden' } }
    if ($w -lt ($pw * 0.9) -or $h -lt ($ph * 0.9)) {
        return @{ ok = $false; why = "render window too small ${w}x${h} vs Progman ${pw}x${ph}" }
    }
    return @{ ok = $true; why = "render ${w}x${h} parent=$parent visible" }
}

$base = Test-LayerBelowIcons
Write-Output ("baseline layer: " + $(if ($base.ok) { 'OK' } else { 'FAIL ' + $base.why }))

Write-Output '== 3. kill explorer (shell restart) =='
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 5
Write-Output ("explorer alive after kill: " + [bool](Get-Process -Name explorer -ErrorAction SilentlyContinue))

# wait for Explorer to come back and our monitor to detect the restart
$deadline = [Environment]::TickCount + 90000
$reacquired = $false
while ([Environment]::TickCount -lt $deadline) {
    Start-Sleep -Seconds 2
    $explorerBack = [bool](Get-Process -Name explorer -ErrorAction SilentlyContinue)
    $l = Get-LogLine 'Explorer restarted|reacquired'
    if ($explorerBack -and $l) {
        Write-Output ('detected: ' + ($l.Line -split '\] ')[1])
        $reacquired = $true
        break
    }
}
Write-Output ("reacquire detected: " + $reacquired)

Start-Sleep -Seconds 8

# ── post-restart state ─────────────────────────────────────────────────────
Write-Output '== 4. verify =='
$post = Test-LayerBelowIcons
Write-Output ("post-restart layer: " + $(if ($post.ok) { 'OK' } else { 'FAIL ' + $post.why }))

$logText = ''
if (Test-Path $runtimeLog) { $logText = Get-Content $runtimeLog -Raw }
$crashText = ''
if (Test-Path $crashLog) { $crashText = Get-Content $crashLog -Raw }

Write-Output ("windowRecreated=true logged : " + [bool]($logText -match 'windowRecreated=true'))
Write-Output ("no SetParent failures        : " + (-not [bool]($logText -match 'SetParent failed')))
Write-Output ("no watchdog hang dumps       : " + (-not [bool]($crashText -match 'WATCHDOG|dump')))
Write-Output ("runtime process alive        : " + [bool](Get-Process -Name rex-wallpaper-runtime -ErrorAction SilentlyContinue))
Write-Output ("no crash.log created         : " + (-not (Test-Path $crashLog)))

Write-Output ''
Write-Output '--- key runtime.log lines ---'
if (Test-Path $runtimeLog) {
    Get-Content $runtimeLog | Select-String 'WorkerW|Progman|reacquire|Explorer|watchdog|windowRecreated' |
        Select-Object -Last 30 | ForEach-Object { ($_.Line -split '\] ')[1] }
}

# cleanup
try { Send-Frame 99 'SHUTDOWN' @{} | Out-Null } catch { }
Start-Sleep -Seconds 3
$rt = Get-Process -Name rex-wallpaper-runtime -ErrorAction SilentlyContinue
if ($rt) { $rt | Stop-Process -Force }
if (-not $p.HasExited) { $p.Kill() }

$pass = $base.ok -and $post.ok -and $reacquired -and ($logText -match 'windowRecreated=true') -and
        (-not ($logText -match 'SetParent failed')) -and
        (-not ($crashText -match 'WATCHDOG|dump'))
Write-Output ''
Write-Output ("RESULT: " + $(if ($pass) { 'PASS' } else { 'FAIL' }))