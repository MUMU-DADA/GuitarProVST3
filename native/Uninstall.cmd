@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Action Uninstall -Elevate %*
if errorlevel 1 (
    echo Uninstallation failed.
    pause
    exit /b 1
)
echo Done. Start Guitar Pro normally.
pause
