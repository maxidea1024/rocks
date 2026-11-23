# Shared functions for the offline tool build of prebuilt binaries (manifest kind: binary) (Windows, PowerShell 7).
# Per-library scripts (Engine/Source/ThirdParty/<Lib>/Build/Build.ps1) dot-source this file. Docs/Design/ShaderSystem section 8, item 4.
#
# Order: download build.source and build.dependencies of the manifest (ThirdParty.json) and verify SHA-256 -> assemble the source tree -> apply build.patches ->
# confirm the public headers and licenses committed to the repository match the sources byte for byte -> (the library script builds with CMake and Ninja) -> collect
# the outputs in a stage directory and produce a tar.gz archive and a host entry JSON (file list, SHA-256, size, role).
# Publishing (GitHub Release) is not done here; the workflow (.github/workflows/third-party-tools.yml) or a person does it separately.
#
# This file is not on the default engine build and test path. Python is used by the upstream CMake for table generation (Python 3 on PATH); this script never
# calls it directly (a directory covered by the python-main-path policy check).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

# Host key (key of the manifest hosts). The script currently produces only Windows x64.
$script:RocksToolHost = 'windows-x64'

function Read-RocksToolManifest {
    param([Parameter(Mandatory)][string] $LibraryDirectory)

    $path = Join-Path $LibraryDirectory 'ThirdParty.json'
    $manifest = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    if ($manifest.kind -ne 'binary' -or -not $manifest.build) {
        throw "$path is not a built binary manifest (kind binary with build)"
    }

    return $manifest
}

