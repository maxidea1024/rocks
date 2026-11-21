@echo off
rem Rocks setup wizard (Docs/Guides/BuildEnvironment.ko.md). Double-click it after cloning, or run it from a terminal.
rem PowerShell 7 runs Build\Setup.ps1 when installed; a fresh Windows only has Windows PowerShell 5.1, which also runs it.
rem Build\Setup.ps1 waits for a key before closing when Explorer started this file, so the result stays readable.
setlocal
set "ROCKS_SHELL=powershell"
where pwsh >nul 2>nul && set "ROCKS_SHELL=pwsh"
%ROCKS_SHELL% -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build\Setup.ps1" %*
exit /b %ERRORLEVEL%
