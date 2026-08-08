@echo off
setlocal
cd /d "%~dp0"
set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if exist "%CLANG%" (
  set "KIT=C:\Program Files (x86)\Windows Kits\10"
  set "SDKVER="
  for /f "delims=" %%d in ('dir /b /ad "%KIT%\Include" 2^>nul') do set "SDKVER=%%d"
  if defined SDKVER (
    "%CLANG%" /nologo /O2 /W3 /TC /imsvc "%KIT%\Include\%SDKVER%\ucrt" /imsvc "%KIT%\Include\%SDKVER%\shared" /imsvc "%KIT%\Include\%SDKVER%\um" mica-cli.c /link /LIBPATH:"%KIT%\Lib\%SDKVER%\ucrt\x64" /LIBPATH:"%KIT%\Lib\%SDKVER%\um\x64" user32.lib dwmapi.lib /out:mica-cli.exe
    if %errorlevel%==0 (echo Built mica-cli.exe with clang-cl & exit /b 0)
    echo clang-cl failed, trying MSVC cl.exe...
  )
)
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%i"
if not defined VS (echo No toolchain found - install VS C++ workload or LLVM & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /W3 mica-cli.c /link user32.lib dwmapi.lib /out:mica-cli.exe
if %errorlevel%==0 (echo Built mica-cli.exe with cl) else (echo Build failed)