function Get-RocksSha256 {
    param([Parameter(Mandatory)][string] $Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Downloads one source archive, verifies its SHA-256 and extracts it into Destination (stripping the single top-level directory inside the archive).
function Expand-RocksVerifiedSource {
    param(
        [Parameter(Mandatory)] $Source,
        [Parameter(Mandatory)][string] $Downloads,
        [Parameter(Mandatory)][string] $Destination)

    New-Item -ItemType Directory -Force -Path $Downloads | Out-Null
    $archive = Join-Path $Downloads ("{0}.tar.gz" -f $Source.sha256)
    if (-not (Test-Path -LiteralPath $archive)) {
        $partial = "$archive.partial"
        Invoke-WebRequest -Uri $Source.url -OutFile $partial
        Move-Item -LiteralPath $partial -Destination $archive
    }

    $actual = Get-RocksSha256 $archive
    if ($actual -ne $Source.sha256) {
        Remove-Item -LiteralPath $archive
        throw "SHA-256 mismatch for $($Source.url): expected $($Source.sha256), got $actual"
    }

    $unpack = "$Destination.unpack"
    if (Test-Path -LiteralPath $unpack) {
        Remove-Item -LiteralPath $unpack -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $unpack | Out-Null
    tar -xzf $archive -C $unpack
    $roots = @(Get-ChildItem -LiteralPath $unpack)
    if ($roots.Count -ne 1 -or -not $roots[0].PSIsContainer) {
        throw "$($Source.url) must contain exactly one top-level directory"
    }

    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Move-Item -LiteralPath $roots[0].FullName -Destination $Destination
    Remove-Item -LiteralPath $unpack -Recurse -Force
}

# Builds the source tree from the manifest build's main source and dependency sources (at build.dependencies[].path) and applies the patches.
function Initialize-RocksSourceTree {
    param(
        [Parameter(Mandatory)] $Manifest,
        [Parameter(Mandatory)][string] $LibraryDirectory,
        [Parameter(Mandatory)][string] $WorkDirectory)

    $downloads = Join-Path $WorkDirectory 'downloads'
    $source = Join-Path $WorkDirectory 'source'
    Expand-RocksVerifiedSource -Source $Manifest.build.source -Downloads $downloads -Destination $source
    foreach ($dependency in $Manifest.build.dependencies) {
        $target = Join-Path $source $dependency.path
        if ((Test-Path -LiteralPath $target) -and @(Get-ChildItem -LiteralPath $target -Force).Count -ne 0) {
            throw "dependency path $($dependency.path) is not empty in the main source archive"
        }

        Expand-RocksVerifiedSource -Source $dependency -Downloads $downloads -Destination $target
    }

    foreach ($patch in $Manifest.build.patches) {
        git -C $source apply --whitespace=nowarn (Join-Path $LibraryDirectory $patch)
    }

    return $source
}

# Checks that every file of the source directory committed to the repository (upstreamDirectory) matches the same path in the build source tree byte for byte.
# So the public headers Rocks includes and the binaries it publishes come from the same sources (the match between committed files and the manifest SHA-256
# is checked by the third-party policy check).
function Assert-RocksCommittedFiles {
    param(
        [Parameter(Mandatory)] $Manifest,
        [Parameter(Mandatory)][string] $LibraryDirectory,
        [Parameter(Mandatory)][string] $SourceDirectory)

    $upstream = Join-Path $LibraryDirectory $Manifest.upstreamDirectory
    foreach ($file in Get-ChildItem -LiteralPath $upstream -Recurse -File) {
        $relative = [IO.Path]::GetRelativePath($upstream, $file.FullName)
        $path = Join-Path $SourceDirectory $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "committed file $relative does not exist in the build source"
        }

        if ((Get-RocksSha256 $path) -ne (Get-RocksSha256 $file.FullName)) {
            throw "committed file $relative differs from the build source"
        }
    }
}
# Imports the environment of the Visual Studio C++ tools (vcvarsall x64) into this process.
function Import-RocksMsvcEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) {
        throw 'Visual Studio C++ tools (VC.Tools.x86.x64) were not found'
    }

    $vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvarsall.bat'
    $lines = cmd /d /c "`"$vcvars`" x64 >nul && set"
    foreach ($line in $lines) {
        $equals = $line.IndexOf('=')
        if ($equals -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $equals), $line.Substring($equals + 1))
        }
    }
}

# Packs the stage directory files with their roles into an archive and writes the host entry JSON. Files is @{ '<path relative to stage>' = '<role>' }, and
# any other file in stage is an error (archive contents = manifest file list).
function New-RocksToolArchive {
    param(
        [Parameter(Mandatory)] $Manifest,
        [Parameter(Mandatory)][string] $StageDirectory,
        [Parameter(Mandatory)][hashtable] $Files,
        [Parameter(Mandatory)][string] $OutputDirectory)

    $staged = @(Get-ChildItem -LiteralPath $StageDirectory -Recurse -File | ForEach-Object {
            [IO.Path]::GetRelativePath($StageDirectory, $_.FullName).Replace('\', '/')
        })
    foreach ($path in $staged) {
        if (-not $Files.ContainsKey($path)) {
            throw "staged file $path has no role"
        }
    }

    foreach ($path in $Files.Keys) {
        if ($staged -notcontains $path) {
            throw "file $path was not staged"
        }
    }

    $name = $Manifest.name
    $revision = $Manifest.build.revision
    $asset = "$name-$($Manifest.version)-r$revision-$script:RocksToolHost.tar.gz"
    $tag = "third-party-$name-$($Manifest.version)-r$revision"
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $archive = Join-Path $OutputDirectory $asset
    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive
    }

    $top = @(Get-ChildItem -LiteralPath $StageDirectory | ForEach-Object Name | Sort-Object)
    tar -czf $archive -C $StageDirectory @top

    $entries = foreach ($path in ($Files.Keys | Sort-Object { $_ } -CaseSensitive)) {
        $full = Join-Path $StageDirectory $path
        [ordered]@{ path = $path; sha256 = (Get-RocksSha256 $full); size = (Get-Item -LiteralPath $full).Length; role = $Files[$path] }
    }

    # Publishing goes to this repository's GitHub Releases (the workflow reads owner/repo from GITHUB_REPOSITORY, local runs from the origin remote).
    $repository = $env:GITHUB_REPOSITORY
    if (-not $repository) {
        $origin = git -C $PSScriptRoot remote get-url origin
        if ($origin -notmatch 'github\.com[:/]([^/]+/[^/]+?)(\.git)?$') {
            throw "cannot derive the GitHub repository from origin '$origin' (set GITHUB_REPOSITORY)"
        }

        $repository = $Matches[1]
    }
    $hostEntry = [ordered]@{
        archive = [ordered]@{
            url = "https://github.com/$repository/releases/download/$tag/$asset"
            sha256 = Get-RocksSha256 $archive
            size = (Get-Item -LiteralPath $archive).Length
        }
        files = @($entries)
    }
    $json = [ordered]@{ $script:RocksToolHost = $hostEntry } | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText("$archive.host.json", $json.Replace("`r`n", "`n") + "`n")
    Write-Host "archive: $archive"
    Write-Host "sha256:  $($hostEntry.archive.sha256)"
    Write-Host "size:    $($hostEntry.archive.size)"
    Write-Host "release: $tag"
    return $archive
}
