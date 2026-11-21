# Visual Studio NMake entry point (Docs/Design/ProjectFiles.ko.md section 7). The generated .vcxproj files call Build/IdeBuild.bat, which
# runs this script.
#
#   IdeBuild.ps1 -Action Build|Rebuild|Clean -Target <target> -Configuration <configuration> [-Platform Windows|Android] [-Compiler <clang-cl>]
#
# -Project and -Sdk are the game project form of the generated game solution; they are declared so that the call binds and reports that
# game projects are not supported by this version of the script.
#
# The whole call runs under the IDE build lock (Build/Tools.ps1): a solution build runs its projects one at a time, like UE -WaitMutex, and
# the tools are prepared (built only when their inputs changed) inside the same lock, so no build overwrites a tool another build runs.
# -Compiler is the clang-cl the project files were generated with; Visual Studio rewrites PATH before NMake, so the build does not search it.
param(
    [Parameter(Mandatory)][ValidateSet('Build', 'Rebuild', 'Clean')][string] $Action,
    [Parameter(Mandatory)][string] $Target,
    [Parameter(Mandatory)][string] $Configuration,
    [ValidateSet('Windows', 'Android')][string] $Platform = 'Windows',
    [string] $Compiler,
    [string] $Project,
    [string] $Sdk
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Tools.ps1')
if ($script:RocksSdk -or $Project -or $Sdk) {
    # Game project files and their NMake commands come with the game solution generator; an SDK only ships these scripts for it.
    [Console]::Out.WriteLine("error RB0002: $([System.IO.Path]::GetFileName($PSCommandPath)) does not support game projects yet; use Rocks.Build.Cli game.")
    exit 1
}

# Visual Studio sets INCLUDE, EXTERNAL_INCLUDE, LIB and LIBPATH for Makefile projects too (SetBuildDefaultEnvironmentVariables), but not
# VCToolsInstallDir, so the build runs VsDevCmd again and VsDevCmd prepends to them. The doubled values would change the toolchain key
# (a terminal build after an IDE build would compile everything again) and the IntelliSense include path the staleness check renders.
# UE clears INCLUDE for the same reason (VCToolChain.cs).
foreach ($name in @('INCLUDE', 'EXTERNAL_INCLUDE', 'LIB', 'LIBPATH')) {
    Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
}

# The CLI adds an "error RB0001: ..." line to a top-level failure so that Visual Studio lists it in the Error List.
$env:ROCKS_IDE_BUILD = '1'
$root = $script:RocksRoot
$cli = $script:RocksCli
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

# Runs the requested action with the Rocks build CLI and returns its exit code. Call it while holding the IDE build lock. Command output
# goes to the host, never into the return value.
function Invoke-RocksIdeAction {
    try {
        Update-RocksTools
    }
    catch {
        [Console]::Out.WriteLine("error RB0002: $($_.Exception.Message)")
        return 1
    }

    $result = 0
    if ($Action -eq 'Clean' -or $Action -eq 'Rebuild') {
        & dotnet $cli clean @common | Out-Host
        $result = $LASTEXITCODE
    }

    if ($result -eq 0 -and ($Action -eq 'Build' -or $Action -eq 'Rebuild')) {
        & dotnet $cli build @common @compilerArguments | Out-Host
        $result = $LASTEXITCODE
    }

    return [int] $result
}

[int] $code = 0
Enter-RocksIdeLock
try {
    # Test hook (Rocks.Build.IntegrationTests, section 16 V7): with ROCKS_IDE_BUILD_PROBE and ROCKS_IDE_BUILD_TEST=1, the script writes that
    # file once it holds the lock and stops before preparing the tools or building. Without ROCKS_IDE_BUILD_TEST it is ignored.
    if ($env:ROCKS_IDE_BUILD_PROBE -and $env:ROCKS_IDE_BUILD_TEST -eq '1') {
        Set-Content -LiteralPath $env:ROCKS_IDE_BUILD_PROBE -Value "$Action $Target $Configuration $Platform" -Encoding utf8NoBOM
    }
    else {
        if ($env:ROCKS_IDE_BUILD_PROBE) {
            [Console]::Out.WriteLine('warning: ROCKS_IDE_BUILD_PROBE is set outside the tests (ROCKS_IDE_BUILD_TEST is not 1); it is ignored.')
        }

        $code = Invoke-RocksIdeAction
    }
}
finally {
    Exit-RocksIdeLock
}

exit $code
