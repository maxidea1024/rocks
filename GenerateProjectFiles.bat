@echo off
rem Generates the Visual Studio solution (Rocks.slnx) and project files (out/ProjectFiles) of this working copy.
rem Docs/Design/ProjectFiles.ko.md section 12. Arguments are passed to Build/GenerateProjectFiles.ps1 (for example -Compiler <clang-cl>).
rem On failure it waits for a key so that the error stays visible when started from Explorer (like the Unreal Engine script).
setlocal
set ROCKS_CODE=0
where pwsh >nul 2>nul
if errorlevel 1 (
  echo error: GenerateProjectFiles needs PowerShell 7 ^(pwsh^) on PATH. Install it from https://aka.ms/powershell.
  set ROCKS_CODE=1
  goto done
)
pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build\GenerateProjectFiles.ps1" %*
if errorlevel 1 set ROCKS_CODE=1
:done
if not "%ROCKS_CODE%"=="0" pause
exit /b %ROCKS_CODE%
