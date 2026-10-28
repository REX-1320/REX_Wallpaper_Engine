#Requires -Version 5.1
<#
.SYNOPSIS
  Build the Paiint MSIX package for Microsoft Store submission.

.DESCRIPTION
  Stages the electron-builder output (app\dist-exe\win-unpacked), generates the
  tile assets from app\assets\paiint-app.ico, expands AppxManifest.xml, and packs the
  result with makeappx.exe.

  No certificate is needed for Store submission: Microsoft signs the package.
  Local sideloading DOES need a certificate (see README.md); use -Sign only if
  you have one.

.PARAMETER Version
  Semver or 4-part version. Default: version from app\package.json.

.PARAMETER Publisher
  The CN= value from Partner Center. Default CN=MoiNeX is a placeholder that
  only works for local sideload tests.

.PARAMETER IdentityName
  The Package/Identity/Name from Partner Center. Default MoiNeX.Paiint; an
  individual account often gets a numeric prefix (for example
  12345MoiNeX.Paiint) and the upload is rejected unless it matches exactly.

.PARAMETER PublisherDisplayName
  Partner Center's "Publisher display name" (the account/company name shown in
  the Store, not the in-app author string). Default MoiNeX.

.PARAMETER OutDir
  Output directory. Default: packaging\msix\out (git-ignored).

.PARAMETER PayloadDir
  electron-builder unpacked folder. Default: app\dist-exe\win-unpacked.

.PARAMETER SkipBuild
  Reuse the existing payload instead of rebuilding the UI and the exe.

.PARAMETER SkipPack
  Stage and validate only; do not run makeappx.

.PARAMETER Sign
  Sign the MSIX with -CertificatePath (local testing only).

.EXAMPLE
  pwsh packaging\msix\build-msix.ps1 -Publisher "CN=1A2B3C4D-0000-0000-0000-000000000000"

.EXAMPLE
  powershell -File packaging\msix\build-msix.ps1 -SkipBuild -SkipPack
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$Publisher = 'CN=MoiNeX',
    [string]$IdentityName = 'MoiNeX.Paiint',
    [string]$PublisherDisplayName = 'MoiNeX',
    [string]$OutDir,
    [string]$PayloadDir,
    [string]$MakeAppxPath,
    [string]$SignToolPath,
    [switch]$SkipBuild,
    [switch]$SkipPack,
    [switch]$Sign,
    [string]$CertificatePath,
    [securestring]$CertificatePassword
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path (Join-Path $repoRoot 'app\package.json'))) {
    throw "Cannot find the repo root from '$PSScriptRoot' (expected app\package.json under $repoRoot)."
}

function Get-ToolPath {
    param([string]$Explicit, [string]$Name)

    $candidates = New-Object System.Collections.Generic.List[string]
    if ($Explicit) { $candidates.Add($Explicit) }
    $kit = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\App Certification Kit'
    $candidates.Add((Join-Path $kit $Name))
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { $candidates.Add($onPath.Source) }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) { return $candidate }
    }
    return $null
}

function ConvertTo-MsixVersion {
    param([string]$Value)

    $clean = ($Value -split '-')[0].Trim()
    if (-not $clean) { throw 'Version is empty.' }
    $parts = @($clean.Split('.'))
    if ($parts.Count -gt 4) { throw "Version '$Value' has more than 4 parts." }
    while ($parts.Count -lt 4) { $parts += '0' }

    $numbers = @()
    foreach ($part in $parts) {
        $n = 0
        if (-not [int]::TryParse($part, [ref]$n)) {
            throw "Version part '$part' in '$Value' is not a number."
        }
        if ($n -lt 0 -or $n -gt 65535) {
            throw "Version part '$n' in '$Value' is outside 0..65535 (MSIX limit)."
        }
        $numbers += $n
    }
    if ($numbers[3] -ne 0) {
        Write-Warning 'MSIX revision (4th part) is not 0. The Microsoft Store requires revision 0; use it only for local sideload tests.'
    }
    return ($numbers -join '.')
}

