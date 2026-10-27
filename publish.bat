@echo off
rem Publishes this project to YOUR GitHub account: creates the repo, uploads the code
rem (commits authored by you), and creates a release with the installer from dist\.
rem Run build.bat first. Double-click to run.
setlocal
cd /d "%~dp0"
set "VERSION=1.0.0"
set "REPO=ps3eye-windows-camera-for-windows-10"
set "AUTHOR=Bassquake"
set "DESCRIPTION=Use the PlayStation 3 Eye as a normal webcam on Windows 10 and 11 (Zoom, Discord, Teams, OBS). One-click installer, no Zadig."
title Publish PS3 Eye Camera to GitHub

set "SETUP=dist\PS3EyeCamera-Setup-%VERSION%.exe"
set "PORTABLE=dist\PS3EyeCamera-%VERSION%-portable.zip"
if not exist "%SETUP%" (echo %SETUP% not found. Run build.bat first. & pause & exit /b 1)

rem ---- tools: git + GitHub CLI ----
where git >nul 2>nul || set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
where git >nul 2>nul || (winget install --id Git.Git -e --accept-package-agreements --accept-source-agreements & set "PATH=%ProgramFiles%\Git\cmd;%PATH%")
where gh >nul 2>nul || set "PATH=%ProgramFiles%\GitHub CLI;%PATH%"
where gh >nul 2>nul || (winget install --id GitHub.cli -e --accept-package-agreements --accept-source-agreements & set "PATH=%ProgramFiles%\GitHub CLI;%PATH%")
where gh >nul 2>nul || (echo GitHub CLI install failed. & pause & exit /b 1)

rem ---- log in to GitHub (opens your browser once) ----
gh auth status >nul 2>nul
if errorlevel 1 (
    echo.
    echo A browser window will open: sign in to GitHub and paste the code shown below.
    gh auth login --hostname github.com --git-protocol https --web || (pause & exit /b 1)
)
gh auth setup-git >nul 2>nul

for /f "delims=" %%a in ('gh api user --jq .login') do set "LOGIN=%%a"
for /f "delims=" %%a in ('gh api user --jq .id') do set "GHID=%%a"
if not defined LOGIN (echo Could not read your GitHub account. & pause & exit /b 1)
echo Publishing as %AUTHOR% (github.com/%LOGIN%)

rem ---- commit, authored by you (GitHub's private no-reply email keeps your real email hidden) ----
if not exist .git git init -q -b main
git config user.name "%AUTHOR%"
git config user.email "%GHID%+%LOGIN%@users.noreply.github.com"
git add -A
git diff --cached --quiet || git commit -q -m "PS3 Eye Camera for Windows 10 and 11 v%VERSION%" || goto failed

rem ---- repo + push ----
gh repo view "%LOGIN%/%REPO%" >nul 2>nul
if errorlevel 1 (
    gh repo create "%REPO%" --public --description "%DESCRIPTION%" || goto failed
)
git remote get-url origin >nul 2>nul || git remote add origin "https://github.com/%LOGIN%/%REPO%.git"
git push -u origin main || goto failed
gh repo edit "%LOGIN%/%REPO%" --add-topic ps3-eye --add-topic webcam --add-topic windows-11 --add-topic windows-10 --add-topic virtual-camera >nul 2>nul

rem ---- release with the installer you built and tested ----
gh release view "v%VERSION%" --repo "%LOGIN%/%REPO%" >nul 2>nul
if not errorlevel 1 (
    echo Release v%VERSION% already exists; updating its files.
    gh release upload "v%VERSION%" "%SETUP%" --repo "%LOGIN%/%REPO%" --clobber || goto failed
    if exist "%PORTABLE%" gh release upload "v%VERSION%" "%PORTABLE%" --repo "%LOGIN%/%REPO%" --clobber
    goto done
)
gh release create "v%VERSION%" "%SETUP%" --repo "%LOGIN%/%REPO%" --title "PS3 Eye Camera %VERSION%" --notes-file installer\release-notes.md || goto failed
if exist "%PORTABLE%" gh release upload "v%VERSION%" "%PORTABLE%" --repo "%LOGIN%/%REPO%"

:done
echo.
echo PUBLISHED: https://github.com/%LOGIN%/%REPO%
start "" "https://github.com/%LOGIN%/%REPO%"
pause
exit /b 0

:failed
echo.
echo PUBLISH FAILED. Send a screenshot of the error above to Claude.
pause
exit /b 1
