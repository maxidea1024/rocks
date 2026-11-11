$ErrorActionPreference = 'Stop'
# 배포 archive에서 네이티브 실행 파일만 추출한다. 패키지 언어의 런타임/설치 코드는 실행하지 않는다.
$platform = if ($IsWindows) { 'windows' } elseif ($IsMacOS) { 'macos' } else { throw 'Windows/macOS hosts only.' }
$architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
$packages = @{
    'windows-X64' = @('https://files.pythonhosted.org/packages/3e/44/274e4b20ffab7707cdca5ddbf109c70789e8eb0bd49233318b9adb1da0c3/clang_format-21.1.6-py2.py3-none-win_amd64.whl', '072e0d611dfc7431d744dc90389aaaa626c26507581e692df60f2000695a92f6')
    'macos-Arm64' = @('https://files.pythonhosted.org/packages/f8/94/46762b38563b4aea840c706149f7d5b3eeed450c0ba874bb77b0de635674/clang_format-21.1.6-py2.py3-none-macosx_11_0_arm64.whl', '74880cb25886c1bb4a95307efeb40f19eba3f1fd86ddee88166814b9ed2600ef')
    'macos-X64' = @('https://files.pythonhosted.org/packages/dd/5b/2c5ae61dd72dc3d0a85af09e4ca007140d804b57a9fe2c9da44b88b62a13/clang_format-21.1.6-py2.py3-none-macosx_10_9_x86_64.whl', 'a0cea6afea00448396eaff4e10a569631a4b06582553e3c44c5c040ec83c6b01')
}
$package = $packages["$platform-$architecture"]
if (-not $package) { throw "Unsupported formatter platform: $platform-$architecture" }
$destination = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../out/toolchains/clang-format-21.1.6'))
[IO.Directory]::CreateDirectory($destination) | Out-Null
$archive = Join-Path $destination 'package.zip'
Invoke-WebRequest -Uri $package[0] -OutFile $archive
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package[1]) { throw 'Formatter archive SHA256 mismatch.' }
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
if ($LASTEXITCODE -ne 0 -or $version -notmatch 'clang-format version 21\.1\.6(?:\s|$)') { throw "Unexpected formatter version: $version" }
$env:ROCKS_CLANG_FORMAT = $binary
if ($env:GITHUB_ENV) { "ROCKS_CLANG_FORMAT=$binary" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append }
Write-Output $binary
