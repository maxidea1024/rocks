param([string]$ToolchainRoot = '')
$ErrorActionPreference = 'Stop'
# Extracts only the native executables from the release archive. Runtime or install code of the package language is never run.
# -ToolchainRoot puts clang-format-<version> under another directory (default <repo>/out/toolchains), as in Build/InstallLlvm.ps1.
$platform = if ($IsWindows) { 'windows' } elseif ($IsMacOS) { 'macos' } elseif ($IsLinux) { 'linux' } else { throw 'Unsupported host.' }
$architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
# The pinned version and the wheel URL and SHA-256 per host come from Build/Toolchain.json (clangFormat; Docs/Design/ClangVersionSupport.ko.md
# section 6.1). Build/Setup.sh reads the same entries, so both install the same file.
$formatter = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Toolchain.json') -Raw | ConvertFrom-Json).clangFormat
$expected = $formatter.version
$package = $formatter.packages."$platform-$architecture"
if (-not $package) { throw "Unsupported formatter platform: $platform-$architecture (Build/Toolchain.json clangFormat.packages)" }
if ($package.url -notmatch "/clang_format-$([regex]::Escape($expected))-") { throw "Formatter wheel does not match Build/Toolchain.json clang-format $expected." }
if (-not $ToolchainRoot) { $ToolchainRoot = Join-Path $PSScriptRoot '../out/toolchains' }
# Resolve against the PowerShell location (not the process directory) so a relative -ToolchainRoot means what the caller typed.
$ToolchainRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ToolchainRoot)
$destination = [IO.Path]::GetFullPath((Join-Path $ToolchainRoot "clang-format-$expected"))
[IO.Directory]::CreateDirectory($destination) | Out-Null
$archive = Join-Path $destination 'package.zip'
try { Invoke-WebRequest -Uri $package.url -OutFile $archive }
catch { throw "Could not download clang-format ($($_.Exception.Message)). Next: check the network or proxy (HTTPS_PROXY) and run the setup again." }
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package.sha256) { throw 'Formatter archive SHA256 mismatch.' }
$name = if ($IsWindows) { 'clang-format.exe' } else { 'clang-format' }
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $entry = $zip.GetEntry("clang_format/data/bin/$name")
    if (-not $entry) { throw 'Formatter binary missing from pinned archive.' }
    $binary = Join-Path $destination $name
    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $binary, $true)
} finally { $zip.Dispose() }
if (-not $IsWindows) { & chmod +x $binary; if ($LASTEXITCODE -ne 0) { throw 'chmod failed.' } }
$version = & $binary --version
if ($LASTEXITCODE -ne 0 -or $version -notmatch "clang-format version $([regex]::Escape($expected))(?:\s|$)") { throw "Unexpected formatter version: $version" }
$env:ROCKS_CLANG_FORMAT = $binary
if ($env:GITHUB_ENV) { "ROCKS_CLANG_FORMAT=$binary" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append }
Write-Output $binary
