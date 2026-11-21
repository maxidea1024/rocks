# Prepares the Rocks build tools (Docs/Design/ProjectFiles.ko.md section 7.2).
#
# Builds Programs/Rocks.Tools.csproj only when the tool input fingerprint differs from the one recorded after the last successful build
# (out/dotnet/.rocks-tools.json), or when a tool output is missing or changed. Builds hold the IDE build lock, a named mutex per working
# copy, so a tool build never overwrites a DLL that another IDE build is running.
#
# Run directly (Rocks.proj BootstrapTools): takes the lock, prepares the tools, releases the lock.
# Dot-sourced (Build/IdeBuild.ps1, Build/GenerateProjectFiles.ps1): defines Enter-RocksIdeLock, Exit-RocksIdeLock and Update-RocksTools;
# the caller keeps the lock while it also runs the tools. A mutex is re-entrant on the thread that owns it.
#
#   pwsh Build/Tools.ps1 [-ListInputs]
#
# -ListInputs prints the repository-relative input files of the fingerprint, one per line, and exits (tests compare it with what MSBuild
# evaluates).
param([switch] $ListInputs)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RocksRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:RocksToolsStamp = Join-Path $script:RocksRoot 'out/dotnet/.rocks-tools.json'
$script:RocksIdeMutex = $null

# Tool outputs that must exist (Rocks.proj and the IDE scripts run them).
$script:RocksToolOutputs = @(
    'out/dotnet/bin/Rocks.Build.Cli/Debug/net10.0/Rocks.Build.Cli.dll',
    'out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll',
    'out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll',
    'out/dotnet/bin/Rocks.Engine.Rules/Debug/net10.0/Rocks.Engine.Rules.dll'
)

function Get-RocksLockName {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($script:RocksRoot.TrimEnd('\', '/').ToLowerInvariant())
    $hash = [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).Substring(0, 16).ToLowerInvariant()
    return "Local\Rocks.IdeBuild.$hash"
}

# Takes the IDE build lock. Prints one line before waiting when another build holds it. An abandoned mutex (its owner died) counts as
# acquired. No timeout: the order comes from the lock, not from time.
function Enter-RocksIdeLock {
    if ($null -eq $script:RocksIdeMutex) {
        $script:RocksIdeMutex = [System.Threading.Mutex]::new($false, (Get-RocksLockName))
    }

    try {
        if (-not $script:RocksIdeMutex.WaitOne(0)) {
            [Console]::Out.WriteLine('Waiting for the IDE build lock ...')
            [Console]::Out.Flush()
            [void] $script:RocksIdeMutex.WaitOne()
        }
    }
    catch [System.Threading.AbandonedMutexException] {
    }
}

function Exit-RocksIdeLock {
    if ($null -ne $script:RocksIdeMutex) {
        $script:RocksIdeMutex.ReleaseMutex()
    }
}

# Repository-relative files whose content decides the tool build: the tool projects (Programs/Rocks.*, without bin/obj), the rule base
# classes, every *.Build.cs/*.Target.cs the rules assembly compiles, the MSBuild props they import, global.json, the response file and the
# files the tools embed or copy (.clang-format, Build/Toolchain.json).
function Get-RocksToolInputs {
    $root = $script:RocksRoot
    $files = [System.Collections.Generic.SortedSet[string]]::new([System.StringComparer]::Ordinal)
    function Add-Tree([string] $directory, [string[]] $patterns) {
        if (-not (Test-Path -LiteralPath $directory)) {
            return
        }

        foreach ($pattern in $patterns) {
            foreach ($file in Get-ChildItem -LiteralPath $directory -Recurse -File -Filter $pattern) {
                $relative = [System.IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')
                if ($relative -notmatch '/(bin|obj)/') {
                    [void] $files.Add($relative)
                }
            }
        }
    }

    foreach ($project in Get-ChildItem -LiteralPath (Join-Path $root 'Programs') -Directory -Filter 'Rocks.*') {
        Add-Tree $project.FullName @('*')
    }

    Add-Tree (Join-Path $root 'Build/Rules') @('*')
    Add-Tree (Join-Path $root 'Build/Format') @('*')
    foreach ($tree in @('Core/Source', 'Engine/Source', 'Engine/Plugins', 'Programs')) {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $root $tree) -Recurse -File -Include '*.Build.cs', '*.Target.cs' -ErrorAction SilentlyContinue) {
            $relative = [System.IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')
            if ($relative -notmatch '^Programs/Rocks\.' -and $relative -notmatch '/(bin|obj)/') {
                [void] $files.Add($relative)
            }
        }
    }

    foreach ($single in @('Programs/Rocks.Tools.csproj', 'Programs/Directory.Build.props', 'Build/Directory.Build.props', 'Build/Toolchain.json',
            'global.json', 'Directory.Build.rsp', '.clang-format')) {
        if (Test-Path -LiteralPath (Join-Path $root $single)) {
            [void] $files.Add($single)
        }
    }

    return $files
}

function Get-RocksToolFingerprint {
    $root = $script:RocksRoot
    $text = [System.Text.StringBuilder]::new()
    [void] $text.Append('dotnet ').Append((& dotnet --version).Trim()).Append("`n")
    foreach ($relative in Get-RocksToolInputs) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $root $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
        [void] $text.Append($relative).Append("`t").Append($hash).Append("`n")
    }

    $bytes = [System.Text.Encoding]::UTF8.GetBytes($text.ToString())
    return [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
}

function Get-RocksToolOutputHashes {
    $root = $script:RocksRoot
    $hashes = [ordered]@{}
    foreach ($relative in $script:RocksToolOutputs) {
        $path = Join-Path $root $relative
        $hashes[$relative] = if (Test-Path -LiteralPath $path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' }
    }

    return $hashes
}

# Builds the tools when the inputs or outputs changed since the last successful build. Call it while holding the IDE build lock.
function Update-RocksTools {
    $fingerprint = Get-RocksToolFingerprint
    $outputs = Get-RocksToolOutputHashes
    if (Test-Path -LiteralPath $script:RocksToolsStamp) {
        try {
            $stamp = Get-Content -LiteralPath $script:RocksToolsStamp -Raw | ConvertFrom-Json
            $same = $stamp.fingerprint -eq $fingerprint
            foreach ($name in $outputs.Keys) {
                $same = $same -and $outputs[$name] -ne '' -and $stamp.outputs.$name -eq $outputs[$name]
            }

            if ($same) {
                return
            }
        }
        catch {
        }
    }

    & dotnet build (Join-Path $script:RocksRoot 'Programs/Rocks.Tools.csproj') --nologo -v:q
    if ($LASTEXITCODE -ne 0) {
        throw "Building the Rocks tools failed (exit code $LASTEXITCODE)."
    }

    $record = [ordered]@{ fingerprint = $fingerprint; outputs = Get-RocksToolOutputHashes }
    New-Item -ItemType Directory -Force -Path (Split-Path $script:RocksToolsStamp) | Out-Null
    ($record | ConvertTo-Json) | Set-Content -LiteralPath $script:RocksToolsStamp -Encoding utf8NoBOM
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($ListInputs) {
        Get-RocksToolInputs | ForEach-Object { $_ }
        exit 0
    }

    Enter-RocksIdeLock
    try {
        Update-RocksTools
    }
    finally {
        Exit-RocksIdeLock
    }
}
