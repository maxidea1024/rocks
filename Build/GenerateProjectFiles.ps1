# Generates the Visual Studio solution and project files of the engine repository (Docs/Design/ProjectFiles.ko.md section 12): Rocks.slnx
# at the root and out/ProjectFiles. Prepares the tools and runs `Rocks.Build.Cli projectfiles --root` under the IDE build lock
# (Build/Tools.ps1), so it never races a Visual Studio build of the same working copy.
#
#   pwsh Build/GenerateProjectFiles.ps1 [-Compiler <clang-cl>]
param([string] $Compiler, [string] $Project)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Tools.ps1')
if ($script:RocksSdk -or $Project) {
    # Game project files and their NMake commands come with the game solution generator; an SDK only ships these scripts for it.
    [Console]::Out.WriteLine("error RB0002: $([System.IO.Path]::GetFileName($PSCommandPath)) does not support game projects yet; use Rocks.Build.Cli game.")
    exit 1
}

$root = $script:RocksRoot
$cli = $script:RocksCli
$arguments = @('projectfiles', '--root', $root)
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
