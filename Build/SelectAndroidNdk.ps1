$ErrorActionPreference = 'Stop'
# Selects the Android NDK pinned in Build/Toolchain.json (android.ndk.version) as ANDROID_NDK_HOME and ANDROID_NDK_ROOT
# (Docs/Design/ClangVersionSupport.ko.md section 6.3). CI runner images default to an older NDK whose clang is below the Android
# minimum in the same table, so CI selects the pinned one explicitly. ANDROID_NDK_LATEST_HOME is not used: it changes with image
# updates. When the image does not carry the pinned version, sdkmanager installs it into the Android SDK.
$version = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Toolchain.json') -Raw | ConvertFrom-Json).android.ndk.version
if (-not $version) { throw 'Build/Toolchain.json does not pin android.ndk.version.' }
$sdk = @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME) | Where-Object { $_ } | Select-Object -First 1
if (-not $sdk) { throw 'ANDROID_SDK_ROOT or ANDROID_HOME must name the Android SDK.' }
$ndk = Join-Path (Join-Path $sdk 'ndk') $version
if (-not (Test-Path -LiteralPath (Join-Path $ndk 'source.properties'))) {
    $sdkmanager = Join-Path $sdk ('cmdline-tools/latest/bin/sdkmanager' + $(if ($IsWindows) { '.bat' } else { '' }))
    if (-not (Test-Path -LiteralPath $sdkmanager)) { throw "Android NDK $version is missing and sdkmanager was not found at $sdkmanager." }
    # sdkmanager asks for each license; the answers are piped in.
    (@('y') * 32) | & $sdkmanager --sdk_root=$sdk --install "ndk;$version" | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $ndk 'source.properties'))) { throw "sdkmanager could not install Android NDK $version." }
}

$revision = (Get-Content -LiteralPath (Join-Path $ndk 'source.properties') | Where-Object { $_ -match '^Pkg\.Revision\s*=' }) -replace '^Pkg\.Revision\s*=\s*', ''
if ($revision.Trim() -ne $version) { throw "Android NDK at $ndk reports revision '$revision', expected $version." }
$env:ANDROID_NDK_HOME = $ndk
$env:ANDROID_NDK_ROOT = $ndk
if ($env:GITHUB_ENV) {
    "ANDROID_NDK_HOME=$ndk" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    "ANDROID_NDK_ROOT=$ndk" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}

Write-Output $ndk
