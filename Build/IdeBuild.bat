@echo off
rem Visual Studio NMake entry point (Docs/Design/ProjectFiles.ko.md section 7). Runs Build/IdeBuild.ps1 with PowerShell 7.
where pwsh >nul 2>nul
if errorlevel 1 (
  echo error: Visual Studio builds of Rocks need PowerShell 7 ^(pwsh^) on PATH. Install it from https://aka.ms/powershell and restart Visual Studio.
  exit /b 1
)
pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0IdeBuild.ps1" %*
exit /b %ERRORLEVEL%
