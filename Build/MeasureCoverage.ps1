# Measures clang source-based coverage (line and branch) for one test executable and a set of source files.
#
# Builds the target in the Coverage configuration (-fprofile-instr-generate -fcoverage-mapping, linked with clang_rt.profile),
# runs the test executable with a test-name filter, merges the raw profiles with llvm-profdata and reports with llvm-cov.
# llvm-profdata and llvm-cov come from the same toolchain directory as the compiler (they must match its profile format).
#
# Example (asset runtime, Docs/Design/AssetSystem.ko.md section 14.1):
#   pwsh Build/MeasureCoverage.ps1 -Target EngineTests -Filter AssetPackage,FixtureAsset `
#     -Sources Engine/Source/Runtime/AssetRuntime/Private
#
# A program measured through an external driver (for example the C# commandlet tests) passes -Executable (the binary name
# when it differs from the target) and -Runner (the driver command line; {executable} is replaced by the coverage binary's
# full path). The runner replaces the filter loop:
#   pwsh Build/MeasureCoverage.ps1 -Target Commandlet -Executable RocksCommandlet `
#     -Runner 'dotnet','run','--project','Programs/Rocks.Commandlet.Tests','--','--executable','{executable}' `
#     -Sources Programs/Commandlet/Private,Engine/Source/Runtime/AssetRuntime/Private
#
# Outputs (under out/coverage/<target>/): merged.profdata, report.txt (per-file summary), uncovered.txt (lines and branches
# that never ran, llvm-cov show), summary.json (llvm-cov export -summary-only).
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Target,
    [string[]]$Filter = @(),
    [Parameter(Mandatory = $true)][string[]]$Sources,
    [string]$Executable = '',
    [string[]]$Runner = @(),
    [string]$Toolchain = "$env:LOCALAPPDATA/Programs/Swift/Toolchains/6.3.3+Asserts/usr/bin",
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$compiler = Join-Path $Toolchain 'clang-cl.exe'
$profdata = Join-Path $Toolchain 'llvm-profdata.exe'
$cov = Join-Path $Toolchain 'llvm-cov.exe'
foreach ($tool in @($compiler, $profdata, $cov)) {
    if (-not (Test-Path $tool)) {
        throw "Missing coverage tool: $tool"
    }
}

$targetLower = $Target.ToLowerInvariant()
$build = Join-Path $root "out/dotnet-windows-$targetLower-coverage"
$output = Join-Path $root "out/coverage/$targetLower"
if (-not $SkipBuild) {
    # The build step reports FAIL when another session's build evidence is stale; the native build line decides.
    & dotnet msbuild (Join-Path $root 'Rocks.proj') -t:Build -p:Configuration=Coverage "-p:RocksTarget=$Target" "-p:Compiler=$compiler" -nologo -v:m |
        Tee-Object -Variable buildLog | Out-Null
    if (-not ($buildLog -match 'Native build: PASS')) {
        $buildLog | Select-Object -Last 40 | Write-Host
        throw 'Coverage build failed.'
    }
}

$binaryName = if ($Executable -ne '') { $Executable } else { $Target }
$executable = Join-Path $build "bin/$binaryName.exe"
if (-not (Test-Path $executable)) {
    throw "Missing test executable: $executable"
}

if (Test-Path $output) {
    Remove-Item -Recurse -Force $output
}

