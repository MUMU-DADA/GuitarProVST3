@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Elevate -MigrateExisting %*
if errorlevel 1 (
    echo Installation failed.
    pause
    exit /b 1
)
echo Done. Start Guitar Pro normally.
pause
