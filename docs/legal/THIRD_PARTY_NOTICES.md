# Third-party notices — Paiint 0.1.0

Paiint is © 2026 MoiNeX. The Paiint application, its native wallpaper engine
(`rex-core`) and its user interface are proprietary software distributed under
the Paiint End User Licence Agreement (`docs/legal/EULA.md`). They are **not**
open source.

Paiint is built on the third-party components listed below. Each remains under
its own licence, and nothing in the Paiint licence restricts your rights under
those licences.

---

## FFmpeg 8.1.2 — GNU Lesser General Public License 2.1 or later

The wallpaper engine decodes video through FFmpeg libraries shipped as four
separate DLLs beside the runtime:

| File | Library |
|---|---|
| `resources/bin/avcodec-62.dll` | libavcodec 62.28.102 |
| `resources/bin/avformat-62.dll` | libavformat |
| `resources/bin/avutil-60.dll` | libavutil |
| `resources/bin/swscale-9.dll` | libswscale |

**Licence.** FFmpeg is licensed under the GNU Lesser General Public License,
version 2.1 or later. A copy of that licence is included with Paiint as
`legal/LGPL-2.1.txt`, and is also available at
<https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>.

**This build is LGPL, not GPL.** The libraries were compiled with the following
relevant configuration (taken from the binary's own configuration string):
`--enable-shared --disable-static`, and with **no** `--enable-gpl`, **no**
`--enable-nonfree`, **no** `--enable-version3`, and every external codec library
disabled (`--disable-libx264`, `--disable-libx265`, `--disable-libfdk-aac`,
`--disable-libmp3lame`, `--disable-libvpl`, `--disable-libdav1d`, …). Only
`avcodec`, `avformat`, `swscale` and `avutil` are built.

**How Paiint complies with LGPL-2.1 §6.** The FFmpeg libraries are linked
*dynamically* and are **not** embedded in the executable. They are ordinary
DLLs on disk, loaded at run time from the folder next to
`paiint-wallpaper-runtime.exe`. You may replace any of them with a modified,
interface-compatible build of your own: drop your DLLs into that folder,
keeping the file names, and Paiint will load them. No recompilation of Paiint
is required, and Paiint does not verify their signatures or checksums.

**Source code.** The exact FFmpeg source for this build is available from the
FFmpeg project at <https://ffmpeg.org/download.html> (release 8.1.2). The build
uses the vcpkg `ffmpeg` port with the `avcodec`, `avformat` and `swscale`
features and default features disabled; the configuration string quoted above
is the complete list of build switches.

---

## Electron 43.2.0 — MIT License

The Paiint desktop application is built on Electron.

```
Copyright (c) Electron contributors
Copyright (c) 2013-2020 GitHub Inc.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Chromium — BSD 3-Clause License and others

Electron embeds Chromium. Chromium's copyright notices and the full set of
licences for the code it contains ship with Electron as
`LICENSES.chromium.html` in the Electron distribution. The primary licence is
the BSD 3-Clause:

```
Copyright 2015 The Chromium Authors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
   * Neither the name of Google Inc. nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## spdlog 1.17.0 — MIT License

```
Copyright (c) 2016 Gabi Melman <gabi.melman@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## {fmt} 12.2.0 — MIT License

```
Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

---

## JavaScript libraries

The Paiint interface is bundled from the npm packages declared in
`app/package.json` (React 19, Radix UI, Base UI, Tailwind CSS, motion, zod,
recharts, and their transitive dependencies). Each is distributed under its own
licence and retains its own copyright.

**188 production packages** are covered: 160 MIT, 14 ISC, 3 BSD-3-Clause,
2 MPL-2.0, 2 Apache-2.0, 1 MIT AND ISC, 1 0BSD and 1 Unlicense.

The verbatim licence text of every package — reproduced from the published
tarball, or the canonical text of the declared licence when a package published
no licence file — is in **`THIRD_PARTY_LICENSES_JS.md`**, shipped beside this
notice. It is generated from the real dependency tree and can be re-checked at
any time:

```powershell
node app/tools/collect-js-licenses.cjs          # regenerate
node app/tools/collect-js-licenses.cjs --check  # fail if out of date
```

Electron's own licence (`LICENSE.electron.txt`) and Chromium's full notice file
(`LICENSES.chromium.html`) ship in the application folder next to `Paiint.exe`.

---

## Microsoft components

Paiint uses Windows APIs (D3D12, Media Foundation, WASAPI, Win32) provided by
the operating system under the Windows licence. The Microsoft Store distributes
the package and collects its own telemetry under the
[Microsoft Privacy Statement](https://privacy.microsoft.com/privacystatement).
