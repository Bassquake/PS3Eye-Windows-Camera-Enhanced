@echo off
rem One-click build: double-click this file. Output (installer, portable zip) goes to .\dist
setlocal
cd /d "%~dp0"
set "APPVERSION=1.1.0"
title PS3 Eye driver build

rem ---- 1. find the Visual Studio C++ compiler (install it if missing) ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
call :findvs
if defined VSDIR goto havevs
echo.
echo The Visual Studio C++ build tools are not installed (free, about 3 GB).
choice /m "Install them now"
if errorlevel 2 exit /b 1
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --includeRecommended"
call :findvs
if not defined VSDIR (echo Build tools install failed. & pause & exit /b 1)
:havevs
rem the ARM64 compiler, for Windows on ARM PCs (Snapdragon laptops etc.)
call :findarm
if defined HAVEARM goto havearm
echo.
echo Adding the ARM64 C++ build tools to Visual Studio (for Windows on ARM PCs).
echo Close Visual Studio first if it's open; Windows will ask for permission.
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "%VSDIR%" --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --passive --norestart --wait
call :findarm
if not defined HAVEARM (echo Adding the ARM64 build tools failed. Close Visual Studio and run build.bat again. & pause & exit /b 1)
:havearm
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
rem x86, for 32-bit Windows 10. Its DirectShow filter also goes in the x64 and ARM64 sets, so
rem 32-bit apps on 64-bit Windows see the camera too.
cmake -S . -B buildx86 -A Win32 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x86-windows-static || goto failed
cmake --build buildx86 --config Release || goto failed
ctest --test-dir buildx86 -C Release --output-on-failure || goto failed
rem ARM64, for Windows on ARM: the Windows camera service there is native ARM64 and can't load
rem the x64 DLL. Its unit tests can't run on an x64 PC, so they're not built.
cmake -S . -B buildarm64 -A ARM64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=arm64-windows-static -DPS3EYE_BUILD_TESTS=OFF || goto failed
cmake --build buildarm64 --config Release || goto failed

rem x64 set, plus the 32-bit filter for 32-bit apps
if not exist dist\x64 mkdir dist\x64
copy /y build\Release\PS3EyeVCam.dll dist\x64\ >nul
copy /y build\Release\PS3EyeSetup.exe dist\x64\ >nul
copy /y build\Release\PS3EyeServer.exe dist\x64\ >nul
copy /y build\Release\PS3EyeDShow64.dll dist\x64\ >nul
copy /y buildx86\Release\PS3EyeDShow32.dll dist\x64\ >nul
copy /y README.md dist\x64\ >nul
rem x86 set (no PS3EyeVCam.dll: that's the Windows 11 camera, and Windows 11 is 64-bit only)
if not exist dist\x86 mkdir dist\x86
copy /y buildx86\Release\PS3EyeSetup.exe dist\x86\ >nul
copy /y buildx86\Release\PS3EyeServer.exe dist\x86\ >nul
copy /y buildx86\Release\PS3EyeDShow32.dll dist\x86\ >nul
copy /y README.md dist\x86\ >nul
rem ARM64 set: its own programs, plus the same 32-bit filter (32-bit apps run emulated there)
if not exist dist\arm64 mkdir dist\arm64
copy /y buildarm64\Release\PS3EyeVCam.dll dist\arm64\ >nul
copy /y buildarm64\Release\PS3EyeSetup.exe dist\arm64\ >nul
copy /y buildarm64\Release\PS3EyeServer.exe dist\arm64\ >nul
copy /y buildarm64\Release\PS3EyeDShow64.dll dist\arm64\ >nul
copy /y buildx86\Release\PS3EyeDShow32.dll dist\arm64\ >nul
copy /y README.md dist\arm64\ >nul

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
powershell -NoProfile -Command "Compress-Archive -Force -Path dist\x64\* -DestinationPath dist\PS3EyeCamera-%APPVERSION%-x64-portable.zip" || goto failed
powershell -NoProfile -Command "Compress-Archive -Force -Path dist\arm64\* -DestinationPath dist\PS3EyeCamera-%APPVERSION%-arm64-portable.zip" || goto failed
powershell -NoProfile -Command "Compress-Archive -Force -Path dist\x86\* -DestinationPath dist\PS3EyeCamera-%APPVERSION%-x86-portable.zip" || goto failed

echo.
echo BUILD OK. Files in dist\:
dir /b dist
echo.
if defined NOINSTALLER (
    echo Inno Setup isn't installed, so there's no installer wizard this time, only the portable zip.
    echo Install Inno Setup 6 or 7 from https://jrsoftware.org/isdl.php and run build.bat again.
) else (
    echo Installer ^(x64, ARM64 and x86^): dist\PS3EyeCamera-Setup-%APPVERSION%.exe
)
echo Portable: dist\PS3EyeCamera-%APPVERSION%-x64-portable.zip ^(x64^), dist\PS3EyeCamera-%APPVERSION%-arm64-portable.zip ^(ARM64^),
echo           dist\PS3EyeCamera-%APPVERSION%-x86-portable.zip ^(x86, 32-bit Windows 10^)
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

:findarm
set "HAVEARM="
rem (vcpkg looks for this same file to decide whether it can build for ARM64)
if exist "%VSDIR%\VC\Auxiliary\Build\vcvarsamd64_arm64.bat" set "HAVEARM=1"
exit /b 0

:findinno
set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 7\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 7\ISCC.exe"
if exist "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe"
exit /b 0