# ─── Version ─────────────────────────────────────────────────────────────────
$packageJsonPath = Join-Path $repoRoot 'app\package.json'
if (-not $Version) {
    $packageJson = Get-Content $packageJsonPath -Raw | ConvertFrom-Json
    $Version = [string]$packageJson.version
}
$msixVersion = ConvertTo-MsixVersion $Version
Write-Host "Paiint MSIX $msixVersion (from '$Version')"

if ($Publisher -eq 'CN=MoiNeX') {
    Write-Warning 'Using the placeholder publisher CN=MoiNeX. Replace it with the Publisher value from Partner Center before submitting to the Store.'
}
if ($Publisher -notmatch '^CN=.+') {
    throw "Publisher '$Publisher' must start with 'CN='."
}

# ─── Build (optional) ────────────────────────────────────────────────────────
if (-not $SkipBuild) {
    Write-Host 'Building the UI and the portable app...'
    Push-Location $repoRoot
    try {
        & pnpm --dir app build
        if ($LASTEXITCODE -ne 0) { throw 'pnpm --dir app build failed.' }
        & node 'app\scripts\package-win.cjs'
        if ($LASTEXITCODE -ne 0) { throw 'app\scripts\package-win.cjs failed.' }
    }
    finally {
        Pop-Location
    }
}

# ─── Payload ─────────────────────────────────────────────────────────────────
if (-not $PayloadDir) { $PayloadDir = Join-Path $repoRoot 'app\dist-exe\win-unpacked' }
if (-not (Test-Path $PayloadDir)) {
    throw "Payload folder '$PayloadDir' does not exist. Build it first (drop -SkipBuild) or pass -PayloadDir."
}
$payloadExe = Join-Path $PayloadDir 'Paiint.exe'
if (-not (Test-Path $payloadExe)) {
    throw "Payload is missing Paiint.exe: '$payloadExe'."
}
$runtimeExe = Join-Path $PayloadDir 'resources\bin\paiint-wallpaper-runtime.exe'
if (-not (Test-Path $runtimeExe)) {
    throw "Payload is missing the native runtime: '$runtimeExe'. Build rex-core first (cmake --build rex-core/build --config Release)."
}
$startupStub = Join-Path $PayloadDir 'resources\bin\paiint-startup.exe'
if (-not (Test-Path $startupStub)) {
    throw "Payload is missing the startup-task stub: '$startupStub'. Build rex-core first (cmake --build rex-core/build --config Release)."
}

