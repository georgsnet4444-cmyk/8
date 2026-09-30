@echo off
rem Double-click to build. Right-click -> "Run as administrator" to also install system-wide.
rem Extra options are passed through, e.g.:  build-windows.bat -Clean -BuildStandalone
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-windows.ps1" -Install %*
echo.
pause
