# Paiint MSIX packaging

Builds the Microsoft Store package for **Paiint** (by MoiNeX). The Store route
is the $0 release path: Microsoft validates and signs the package, so we do not
buy a code-signing certificate.

## Why MSIX

- **Microsoft signs it.** No certificate, no SmartScreen "Unknown publisher"
  wall on install.
- **Full trust still owns the desktop.** The spike on 2026-10-25 proved the
  packaged runtime still parents into Explorer's `WorkerW` and renders. See
  `TODO.md` / `everything.md` for the evidence.
- **Sideloading is not the plan.** `Add-AppxPackage` of an unsigned package
  needs Developer Mode or admin, and a self-signed certificate if signed. The
  Store is the only path a stranger will complete.

## Prerequisites

| Need | Where |
|---|---|
| `makeappx.exe` / `signtool.exe` | `C:\Program Files (x86)\Windows Kits\10\App Certification Kit` (ships with the Windows SDK; no extra install on this machine) |
| Native runtime | `rex-core\build\Release\paiint-wallpaper-runtime.exe` — build with `cmake --build rex-core/build --config Release` |
| UI payload | `app\dist-exe\win-unpacked` — produced by `node app\scripts\package-win.cjs` |

`build-msix.ps1` finds `makeappx` in the App Certification Kit, then on `PATH`.
Use `-MakeAppxPath` to override.

## Build

```powershell
# full: rebuild the UI + portable exe, then pack
powershell -File packaging\msix\build-msix.ps1 -Publisher "CN=<Partner Center publisher>"

# reuse the existing app\dist-exe\win-unpacked
powershell -File packaging\msix\build-msix.ps1 -SkipBuild -Publisher "CN=<...>"

# stage + validate only (no makeappx)
powershell -File packaging\msix\build-msix.ps1 -SkipBuild -SkipPack
```

Output: `packaging\msix\out\Paiint-<version>-x64.msix` (git-ignored).

The script:

1. reads the version from `app/package.json` unless `-Version` is passed,
2. stages the payload into `out\stage`,
3. generates the seven tile assets from `app\build\icon.ico`,
4. expands `AppxManifest.xml` (`__VERSION__`, `__PUBLISHER__`),
5. validates the XML and every asset/executable it references,
6. packs with `makeappx`, prints the SHA-256.

## Store submission

1. **Publisher must match Partner Center.** Open the app in Partner Center →
   *Package/Identity/Publisher* and copy the exact `CN=...` value. The default
   `CN=MoiNeX` is a placeholder; a mismatch is rejected at upload.
2. Bump the version for **every** submission. MSIX versions are
   `Major.Minor.Build.Revision`; the Store requires **Revision = 0**.
   `0.1.0` → `0.1.0.0`; the next submission is `0.1.1.0` or `0.2.0.0`.
3. Upload the `.msix` in Partner Center → *Packages*. Microsoft runs the
   certification tests and signs it.
4. `runFullTrust` is declared in the manifest. The Store accepts it for
   desktop-bridge apps but reviews it; the app needs it because the Electron
   shell and the native wallpaper runtime are ordinary Win32 processes.

## Local sideload (testing only)

Sideloading an **unsigned** package requires Developer Mode or admin.
Signing it yourself requires a certificate whose subject matches the manifest
`Publisher` exactly:

```powershell
# once: create a self-signed cert for local tests (never ship this)
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=MoiNeX" `
  -KeyUsage DigitalSignature -FriendlyName "Paiint local test" `
  -CertStoreLocation "Cert:\CurrentUser\My" `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
$pwd = ConvertTo-SecureString -String "paiint" -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath paiint-test.pfx -Password $pwd
Export-Certificate -Cert $cert -FilePath paiint-test.cer

# trust it (CurrentUser\TrustedPeople is enough for sideload)
Import-Certificate -FilePath paiint-test.cer -CertStoreLocation Cert:\CurrentUser\TrustedPeople

# pack + sign
powershell -File packaging\msix\build-msix.ps1 -SkipBuild -Sign `
  -CertificatePath paiint-test.pfx -CertificatePassword $pwd

Add-AppxPackage packaging\msix\out\Paiint-0.1.0-x64.msix
```

Remove the test certificate from every store afterwards — the 2026-10-25 spike
did exactly that.

## Manifest notes

- `Identity Name="MoiNeX.Paiint"` — changing it creates a **different app** in
  the Store; keep it once the Partner Center reservation exists.
- `EntryPoint="Windows.FullTrustApplication"` + `rescap:runFullTrust` is the
  desktop-bridge combination the spike proved.
- `TargetDeviceFamily MinVersion="10.0.19041.0"` (Windows 10 2004) matches what
  Electron 43 supports.
- `ProcessorArchitecture="x64"` only. An arm64 package needs its own native
  runtime build; do not ship the x64 payload as arm64.
- The tile assets are generated from `app\build\icon.ico`, so replacing the
  logo and re-running the script updates the package automatically.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `makeappx.exe not found` | Windows SDK missing; pass `-MakeAppxPath` |
| `Payload is missing ...paiint-wallpaper-runtime.exe` | native runtime not built, or `app\dist-exe\win-unpacked` is stale — rebuild |
| `Manifest references missing assets` | an asset name in `AppxManifest.xml` does not match the generated set |
| Store rejects the upload with an identity mismatch | `-Publisher` does not match Partner Center exactly |
| `0x80073D2C` on `Add-AppxPackage` | Developer Mode off / package unsigned |
