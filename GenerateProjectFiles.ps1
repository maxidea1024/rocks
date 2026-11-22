#Requires -Version 7.0
# Generates the Visual Studio solution (Rocks.slnx) and project files (out/ProjectFiles) of this working copy.
# PowerShell counterpart of GenerateProjectFiles.bat (Docs/Design/ProjectFiles.ko.md section 12): the arguments go to
# Build/GenerateProjectFiles.ps1 unchanged and its exit code is returned. Unlike the batch file it never waits for a key.
#
#   ./GenerateProjectFiles.ps1 [-Compiler <clang-cl>]
#   ./GenerateProjectFiles.ps1 -Project <game directory or .rocksproject> [-Sdk <installed SDK>] [-Compiler <clang-cl>]
param([string] $Compiler, [string] $Project, [string] $Sdk)

& (Join-Path $PSScriptRoot 'Build/GenerateProjectFiles.ps1') @PSBoundParameters
exit $LASTEXITCODE
