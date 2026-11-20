param([string]$Version = '21.1.6')
$ErrorActionPreference = 'Stop'
# Installs a pinned LLVM on Windows hosts (CI runner images ship whatever LLVM they carry, e.g. 20.1.8). The verified version is
# 21.1.6 (paired with ROCKS_MSVC_TOOLSET=14.44; STL 14.52 requires Clang 22). -Version picks another pinned release (22.1.8 runs the
# Clang 22 CI leg with the image's default MSVC — Docs/Design/ClangVersionSupport.ko.md section 6.3).
# The official installer is unpacked with 7-Zip after the SHA-256 check; its install code is never run. The bin directory goes
# first on PATH so Rocks.Build and the MetaCompiler discover this clang-cl, lld-link and llvm-lib together.
if (-not $IsWindows) { throw 'Windows hosts only (macOS uses the Xcode toolchain).' }
$architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
if ($architecture -ne 'X64') { throw "Unsupported LLVM host architecture: $architecture" }
$pinned = @{
    '21.1.6' = '11d721f66d46b216f69a4a9db75ac8b36933c50c59da83c67129a06740af22fc'
    '22.1.8' = '16e5709785fef73c854646241c4a92c5cd574318d1b33c63330dd7721903e55c'
}
if (-not $pinned.ContainsKey($Version)) { throw "LLVM $Version is not pinned (known: $($pinned.Keys -join ', '))." }
$version = $Version
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/LLVM-$version-win64.exe"
$sha256 = $pinned[$version]
$destination = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../out/toolchains/llvm-$version"))
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
