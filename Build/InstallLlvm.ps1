param([string]$Version = '', [string]$ToolchainRoot = '')
$ErrorActionPreference = 'Stop'
# Installs a pinned LLVM on Windows hosts (CI runner images ship whatever LLVM they carry, e.g. 20.1.8). The default version and the
# installer SHA-256 of every pinned version come from Build/Toolchain.json (Docs/Design/ClangVersionSupport.ko.md section 6.1);
# -Version picks another pinned release. Pair the LLVM with an MSVC toolset whose STL accepts it (STL 14.52 requires Clang 22).
# The official installer is unpacked with 7-Zip after the SHA-256 check; its install code is never run. The bin directory goes
# first on PATH so Rocks.Build and the MetaCompiler discover this clang-cl, lld-link and llvm-lib together.
# -ToolchainRoot puts llvm-<version> under another directory (default <repo>/out/toolchains). A fixed location outside out/ (for example
# %LOCALAPPDATA%/Rocks/toolchains) keeps a PATH entry valid when out/ is deleted (Docs/Guides/BuildEnvironment.ko.md).
if (-not $IsWindows) { throw 'Windows hosts only (macOS uses the Xcode toolchain).' }
$architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
if ($architecture -ne 'X64') { throw "Unsupported LLVM host architecture: $architecture" }
$table = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Toolchain.json') -Raw | ConvertFrom-Json
if (-not $Version) { $Version = $table.llvm.windows.version }
$sha256 = $table.llvm.windows.installers.$Version
if (-not $sha256) { throw "LLVM $Version is not pinned in Build/Toolchain.json." }
$version = $Version
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/LLVM-$version-win64.exe"
if (-not $ToolchainRoot) { $ToolchainRoot = Join-Path $PSScriptRoot '../out/toolchains' }
# Resolve against the PowerShell location (not the process directory) so a relative -ToolchainRoot means what the caller typed.
$ToolchainRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ToolchainRoot)
$destination = [IO.Path]::GetFullPath((Join-Path $ToolchainRoot "llvm-$version"))
$bin = Join-Path $destination 'bin'
$compiler = Join-Path $bin 'clang-cl.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    $staging = "$destination.partial"
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    [IO.Directory]::CreateDirectory($staging) | Out-Null
    $archive = Join-Path $staging 'installer.exe'
    Invoke-WebRequest -Uri $url -OutFile $archive
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha256) { throw 'LLVM installer SHA256 mismatch.' }
    $sevenZip = (Get-Command 7z -ErrorAction SilentlyContinue)?.Source
    if (-not $sevenZip) {
        $installed = Join-Path $env:ProgramFiles '7-Zip/7z.exe'
        if (-not (Test-Path -LiteralPath $installed)) { throw '7-Zip (7z) is required to unpack the LLVM installer.' }
        $sevenZip = $installed
    }
    & $sevenZip x $archive "-o$staging/tree" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed with exit code $LASTEXITCODE." }
    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    Move-Item -LiteralPath (Join-Path $staging 'tree') -Destination $destination
    Remove-Item -LiteralPath $staging -Recurse -Force
}
foreach ($tool in @('clang-cl.exe', 'lld-link.exe', 'llvm-lib.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $bin $tool))) { throw "Pinned LLVM is missing $tool." }
}
$reported = (& $compiler --version | Select-Object -First 1)
if ($LASTEXITCODE -ne 0 -or $reported -notmatch "clang version $([regex]::Escape($version))(?:\s|$)") { throw "Unexpected LLVM version: $reported" }
$env:PATH = "$bin$([IO.Path]::PathSeparator)$env:PATH"
if ($env:GITHUB_PATH) { $bin | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append }
Write-Output $compiler
