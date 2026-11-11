param(
    [string]$ScratchRoot = [IO.Path]::GetTempPath(),
    [string]$Generator = ""
)

$ErrorActionPreference = "Stop"
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$caseRoot = Join-Path $ScratchRoot ("rocks-cmake-platform-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $caseRoot | Out-Null

$nativeArguments = @("-S", $repository, "-B", (Join-Path $caseRoot "native"), "-DROCKS_BUILD_OBJECT=OFF")
if ($Generator) {
    $nativeArguments += @("-G", $Generator)
}

$nativeOutput = (& cmake @nativeArguments 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "Native configure failed:`n$nativeOutput"
}

if ($IsWindows) {
    $expected = "Rocks target system: Windows (Win64)"
} elseif ($IsMacOS) {
    $expected = "Rocks target system: Darwin (Mac)"
} else {
    throw "This check requires a supported Windows or macOS host."
}

if (-not $nativeOutput.Contains($expected)) {
    throw "Native configure selected the wrong platform:`n$nativeOutput"
}
Write-Output "PASS native platform selection: $expected"

foreach ($target in @("iOS", "tvOS", "watchOS", "Linux", "WindowsStore")) {
    $output = (& cmake -S $repository -B (Join-Path $caseRoot $target) "-DCMAKE_SYSTEM_NAME=$target" 2>&1 | Out-String)
    $status = $LASTEXITCODE
    if ($status -eq 0 -or -not $output.Contains("Unsupported target system '$target'")) {
        throw "Unsupported target $target did not fail at platform selection (exit $status):`n$output"
    }
    Write-Output "PASS unsupported target rejected: $target"
}

# toolchain 파일은 project() 도중 CMAKE_SYSTEM_NAME을 바꿀 수 있다. 초기 검사를
# 통과한 이름을 덮어쓰는 상황에서도 최종 대상을 다시 검사해야 한다.
$toolchain = Join-Path $caseRoot "unsupported-toolchain.cmake"
Set-Content -LiteralPath $toolchain -Encoding ascii -Value 'set(CMAKE_SYSTEM_NAME Linux)'
$toolchainArguments = @("-S", $repository, "-B", (Join-Path $caseRoot "toolchain"),
    "-DCMAKE_SYSTEM_NAME=Windows", "-DCMAKE_TOOLCHAIN_FILE=$toolchain")
if ($Generator) {
    $toolchainArguments += @("-G", $Generator)
}
$toolchainOutput = (& cmake @toolchainArguments 2>&1 | Out-String)
$toolchainStatus = $LASTEXITCODE
if ($toolchainStatus -eq 0 -or -not $toolchainOutput.Contains("Unsupported target system 'Linux'")) {
    throw "Unsupported target from toolchain was not rejected (exit $toolchainStatus):`n$toolchainOutput"
}
Write-Output "PASS unsupported toolchain target rejected"

Write-Output "Configure outputs: $caseRoot"