# ─── Stage ───────────────────────────────────────────────────────────────────
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot 'out' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$stage = Join-Path $OutDir 'stage'
if (Test-Path $stage) { Remove-Item -Path $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Write-Host "Staging $PayloadDir -> $stage"
Copy-Item -Path (Join-Path $PayloadDir '*') -Destination $stage -Recurse -Force

# ─── Tile assets ─────────────────────────────────────────────────────────────
Add-Type -AssemblyName System.Drawing
$iconPath = Join-Path $repoRoot 'app\assets\paiint-app.ico'
if (-not (Test-Path $iconPath)) { throw "Icon not found: '$iconPath'." }

# The shipped .ico is PNG-compressed (see app\scripts\make-tray-icon.ps1), and
# System.Drawing.Icon's size constructor fails on it. Read the directory and
# decode the largest frame directly instead.
function Get-LargestIconBitmap {
    param([string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 22) { throw "Icon is too small: $Path" }
    $count = [BitConverter]::ToUInt16($bytes, 4)
    if ($count -lt 1) { throw "Icon has no images: $Path" }

    $best = $null
    for ($i = 0; $i -lt $count; $i++) {
        $entry = 6 + ($i * 16)
        $width = [int]$bytes[$entry]
        if ($width -eq 0) { $width = 256 }
        $length = [BitConverter]::ToUInt32($bytes, $entry + 8)
        $offset = [BitConverter]::ToUInt32($bytes, $entry + 12)
        if (($null -eq $best) -or ($width -gt $best.Width)) {
            $best = @{ Width = $width; Length = $length; Offset = $offset }
        }
    }

    $data = New-Object byte[] ([int]$best.Length)
    [Array]::Copy($bytes, [int]$best.Offset, $data, 0, [int]$best.Length)

    # PNG signature 0x89 'P' 'N' 'G'
    if (($data.Length -gt 8) -and ($data[0] -eq 0x89) -and ($data[1] -eq 0x50) -and ($data[2] -eq 0x4E) -and ($data[3] -eq 0x47)) {
        $stream = [System.IO.MemoryStream]::new($data)
        try {
            $image = [System.Drawing.Image]::FromStream($stream)
            try { return (New-Object System.Drawing.Bitmap($image)) }
            finally { $image.Dispose() }
        }
        finally { $stream.Dispose() }
    }

    $icon = New-Object -TypeName System.Drawing.Icon -ArgumentList @($Path, $best.Width, $best.Width)
    try { return $icon.ToBitmap() } finally { $icon.Dispose() }
}

$assetsDir = Join-Path $stage 'Assets'
New-Item -ItemType Directory -Path $assetsDir -Force | Out-Null

$source = Get-LargestIconBitmap -Path $iconPath
try {
    $sizes = @(
        @{ Name = 'StoreLogo.png';          Width = 50;  Height = 50  },
        @{ Name = 'Square44x44Logo.png';    Width = 44;  Height = 44  },
        @{ Name = 'Square150x150Logo.png';  Width = 150; Height = 150 },
        @{ Name = 'SmallTile.png';          Width = 71;  Height = 71  },
        @{ Name = 'Square310x310Logo.png';  Width = 310; Height = 310 },
        @{ Name = 'Wide310x150Logo.png';    Width = 310; Height = 150 },
        @{ Name = 'SplashScreen.png';       Width = 620; Height = 300 }
    )

    foreach ($size in $sizes) {
        $bitmap = New-Object System.Drawing.Bitmap($size.Width, $size.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $graphics.Clear([System.Drawing.Color]::Transparent)

            $scale = [Math]::Min($size.Width / $source.Width, $size.Height / $source.Height)
            $drawWidth = [int][Math]::Round($source.Width * $scale)
            $drawHeight = [int][Math]::Round($source.Height * $scale)
            $x = [int](($size.Width - $drawWidth) / 2)
            $y = [int](($size.Height - $drawHeight) / 2)
            $graphics.DrawImage($source, $x, $y, $drawWidth, $drawHeight)
        }
        finally {
            $graphics.Dispose()
        }
        $target = Join-Path $assetsDir $size.Name
        $bitmap.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
        $bitmap.Dispose()
    }
}
finally {
    $source.Dispose()
}
Write-Host ("Generated {0} tile assets" -f 7)

# ─── Manifest ────────────────────────────────────────────────────────────────
$templatePath = Join-Path $PSScriptRoot 'AppxManifest.xml'
$template = Get-Content $templatePath -Raw
$manifest = $template.Replace('__VERSION__', $msixVersion)
$manifest = $manifest.Replace('__PUBLISHER__', $Publisher)
$manifest = $manifest.Replace('__IDENTITY_NAME__', $IdentityName)
$manifest = $manifest.Replace('__PUBLISHER_DISPLAY_NAME__', $PublisherDisplayName)
$manifestPath = Join-Path $stage 'AppxManifest.xml'
[System.IO.File]::WriteAllText($manifestPath, $manifest, (New-Object System.Text.UTF8Encoding($false)))

# Validate the manifest and every path it references before packing.
$xml = New-Object System.Xml.XmlDocument
$xml.PreserveWhitespace = $true
$xml.Load($manifestPath)
$ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$ns.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
$ns.AddNamespace('uap', 'http://schemas.microsoft.com/appx/manifest/uap/windows10')
$ns.AddNamespace('desktop', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10')

$identity = $xml.SelectSingleNode('/f:Package/f:Identity', $ns)
if (-not $identity) { throw 'Manifest has no Identity element.' }
if ($identity.Version -ne $msixVersion) { throw "Manifest version '$($identity.Version)' != '$msixVersion'." }
if ($identity.Publisher -ne $Publisher) { throw "Manifest publisher '$($identity.Publisher)' != '$Publisher'." }
if ($identity.Name -ne $IdentityName) { throw "Manifest identity name '$($identity.Name)' != '$IdentityName'." }
$publisherDisplay = $xml.SelectSingleNode('/f:Package/f:Properties/f:PublisherDisplayName', $ns)
if (-not $publisherDisplay) { throw 'Manifest has no PublisherDisplayName.' }
if ($publisherDisplay.InnerText -ne $PublisherDisplayName) {
    throw "Manifest publisher display name '$($publisherDisplay.InnerText)' != '$PublisherDisplayName'."
}

$referenced = @()
$referenced += $xml.SelectSingleNode('/f:Package/f:Properties/f:Logo', $ns).InnerText
foreach ($node in $xml.SelectNodes('//@*')) {
    $value = $node.Value
    if ($value -match '\.png$') { $referenced += $value }
}
$missing = @()
foreach ($rel in ($referenced | Sort-Object -Unique)) {
    $full = Join-Path $stage ($rel -replace '\\', '\')
    if (-not (Test-Path $full)) { $missing += $rel }
}
if ($missing.Count -gt 0) { throw ("Manifest references missing assets: " + ($missing -join ', ')) }

$executable = $xml.SelectSingleNode('/f:Package/f:Applications/f:Application/@Executable', $ns).Value
if (-not (Test-Path (Join-Path $stage $executable))) { throw "Manifest executable '$executable' is not in the package." }

# Extension executables are separate processes Windows launches (the startup
# stub at logon). A missing one is a silent no-op, so fail the build instead.
foreach ($node in $xml.SelectNodes('//f:Application/f:Extensions/desktop:Extension/@Executable', $ns)) {
    $extExe = $node.Value
    if (-not (Test-Path (Join-Path $stage $extExe))) { throw "Extension executable '$extExe' is not in the package." }
}

Write-Host 'Manifest and assets validated.'

if ($SkipPack) {
    Write-Host "Staged package ready: $stage"
    return
}

# ─── Pack ────────────────────────────────────────────────────────────────────
$makeappx = Get-ToolPath -Explicit $MakeAppxPath -Name 'makeappx.exe'
if (-not $makeappx) {
    throw 'makeappx.exe not found. Install the Windows SDK, or pass -MakeAppxPath. (It also ships in "C:\Program Files (x86)\Windows Kits\10\App Certification Kit".)'
}

$msixPath = Join-Path $OutDir ("Paiint-{0}-x64.msix" -f $Version)
if (Test-Path $msixPath) { Remove-Item $msixPath -Force }
Write-Host "Packing with $makeappx"
& $makeappx pack /d $stage /p $msixPath /o
if ($LASTEXITCODE -ne 0) { throw "makeappx failed with exit code $LASTEXITCODE." }

$hash = Get-FileHash -Path $msixPath -Algorithm SHA256
$sizeMb = [Math]::Round((Get-Item $msixPath).Length / 1MB, 1)
Write-Host ''
Write-Host "MSIX:   $msixPath"
Write-Host "Size:   $sizeMb MB"
Write-Host "SHA256: $($hash.Hash)"

# ─── Sign (local sideload only) ──────────────────────────────────────────────
if ($Sign) {
    if (-not $CertificatePath) { throw '-Sign needs -CertificatePath (a .pfx). The Store does not need this.' }
    if (-not (Test-Path $CertificatePath)) { throw "Certificate not found: '$CertificatePath'." }
    $signtool = Get-ToolPath -Explicit $SignToolPath -Name 'signtool.exe'
    if (-not $signtool) { throw 'signtool.exe not found; install the Windows SDK or pass -SignToolPath.' }

    $signArgs = @('sign', '/fd', 'SHA256', '/f', $CertificatePath)
    if ($CertificatePassword) {
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($CertificatePassword)
        try {
            $plain = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
            $signArgs += @('/p', $plain)
        }
        finally {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) | Out-Null
        }
    }
    $signArgs += $msixPath
    & $signtool @signArgs
    if ($LASTEXITCODE -ne 0) { throw "signtool failed with exit code $LASTEXITCODE." }
    Write-Host 'Signed for local sideloading.'
}
else {
    Write-Host 'Not signed. This is correct for the Store (Microsoft signs it); local sideloading needs -Sign.'
}

# Explicit success: some of the tools above (and pnpm's stderr warnings) can
# leave a non-zero $LASTEXITCODE behind even when every step succeeded, which
# would make a CI job treat a good package as a failed build.
exit 0
