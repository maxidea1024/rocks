@echo off
rem Generates the Visual Studio solution (Rocks.slnx) and project files (out/ProjectFiles) of this working copy.
rem Docs/Design/ProjectFiles.ko.md section 12. Arguments are passed to Build/GenerateProjectFiles.ps1 (for example -Compiler <clang-cl>).
where pwsh >nul 2>nul
if errorlevel 1 (
  echo error: GenerateProjectFiles needs PowerShell 7 ^(pwsh^) on PATH. Install it from https://aka.ms/powershell.
  pause
  exit /b 1
)
pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build\GenerateProjectFiles.ps1" %*
if errorlevel 1 (
  pause
  exit /b 1
)
exit /b 0
