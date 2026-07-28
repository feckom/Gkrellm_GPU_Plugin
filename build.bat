@echo off
REM ---------------------------------------------------------------------
REM  GKrellM NVIDIA plugin - turnkey build launcher.
REM  Double click this file, or run it from a command prompt with extra
REM  switches, e.g.   build.bat -DryRun
REM ---------------------------------------------------------------------
setlocal
cd /d "%~dp0"

where powershell >nul 2>&1
if errorlevel 1 (
    echo.
    echo   Windows PowerShell was not found on this system.
    echo   It ships with Windows 7 and newer; check that
    echo   %%SystemRoot%%\System32\WindowsPowerShell\v1.0 is in your PATH.
    echo.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\bootstrap.ps1" %*
set RC=%ERRORLEVEL%

echo.
if "%RC%"=="0" (
    echo   Build finished successfully.
) else (
    echo   Build failed with exit code %RC%.  See build-log.txt
)
echo.
pause
exit /b %RC%
