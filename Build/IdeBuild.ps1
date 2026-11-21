# Visual Studio NMake entry point (Docs/Design/ProjectFiles.ko.md section 7). The generated .vcxproj files call Build/IdeBuild.bat, which
# runs this script.
#
#   IdeBuild.ps1 -Action Build|Rebuild|Clean -Target <target> -Configuration <configuration> [-Platform Windows|Android] [-Compiler <clang-cl>]
#
#   IdeBuild.ps1 -Action Build|Rebuild|Clean -Project <game.rocksproject> -Sdk <installed SDK> -Target <target> -Configuration <configuration>
#                [-Compiler <clang-cl>]
#
# The -Project form is the game solution (Docs/Design/ProjectFiles.ko.md section 10.4). It runs only from an installed SDK (rocks-sdk.json
# next to Build/), with the SDK's own build CLI: Build = game, Clean = clean --project, Rebuild = both.
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
$game = [bool] ($Project -or $Sdk)
if ($game -ne $script:RocksSdk -or ($game -and -not ($Project -and $Sdk))) {
    # Games always build with an installed SDK; an engine working copy builds its own targets only.
    $reason = if ($script:RocksSdk) { 'an installed SDK builds game projects only (-Project and -Sdk)' }
              else { 'game projects build with the IdeBuild.ps1 of their installed SDK, not of an engine working copy' }
    [Console]::Out.WriteLine("error RB0002: $reason.")
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
$common = @(if ($game) {
    @('--project', [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($Project)), '--target', $Target, '--configuration', $Configuration)
}
else {
    @('--root', $root, '--target', $Target, '--configuration', $Configuration)
})

# @(...) keeps a one-element result an array (a bare string would splat as characters).
$buildCommand = @(if ($game) { 'game', '--sdk', [System.IO.Path]::GetFullPath($Sdk) } else { 'build' })
# The compiler choice only matters to the build; clean computes the same output path without it. (PowerShell names are case-insensitive,
# so this must not be called $compiler.)
$compilerArguments = @()
if ($Platform -eq 'Android') {
    if ($game) {
        throw 'Game projects build for Windows only (the installed SDK has no Android libraries).'
    }

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
        & dotnet $cli @buildCommand @common @compilerArguments | Out-Host
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
