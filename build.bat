@echo off
rem One-click build: double-click this file. Output (installer, portable zip) goes to .\dist
setlocal
cd /d "%~dp0"
set "APPVERSION=1.0.0"
title PS3 Eye driver build

rem ---- 1. find the Visual Studio C++ compiler (install it if missing) ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
call :findvs
if defined VSDIR goto havevs
echo.
echo The Visual Studio C++ build tools are not installed (free, about 3 GB).
choice /m "Install them now"
if errorlevel 2 exit /b 1
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
call :findvs
if not defined VSDIR (echo Build tools install failed. & pause & exit /b 1)
:havevs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || (echo Could not load the compiler. & pause & exit /b 1)

rem ---- 2. git + vcpkg (our own copy, so no extra setup is needed) ----
where git >nul 2>nul || winget install --id Git.Git -e --accept-package-agreements --accept-source-agreements
where git >nul 2>nul || set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
set "VCPKG_ROOT=%~dp0vcpkg"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo Downloading vcpkg...
    git clone --depth 1 https://github.com/microsoft/vcpkg "%VCPKG_ROOT%" || (pause & exit /b 1)
    call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics || (pause & exit /b 1)
)

rem ---- 3. build (first run takes ~10 minutes: libusb, WIL and C++/WinRT get compiled) ----
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static || goto failed
cmake --build build --config Release || goto failed
ctest --test-dir build -C Release --output-on-failure || goto failed
rem 32-bit DirectShow filter, so 32-bit apps on Windows 10 see the camera too (no vcpkg needed)
cmake -S . -B build32 -A Win32 -DPS3EYE_FILTER_ONLY=ON || goto failed
cmake --build build32 --config Release || goto failed

if not exist dist mkdir dist
copy /y build\Release\PS3EyeVCam.dll dist\ >nul
copy /y build\Release\PS3EyeSetup.exe dist\ >nul
copy /y build\Release\PS3EyeServer.exe dist\ >nul
copy /y build\Release\PS3EyeDShow64.dll dist\ >nul
copy /y build32\Release\PS3EyeDShow32.dll dist\ >nul
copy /y README.md dist\ >nul

rem ---- 4. installer wizard (Inno Setup, free) ----
set "NOINSTALLER="
call :findinno
if defined ISCC goto haveinno
where winget >nul 2>nul && (
    echo Installing Inno Setup to build the installer wizard...
    winget install --id JRSoftware.InnoSetup -e --accept-package-agreements --accept-source-agreements
)
call :findinno
if defined ISCC goto haveinno
set "NOINSTALLER=1"
goto zip
:haveinno
"%ISCC%" /Q /DAppVersion=%APPVERSION% "/DDistDir=%~dp0dist" installer\PS3EyeCamera.iss || goto failed

:zip
rem portable zip too
powershell -NoProfile -Command "Compress-Archive -Force -Path dist\PS3EyeSetup.exe,dist\PS3EyeVCam.dll,dist\PS3EyeServer.exe,dist\PS3EyeDShow64.dll,dist\PS3EyeDShow32.dll,dist\README.md -DestinationPath dist\PS3EyeCamera-%APPVERSION%-portable.zip" || goto failed

echo.
echo BUILD OK. Files in dist\:
dir /b dist
echo.
if defined NOINSTALLER (
    echo Inno Setup isn't installed, so there's no installer wizard this time, only the portable zip.
    echo Install Inno Setup 6 or 7 from https://jrsoftware.org/isdl.php and run build.bat again.
) else (
    echo Installer: dist\PS3EyeCamera-Setup-%APPVERSION%.exe   Portable: dist\PS3EyeCamera-%APPVERSION%-portable.zip
)
explorer "%~dp0dist"
pause
exit /b 0

:failed
echo.
echo BUILD FAILED. Copy the red error lines above and send them to Claude.
pause
exit /b 1

:findvs
set "VSDIR="
if not exist "%VSWHERE%" exit /b 0
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
exit /b 0

:findinno
set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 7\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 7\ISCC.exe"
if exist "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe"
exit /b 0
