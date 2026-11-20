$ErrorActionPreference = 'Stop'
# Selects the Android NDK pinned in Build/Toolchain.json (android.ndk.version) as ANDROID_NDK_HOME and ANDROID_NDK_ROOT
# (Docs/Design/ClangVersionSupport.ko.md section 6.3). CI runner images default to an older NDK whose clang is below the Android
# minimum in the same table, so CI selects the pinned one explicitly. ANDROID_NDK_LATEST_HOME is not used: it changes with image
# updates. When the image does not carry the pinned version, sdkmanager installs it into the Android SDK.
$version = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Toolchain.json') -Raw | ConvertFrom-Json).android.ndk.version
if (-not $version) { throw 'Build/Toolchain.json does not pin android.ndk.version.' }
# ANDROID_HOME comes first, as in Rocks.Build's NDK discovery (NativeToolchains.FindDefaultNdk).
$sdk = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT) | Where-Object { $_ } | Select-Object -First 1
if (-not $sdk) { throw 'ANDROID_HOME or ANDROID_SDK_ROOT must name the Android SDK.' }
$ndk = Join-Path (Join-Path $sdk 'ndk') $version
$prebuilt = if ($IsWindows) { 'windows-x86_64' } elseif ($IsMacOS) { 'darwin-x86_64' } else { throw 'Windows/macOS hosts only.' }
$compiler = Join-Path $ndk ("toolchains/llvm/prebuilt/$prebuilt/bin/clang++" + $(if ($IsWindows) { '.exe' } else { '' }))
if (-not (Test-Path -LiteralPath (Join-Path $ndk 'source.properties'))) {
    $sdkmanager = Join-Path $sdk ('cmdline-tools/latest/bin/sdkmanager' + $(if ($IsWindows) { '.bat' } else { '' }))
    if (-not (Test-Path -LiteralPath $sdkmanager)) { throw "Android NDK $version is missing and sdkmanager was not found at $sdkmanager." }
    # sdkmanager needs Java 17 or newer; the runner images name their JDKs JAVA_HOME_<major>_<architecture>.
    $java = Get-ChildItem env: | Where-Object { $_.Name -match '^JAVA_HOME_(21|17)_' -and $_.Value -and (Test-Path -LiteralPath $_.Value) } |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($java) { $env:JAVA_HOME = $java.Value }
    # sdkmanager asks for each license; the answers are piped in.
    (@('y') * 32) | & $sdkmanager --sdk_root=$sdk --install "ndk;$version" | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $ndk 'source.properties'))) { throw "sdkmanager could not install Android NDK $version." }
}

$revisionLine = Get-Content -LiteralPath (Join-Path $ndk 'source.properties') | Where-Object { $_ -match '^\s*Pkg\.Revision\s*=' } | Select-Object -First 1
if (-not $revisionLine) { throw "Android NDK at $ndk has no Pkg.Revision in source.properties." }
# The revision must equal the pinned version exactly, suffix included: a beta NDK reports e.g. 29.0.13599879-beta1 and is a different NDK.
$revision = ($revisionLine -replace '^\s*Pkg\.Revision\s*=\s*', '').Trim()
if ($revision -ne $version) { throw "Android NDK at $ndk reports revision '$revision', expected $version." }
if (-not (Test-Path -LiteralPath $compiler)) { throw "Android NDK at $ndk has no clang++ at $compiler." }
$env:ANDROID_NDK_HOME = $ndk
$env:ANDROID_NDK_ROOT = $ndk
if ($env:GITHUB_ENV) {
    "ANDROID_NDK_HOME=$ndk" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    "ANDROID_NDK_ROOT=$ndk" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}

Write-Output $ndk
