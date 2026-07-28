@echo off
REM ---------------------------------------------------------------------
REM  Removes the plugin and every file this project created.
REM  MSYS2 is NOT removed: it may be in use by other work. Uninstall it
REM  from Settings -> Apps if you want it gone.
REM ---------------------------------------------------------------------
setlocal
cd /d "%~dp0"

echo.
echo   Removing the plugin from %USERPROFILE%\.gkrellm2\plugins
if exist "%USERPROFILE%\.gkrellm2\plugins\gkrellm-nvidia.dll" (
    del /f /q "%USERPROFILE%\.gkrellm2\plugins\gkrellm-nvidia.dll"
    echo     removed.
) else (
    echo     not installed.
)

echo   Removing build output
if exist "%~dp0build" rmdir /s /q "%~dp0build"
if exist "%~dp0work"  rmdir /s /q "%~dp0work"
if exist "%~dp0build-log.txt" del /f /q "%~dp0build-log.txt"
echo     done.

echo.
echo   Note: MSYS2 (C:\msys64) was left in place.
echo   Remove it from Settings - Apps if it is not needed for anything else.
echo.
pause
