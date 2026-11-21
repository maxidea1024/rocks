# Generates the Visual Studio solution and project files of the engine repository (Docs/Design/ProjectFiles.ko.md section 12): Rocks.slnx
# at the root and out/ProjectFiles. Prepares the tools and runs `Rocks.Build.Cli projectfiles --root` under the IDE build lock
# (Build/Tools.ps1), so it never races a Visual Studio build of the same working copy.
#
#   pwsh Build/GenerateProjectFiles.ps1 [-Compiler <clang-cl>]
#   pwsh Build/GenerateProjectFiles.ps1 -Project <game directory or .rocksproject> [-Sdk <installed SDK>] [-Compiler <clang-cl>]
#
# The -Project form generates the solution of a game project (section 10.4): <game>/<name>.slnx and <game>/out/ProjectFiles. Without -Sdk the
# SDK comes from the game's engineAssociation or the SDK registry, like a game build.
param([string] $Compiler, [string] $Project, [string] $Sdk)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Tools.ps1')
if ($script:RocksSdk -and -not $Project) {
    [Console]::Out.WriteLine('error RB0002: an installed SDK generates game project files only; pass -Project <game>.')
    exit 1
}

$root = $script:RocksRoot
$cli = $script:RocksCli
$arguments = if ($Project) { @('projectfiles', '--project', [System.IO.Path]::GetFullPath($Project)) } else { @('projectfiles', '--root', $root) }
if ($Sdk) {
    $arguments += @('--sdk', [System.IO.Path]::GetFullPath($Sdk))
}

if ($Compiler) {
    $arguments += @('--compiler', $Compiler)
}

[int] $code = 0
Enter-RocksIdeLock
try {
    Update-RocksTools
    & dotnet $cli @arguments | Out-Host
    $code = $LASTEXITCODE
}
catch {
    [Console]::Out.WriteLine("error RB0002: $($_.Exception.Message)")
    $code = 1
}
finally {
    Exit-RocksIdeLock
}

exit $code
