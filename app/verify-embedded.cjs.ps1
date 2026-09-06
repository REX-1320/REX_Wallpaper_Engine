$ErrorActionPreference = 'Stop'

$temp = Join-Path $env:TEMP 'rex-electron-release'
$win = Join-Path $temp 'win-unpacked'
$asarPath = Join-Path $win 'resources\app.asar'
$freshExe = 'D:\REX_doc\rex-core\.build\Release\rex-wallpaper-runtime.exe'

if (-not (Test-Path $asarPath)) {
    Write-Output "asar missing: $asarPath"
    exit 1
}

function Read-AsarFile([string]$asarPath, [string]$wanted) {
    $fs = [System.IO.File]::OpenRead($asarPath)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $null = $br.ReadUInt32()  # pickle prefix
        $null = $br.ReadUInt32()  # json pickle size
        $null = $br.ReadUInt32()  # json string size + 4
        $jsonLen = $br.ReadUInt32()  # json string size
        $jsonBytes = $br.ReadBytes($jsonLen)
        $json = [System.Text.Encoding]::UTF8.GetString($jsonBytes) | ConvertFrom-Json
        $entry = $json.files
        foreach ($part in ($wanted -split '/')) {
            $entry = $entry.files.$part
        }
        $offset = [int64]$entry.offset
        $size = [int64]$entry.size
        $fs.Seek($offset, [System.IO.SeekOrigin]::Begin) | Out-Null
        $stored = New-Object byte[] $size
        $read = 0
        while ($read -lt $size) {
            $n = $fs.Read($stored, $read, [int]($size - $read))
            if ($n -eq 0) { throw 'short read' }
            $read += $n
        }
        if ($stored[0] -eq 0x78 -and ($stored[1] -eq 0x9C -or $stored[1] -eq 0x01 -or $stored[1] -eq 0xDA)) {
            $raw = New-Object byte[] ($stored.Length - 6)
            [Array]::Copy($stored, 2, $raw, 0, $raw.Length)
            $ms = New-Object System.IO.MemoryStream(,$raw)
            $ds = New-Object System.IO.Compression.DeflateStream($ms, [System.IO.Compression.CompressionMode]::Decompress)
            $out = New-Object System.IO.MemoryStream
            $ds.CopyTo($out)
            $ds.Dispose()
            return $out.ToArray()
        }
        return $stored
    } finally {
        $fs.Dispose()
    }
}

$embedded = Read-AsarFile $asarPath 'bin/rex-wallpaper-runtime.exe'
$sha = [System.Security.Cryptography.SHA256]::Create()
$embeddedHash = [BitConverter]::ToString($sha.ComputeHash([byte[]]$embedded)).Replace('-', '')
$freshHash = [BitConverter]::ToString($sha.ComputeHash([byte[]][System.IO.File]::ReadAllBytes($freshExe))).Replace('-', '')

Write-Output "embedded runtime: $($embedded.Length) bytes"
Write-Output "embedded hash:    $embeddedHash"
Write-Output "fresh build hash: $freshHash"
Write-Output "match:            $($embeddedHash -eq $freshHash)"