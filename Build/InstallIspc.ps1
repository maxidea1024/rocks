param([string]$ToolchainRoot = '')
$ErrorActionPreference = 'Stop'
# Installs the pinned Intel ISPC compiler (Docs/Design/BuildTargets section 3.12, BD45): the ISPC sources of the texture compressor
# (AssetSystem section 12.17.6) build only with this version. The version and the release archive URL and SHA-256 per host come from
# Build/Toolchain.json (ispc.packages); only the ispc executable is extracted after the SHA-256 check, nothing else in the archive is
# run. -ToolchainRoot puts ispc-<version> under another directory (default <repo>/out/toolchains), as in Build/InstallFormatter.ps1.
$platform = if ($IsWindows) { 'windows' } elseif ($IsMacOS) { 'macos' } elseif ($IsLinux) { 'linux' } else { throw 'Unsupported host.' }
$architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
$ispc = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Toolchain.json') -Raw | ConvertFrom-Json).ispc
$expected = $ispc.version
$package = $ispc.packages."$platform-$architecture"
if (-not $package) { throw "Unsupported ISPC platform: $platform-$architecture (Build/Toolchain.json ispc.packages)" }
if ($package.url -notmatch "/ispc-v$([regex]::Escape($expected))-") { throw "ISPC archive does not match Build/Toolchain.json ispc $expected." }
if (-not $ToolchainRoot) { $ToolchainRoot = Join-Path $PSScriptRoot '../out/toolchains' }
# Resolve against the PowerShell location (not the process directory) so a relative -ToolchainRoot means what the caller typed.
$ToolchainRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ToolchainRoot)
$destination = [IO.Path]::GetFullPath((Join-Path $ToolchainRoot "ispc-$expected"))
$name = if ($IsWindows) { 'ispc.exe' } else { 'ispc' }
$binary = Join-Path $destination $name
if (-not (Test-Path -LiteralPath $binary)) {
    $staging = "$destination.partial"
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    [IO.Directory]::CreateDirectory($staging) | Out-Null
    $archiveName = [IO.Path]::GetFileName(([Uri]$package.url).AbsolutePath)
    $archive = Join-Path $staging $archiveName
    try { Invoke-WebRequest -Uri $package.url -OutFile $archive }
    catch { throw "Could not download ISPC $expected ($($_.Exception.Message)). Next: check the network or proxy (HTTPS_PROXY) and run the setup again." }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package.sha256) { throw 'ISPC archive SHA256 mismatch.' }
    # The archives hold one top directory (ispc-v<version>-<host>) with bin/ispc. The bsdtar shipped with Windows 10+ and the tar of
    # macOS and Linux read both zip and tar.gz; only the executable is extracted.
    $top = [IO.Path]::GetFileName($archiveName) -replace '\.(zip|tar\.gz)$', ''
    $member = "$top/bin/$name"
    $extracted = Join-Path $staging 'tree'
    [IO.Directory]::CreateDirectory($extracted) | Out-Null
    # On Windows use the system bsdtar explicitly: a GNU tar earlier on PATH (MSYS2, Cygwin) cannot read zip and treats C:\ as a remote host.
    $tar = if ($IsWindows) { Join-Path $env:SystemRoot 'System32\tar.exe' } else { 'tar' }
    & $tar -xf $archive -C $extracted $member
    if ($LASTEXITCODE -ne 0) { throw "tar could not extract $member from the ISPC archive (exit code $LASTEXITCODE)." }
    $source = Join-Path $extracted $member
    if (-not (Test-Path -LiteralPath $source)) { throw 'ISPC executable missing from the pinned archive.' }
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    Move-Item -LiteralPath $source -Destination $binary -Force
    Remove-Item -LiteralPath $staging -Recurse -Force
}
if (-not $IsWindows) { & chmod +x $binary; if ($LASTEXITCODE -ne 0) { throw 'chmod failed.' } }
$version = & $binary --version
if ($LASTEXITCODE -ne 0 -or "$version" -notmatch "\(Intel\(r\) ISPC\), $([regex]::Escape($expected))(?:\s|$)") { throw "Unexpected ISPC version: $version" }
$env:ROCKS_ISPC = $binary
if ($env:GITHUB_ENV) { "ROCKS_ISPC=$binary" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append }
Write-Output $binary
