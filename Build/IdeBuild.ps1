# Visual Studio NMake entry point (Docs/Design/ProjectFiles.ko.md section 7). The generated .vcxproj files call Build/IdeBuild.bat, which
# runs this script.
#
#   IdeBuild.ps1 -Action Build|Rebuild|Clean -Target <target> -Configuration <configuration> -Platform Windows|Android [-Compiler <clang-cl>]
#
# The whole call runs under the IDE build lock (Build/Tools.ps1): a solution build runs its projects one at a time, like UE -WaitMutex, and
# the tools are prepared (built only when their inputs changed) inside the same lock, so no build overwrites a tool another build runs.
# -Compiler is the clang-cl the project files were generated with; Visual Studio rewrites PATH before NMake, so the build does not search it.
param(
    [Parameter(Mandatory)][ValidateSet('Build', 'Rebuild', 'Clean')][string] $Action,
    [Parameter(Mandatory)][string] $Target,
    [Parameter(Mandatory)][string] $Configuration,
    [Parameter(Mandatory)][ValidateSet('Windows', 'Android')][string] $Platform,
    [string] $Compiler
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Tools.ps1')

$root = $script:RocksRoot
$cli = Join-Path $root 'out/dotnet/bin/Rocks.Build.Cli/Debug/net10.0/Rocks.Build.Cli.dll'
$common = @('--root', $root, '--target', $Target, '--configuration', $Configuration)
# The compiler choice only matters to the build; clean computes the same output path without it. (PowerShell names are case-insensitive,
# so this must not be called $compiler.)
$compilerArguments = @()
if ($Platform -eq 'Android') {
    if ($Compiler) {
        throw 'Android builds take no -Compiler (the NDK clang is found with --android-ndk or ANDROID_NDK_HOME).'
    }

    $common += @('--platform', 'android')
}
elseif ($Compiler) {
    $compilerArguments = @('--compiler', $Compiler)
}

# Runs the requested action with the Rocks build CLI and returns its exit code. Call it while holding the IDE build lock.
function Invoke-RocksIdeAction {
    Update-RocksTools
    $result = 0
    if ($Action -eq 'Clean' -or $Action -eq 'Rebuild') {
        & dotnet $cli clean @common | Out-Host
        $result = $LASTEXITCODE
    }

    if ($result -eq 0 -and ($Action -eq 'Build' -or $Action -eq 'Rebuild')) {
        & dotnet $cli build @common @compilerArguments | Out-Host
        $result = $LASTEXITCODE
    }

    return $result
}

$code = 0
Enter-RocksIdeLock
try {
    # Test hook (Rocks.Build.IntegrationTests, section 16 V7): with ROCKS_IDE_BUILD_PROBE set, the script writes that file once it holds the
    # lock and stops before preparing the tools or building.
    if ($env:ROCKS_IDE_BUILD_PROBE) {
        Set-Content -LiteralPath $env:ROCKS_IDE_BUILD_PROBE -Value "$Action $Target $Configuration $Platform" -Encoding utf8NoBOM
    }
    else {
        $code = Invoke-RocksIdeAction
    }
}
finally {
    Exit-RocksIdeLock
}

exit $code
