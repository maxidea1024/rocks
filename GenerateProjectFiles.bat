@echo off
rem Generates the Visual Studio solution (Rocks.slnx) and project files (out/ProjectFiles) of this working copy.
rem Docs/Design/ProjectFiles.ko.md section 12. Arguments are passed to Build/GenerateProjectFiles.ps1 (for example -Compiler <clang-cl>).
rem Started from Explorer (the command line of this cmd names this file by its full path), it waits for a key at the end so that the output stays visible.
setlocal
set ROCKS_PAUSE=0
echo %cmdcmdline% | find /i "%~f0" >nul && set ROCKS_PAUSE=1
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
if "%ROCKS_PAUSE%"=="1" pause
exit /b %ROCKS_CODE%