New-Item -ItemType Directory -Force $output | Out-Null
$env:LLVM_PROFILE_FILE = Join-Path $output 'raw-%p-%m.profraw'
# -File invocations pass a comma list as one string.
$filters = @($Filter | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
if ($filters.Count -eq 0) {
    $filters = @('')
}
$runnerArguments = @($Runner | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' } | ForEach-Object { $_ -replace '\{executable\}', $executable })
if ($runnerArguments.Count -ne 0) {
    $runnerTool = $runnerArguments[0]
    $runnerRest = @($runnerArguments | Select-Object -Skip 1)
    Push-Location $root
    try {
        & $runnerTool @runnerRest | Tee-Object -FilePath (Join-Path $output 'run-runner.log') | Select-Object -Last 3 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            Write-Warning 'Runner failed (see run-runner.log)'
        }
    }
    finally {
        Pop-Location
    }

    $filters = @()
}

foreach ($name in $filters) {
    $arguments = @()
    if ($name -ne '') {
        $arguments += @('--filter', $name)
    }

    & $executable @arguments | Tee-Object -FilePath (Join-Path $output "run-$name.log") | Select-Object -Last 3 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        # Coverage is still measured; the failing tests are listed in the run log.
        Write-Warning "Tests failed: $name (see run-$name.log)"
    }
}

Remove-Item Env:LLVM_PROFILE_FILE
$raw = @(Get-ChildItem -Path $output -Filter '*.profraw' | ForEach-Object { $_.FullName })
$merged = Join-Path $output 'merged.profdata'
& $profdata merge -sparse @raw -o $merged
if ($LASTEXITCODE -ne 0) {
    throw 'llvm-profdata merge failed.'
}

# The coverage mapping may record a file relative to the repository root or as an absolute path (whichever the
# compile command used), so llvm-cov runs unfiltered and this script selects the source files itself.
$prefixes = @($Sources | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' } | ForEach-Object { ($_ -replace '/', '\').TrimEnd('\') + '\' })
function Test-Selected([string]$path) {
    $normalized = $path -replace '/', '\'
    foreach ($prefix in $prefixes) {
        if ($normalized.IndexOf($prefix, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }

    return $false
}

$json = & $cov export $executable "-instr-profile=$merged" -summary-only | ConvertFrom-Json
$selected = @($json.data[0].files | Where-Object { Test-Selected $_.filename } | Sort-Object filename)
$selected | ConvertTo-Json -Depth 6 | Out-File -Encoding utf8 (Join-Path $output 'summary.json')
$rows = @('{0,-70} {1,14} {2,14} {3,14}' -f 'File', 'Lines', 'Branches', 'Functions')
$total = @{ lines = 0; linesCovered = 0; branches = 0; branchesCovered = 0; functions = 0; functionsCovered = 0 }
foreach ($file in $selected) {
    $summary = $file.summary
    $total.lines += $summary.lines.count
    $total.linesCovered += $summary.lines.covered
    $total.branches += $summary.branches.count
    $total.branchesCovered += $summary.branches.covered
    $total.functions += $summary.functions.count
    $total.functionsCovered += $summary.functions.covered
    $rows += '{0,-70} {1,6}/{2,-7} {3,6}/{4,-7} {5,6}/{6,-7}' -f (Split-Path -Leaf $file.filename),
        $summary.lines.covered, $summary.lines.count, $summary.branches.covered, $summary.branches.count,
        $summary.functions.covered, $summary.functions.count
}

function Format-Percent([int]$covered, [int]$count) {
    if ($count -eq 0) {
        return '-'
    }

    return '{0:N2}%' -f (100.0 * $covered / $count)
}

$rows += 'TOTAL lines {0} ({1}/{2}), branches {3} ({4}/{5}), functions {6} ({7}/{8})' -f
    (Format-Percent $total.linesCovered $total.lines), $total.linesCovered, $total.lines,
    (Format-Percent $total.branchesCovered $total.branches), $total.branchesCovered, $total.branches,
    (Format-Percent $total.functionsCovered $total.functions), $total.functionsCovered, $total.functions
$rows | Tee-Object -FilePath (Join-Path $output 'report.txt')
$files = @($selected | ForEach-Object { $_.filename })
& $cov show $executable "-instr-profile=$merged" -show-branches=count -show-line-counts-or-regions -format=text @files |
    Out-File -Encoding utf8 (Join-Path $output 'uncovered.txt')
Write-Host "Coverage written to $output"
