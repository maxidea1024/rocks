# Prepares the Rocks build tools (Docs/Design/ProjectFiles.ko.md section 7.2).
#
# Builds Programs/Rocks.Tools.csproj only when the tool input fingerprint differs from the one recorded after the last successful build
# (out/dotnet/.rocks-tools.json), or when a tool output DLL is missing or changed. Builds hold the IDE build lock, a named mutex per
# working copy, so a tool build never overwrites a DLL that another IDE build is running.
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

# The repository root in its on-disk spelling (each existing component as the file system names it), so the lock name does not depend
# on how the caller spelled the path.
function Get-RocksOnDiskPath([string] $path) {
    $full = [System.IO.Path]::GetFullPath($path).TrimEnd('\', '/')
    $root = [System.IO.Path]::GetPathRoot($full)
    $current = $root.ToUpperInvariant()
    foreach ($part in $full.Substring($root.Length).Split([char[]] @('\', '/'), [System.StringSplitOptions]::RemoveEmptyEntries)) {
        $match = [System.IO.Directory]::GetFileSystemEntries($current, $part) | Select-Object -First 1
        $current = if ($match) { $match } else { [System.IO.Path]::Combine($current, $part) }
    }

    return $current
}

$script:RocksRoot = Get-RocksOnDiskPath (Join-Path $PSScriptRoot '..')
$script:RocksToolsStamp = Join-Path $script:RocksRoot 'out/dotnet/.rocks-tools.json'
$script:RocksIdeMutex = $null

# Tool output directories. Every DLL in them is checked (the tools and the dependencies they load).
$script:RocksToolOutputs = @(
    'out/dotnet/bin/Rocks.Build.Cli/Debug/net10.0',
    'out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0',
    'out/dotnet/bin/Rocks.ToolTests/Debug/net10.0',
    'out/dotnet/bin/Rocks.Engine.Rules/Debug/net10.0'
)

function Get-RocksLockName {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($script:RocksRoot.ToLowerInvariant())
    $hash = [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).Substring(0, 16).ToLowerInvariant()
    # Local\ is per logon session (Docs/Design/ProjectFiles.ko.md section 17, L18).
    return "Local\Rocks.IdeBuild.$hash"
}

# Takes the IDE build lock. Prints one line before waiting when another build holds it. An abandoned mutex (its owner died) counts as
# acquired. The order comes from the lock alone; the wait wakes up periodically only so that Ctrl+C can stop it.
function Enter-RocksIdeLock {
    if ($null -eq $script:RocksIdeMutex) {
        $script:RocksIdeMutex = [System.Threading.Mutex]::new($false, (Get-RocksLockName))
    }

    try {
        if (-not $script:RocksIdeMutex.WaitOne(0)) {
            [Console]::Out.WriteLine('Waiting for the IDE build lock ...')
            [Console]::Out.Flush()
            while (-not $script:RocksIdeMutex.WaitOne(250)) {
            }
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

# Project files reachable from Programs/Rocks.Tools.csproj through ProjectReference (the tools and the rules assembly, not the tests).
function Get-RocksToolProjects {
    $projects = [System.Collections.Generic.SortedSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push((Join-Path $script:RocksRoot 'Programs/Rocks.Tools.csproj'))
    while ($pending.Count -ne 0) {
        $project = [System.IO.Path]::GetFullPath($pending.Pop())
        if (-not $projects.Add($project)) {
            continue
        }

        foreach ($reference in ([xml] (Get-Content -LiteralPath $project -Raw)).SelectNodes('//*[local-name()="ProjectReference"]')) {
            $pending.Push([System.IO.Path]::Combine([System.IO.Path]::GetDirectoryName($project), $reference.GetAttribute('Include')))
        }
    }

    return $projects
}

# Repository-relative files whose content decides the tool build: every file of the tool project directories (Rocks.Tools.csproj itself
# only, not its whole Programs directory), every *.Build.cs/*.Target.cs the rules assembly compiles, the MSBuild props they import,
# global.json, the response file and the files the tools embed or copy (.clang-format, .editorconfig, Build/Toolchain.json).
function Get-RocksToolInputs {
    $root = $script:RocksRoot
    $files = [System.Collections.Generic.SortedSet[string]]::new([System.StringComparer]::Ordinal)
    function Add-File([string] $path) {
        $relative = [System.IO.Path]::GetRelativePath($root, $path).Replace('\', '/')
        if ($relative -notmatch '(^|/)(bin|obj)/' -and -not $relative.StartsWith('out/')) {
            [void] $files.Add($relative)
        }
    }

    foreach ($project in Get-RocksToolProjects) {
        if ([System.IO.Path]::GetFileName($project) -eq 'Rocks.Tools.csproj') {
            Add-File $project
            continue
        }

        foreach ($file in Get-ChildItem -LiteralPath ([System.IO.Path]::GetDirectoryName($project)) -Recurse -File) {
            Add-File $file.FullName
        }
    }

    foreach ($tree in @('Core/Source', 'Engine/Source', 'Engine/Plugins', 'Programs')) {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $root $tree) -Recurse -File -Include '*.Build.cs', '*.Target.cs' -ErrorAction SilentlyContinue) {
            if ([System.IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/') -notmatch '^Programs/Rocks\.') {
                Add-File $file.FullName
            }
        }
    }

    foreach ($single in @('Programs/Directory.Build.props', 'Build/Directory.Build.props', 'Build/Toolchain.json', 'global.json', 'Directory.Build.rsp',
            '.clang-format', '.editorconfig')) {
        if (Test-Path -LiteralPath (Join-Path $root $single)) {
            [void] $files.Add($single)
        }
    }

    return $files
}

# Content SHA-256 of a file, reusing the recorded hash when the size and both times match the record (the same rule as the incremental
# store's large output check); otherwise the file is hashed again.
function Get-RocksFileHash([string] $path, $memo, $next) {
    $item = Get-Item -LiteralPath $path
    $key = "$($item.Length)|$($item.LastWriteTimeUtc.Ticks)|$($item.CreationTimeUtc.Ticks)"
    $relative = [System.IO.Path]::GetRelativePath($script:RocksRoot, $path).Replace('\', '/')
    $recorded = if ($null -ne $memo) { $memo.PSObject.Properties[$relative] } else { $null }
    $hash = if ($null -ne $recorded -and $recorded.Value.key -eq $key) {
        $recorded.Value.sha256
    }
    else {
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }

    $next[$relative] = [ordered]@{ key = $key; sha256 = $hash }
    return $hash
}

function Get-RocksToolState($memo) {
    $root = $script:RocksRoot
    $next = [ordered]@{}
    $text = [System.Text.StringBuilder]::new()
    [void] $text.Append('dotnet ').Append((& dotnet --version).Trim()).Append("`n")
    foreach ($relative in Get-RocksToolInputs) {
        [void] $text.Append($relative).Append("`t").Append((Get-RocksFileHash (Join-Path $root $relative) $memo $next)).Append("`n")
    }

    $outputs = [System.Text.StringBuilder]::new()
    foreach ($directory in $script:RocksToolOutputs) {
        $full = Join-Path $root $directory
        if (-not (Test-Path -LiteralPath $full)) {
            [void] $outputs.Append($directory).Append("`tmissing`n")
            continue
        }

        foreach ($dll in Get-ChildItem -LiteralPath $full -File -Filter '*.dll' | Sort-Object Name) {
            [void] $outputs.Append($directory).Append('/').Append($dll.Name).Append("`t").Append((Get-RocksFileHash $dll.FullName $memo $next)).Append("`n")
        }
    }

    $hash = { param($value) [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData([System.Text.Encoding]::UTF8.GetBytes($value))).ToLowerInvariant() }
    return [pscustomobject]@{ Fingerprint = (& $hash $text.ToString()); Outputs = (& $hash $outputs.ToString()); Files = $next }
}

# Builds the tools when the inputs or outputs changed since the last successful build. Call it while holding the IDE build lock. Prints
# "Rocks tools are up to date." or the build output; throws when the build fails.
function Update-RocksTools {
    $stamp = $null
    if (Test-Path -LiteralPath $script:RocksToolsStamp) {
        try {
            $stamp = Get-Content -LiteralPath $script:RocksToolsStamp -Raw | ConvertFrom-Json
        }
        catch {
            $stamp = $null
        }
    }

    $memo = if ($null -ne $stamp -and $stamp.PSObject.Properties['files']) { $stamp.files } else { $null }
    $state = Get-RocksToolState $memo
    if ($null -ne $stamp -and $stamp.fingerprint -eq $state.Fingerprint -and $stamp.outputs -eq $state.Outputs) {
        [Console]::Out.WriteLine('Rocks tools are up to date.')
        return
    }

    [Console]::Out.WriteLine('Building the Rocks tools ...')
    & dotnet build (Join-Path $script:RocksRoot 'Programs/Rocks.Tools.csproj') --nologo -v:q | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Building the Rocks tools failed (exit code $LASTEXITCODE)."
    }

    $after = Get-RocksToolState $state.Files
    $record = [ordered]@{ fingerprint = $after.Fingerprint; outputs = $after.Outputs; files = $after.Files }
    New-Item -ItemType Directory -Force -Path (Split-Path $script:RocksToolsStamp) | Out-Null
    ($record | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath $script:RocksToolsStamp -Encoding utf8NoBOM
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
