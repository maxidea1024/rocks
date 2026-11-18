# Runs the SpatialIndexBench measurement set (Docs/Design/SpatialIndex.ko.md section 9.5) and writes raw JSON files plus a run
# manifest (run.json) to OutDir. Every comparison group runs in its own process. The group order rotates per repeat, and each
# process starts with an untimed warm-up batch. System CPU load (total and the busiest other processes) is sampled right before
# and right after every process so loaded runs can be marked as preliminary.
#
# Build first:
#   dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=SpatialIndexBench -p:Configuration=Release
# Run:
#   pwsh Programs/SpatialIndexBench/RunBench.ps1 -OutDir Docs/Benchmarks/SpatialIndex/2026-11-18
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$Repeats = 3,
    [int]$LargeRepeats = 2,
    [string[]]$Groups = @('A1', 'A2', 'A2s', 'B', 'C16', 'C256', 'D'),
    [string[]]$Variants = @('D', 'D-k1', 'D-k65_64', 'D-k5_4', 'D-k3_2', 'D-s8', 'D-s32', 'D-s64', 'D-noinplace', 'D-immediate', 'D-boxdouble'),
    [int]$VariantRepeats = 1,
    [switch]$SkipTiming,
    [switch]$SkipCounters,
    [switch]$SkipVariants,
    [int]$LoadSampleSeconds = 3
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '../..')
$exe = Join-Path $root 'out/dotnet-windows-spatialindexbench-release/bin/SpatialIndexBench.exe'
if (-not (Test-Path $exe)) {
    throw "SpatialIndexBench.exe not found at $exe (build the Release target first)"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$logical = [Environment]::ProcessorCount

# Samples total CPU use and the busiest processes over a short window.
function Measure-Load {
    $before = @{}
    foreach ($process in Get-Process) {
        try {
            $before[$process.Id] = @{ Name = $process.Name; Cpu = $process.TotalProcessorTime.TotalSeconds }
        }
        catch {
        }
    }

    $counter = Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval $LoadSampleSeconds -MaxSamples 1
    $busy = @()
    foreach ($process in Get-Process) {
        if ($before.ContainsKey($process.Id)) {
            try {
                $delta = $process.TotalProcessorTime.TotalSeconds - $before[$process.Id].Cpu
                $percent = 100.0 * $delta / ($LoadSampleSeconds * $logical)
                if ($percent -ge 0.5) {
                    $busy += [ordered]@{ name = $process.Name; id = $process.Id; cpuPercentOfMachine = [Math]::Round($percent, 2) }
                }
            }
            catch {
            }
        }
    }

    return [ordered]@{
        utc = (Get-Date).ToUniversalTime().ToString('o')
        totalCpuPercent = [Math]::Round($counter.CounterSamples[0].CookedValue, 2)
        busiest = @($busy | Sort-Object { $_.cpuPercentOfMachine } -Descending | Select-Object -First 6)
    }
}

function Invoke-Bench([string]$Name, [string[]]$Arguments) {
    $loadBefore = Measure-Load
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & $exe @Arguments 2>&1 | Where-Object { $_ -notmatch '^\[Warning\]\[SpatialIndex\]' } | Out-Host
    $exitCode = $LASTEXITCODE
    $watch.Stop()
    $loadAfter = Measure-Load
    return [ordered]@{
        name = $Name
        command = "SpatialIndexBench.exe " + ($Arguments -join ' ')
        exitCode = $exitCode
        seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 2)
        loadBefore = $loadBefore
        loadAfter = $loadAfter
    }
}

$processor = Get-CimInstance Win32_Processor | Select-Object -First 1
$os = Get-CimInstance Win32_OperatingSystem
$powerPlan = (powercfg /getactivescheme) -join ' '
$clang = ''
try {
    $clang = (& clang-cl --version | Select-Object -First 1)
}
catch {
}

$script:manifest = [ordered]@{
    tool = 'SpatialIndexBench RunBench.ps1'
    startedUtc = (Get-Date).ToUniversalTime().ToString('o')
    machine = [ordered]@{
        cpu = $processor.Name.Trim()
        cores = $processor.NumberOfCores
        logicalProcessors = $processor.NumberOfLogicalProcessors
        maxClockMHz = $processor.MaxClockSpeed
        memoryBytes = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
        os = "$($os.Caption) $($os.Version)"
        powerPlan = $powerPlan
        coreAffinity = 'none (not pinned)'
    }
    compilerOnPath = $clang
    gitHead = (git -C $root rev-parse HEAD)
    repeats = $Repeats
    runs = @()
}

# Runs every group once per repeat with the group order rotated per repeat.
function Invoke-Rotation([string]$Prefix, [string[]]$Names, [int]$Count, [string[]]$Extra) {
    for ($repeat = 0; $repeat -lt $Count; $repeat++) {
        for ($step = 0; $step -lt $Names.Count; $step++) {
            $name = $Names[($step + $repeat) % $Names.Count]
            $file = Join-Path $OutDir "$Prefix-r$repeat-$name.json"
            Write-Host "== $Prefix repeat $repeat $name"
            $script:manifest.runs += Invoke-Bench "$Prefix-r$repeat-$name" (@('--group', $name, '--out', $file) + $Extra)
        }
    }
}

if (-not $SkipTiming) {
    # 8 distributions x 1e3..1e5 (and the separate scenarios), then the representative 4 distributions x 1e6.
    Invoke-Rotation 'timing' $Groups $Repeats @()
    Invoke-Rotation 'timing-large' $Groups $LargeRepeats @('--profile', 'large')
}

if (-not $SkipCounters) {
    Invoke-Rotation 'counters' $Groups 1 @('--counters', '--no-scenarios')
    Invoke-Rotation 'counters-large' $Groups 1 @('--counters', '--profile', 'large')
}

if (-not $SkipVariants) {
    Invoke-Rotation 'variant' $Variants $VariantRepeats @('--profile', 'variant')
}

$manifest.finishedUtc = (Get-Date).ToUniversalTime().ToString('o')
$manifest | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $OutDir 'run.json')
Write-Host "wrote $(Join-Path $OutDir 'run.json')"
