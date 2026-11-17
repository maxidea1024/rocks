# Offline build of the SPIRV-Cross C API shared library (spirv-cross-c-shared) (Windows x64). Docs/Design/ShaderSystem.ko.md section 8, item 4.
# The input is the build entry (pinned tag, source SHA-256, patches) of ThirdParty.json one folder up, compiled outside the engine build with the upstream
# build settings (exceptions and RTTI) (the prebuilt binary section of the warning policy). The result is a tar.gz archive and a manifest hosts entry JSON. Does not publish.
#
# Usage: pwsh Engine/Source/ThirdParty/SpirvCross/Build/Build.ps1 [-WorkDirectory C:\rtb\SpirvCross] [-OutputDirectory <dir>]
# Requires: CMake, Ninja, Visual Studio C++ tools, git (to apply patches).
param(
    [string] $WorkDirectory = (Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) 'rtb\SpirvCross'),
    [string] $OutputDirectory = '',
    [int] $Jobs = [Environment]::ProcessorCount)

. (Join-Path $PSScriptRoot '../../../../../Build/ThirdPartyTools/ToolBuild.ps1')
$library = Split-Path -Parent $PSScriptRoot
$manifest = Read-RocksToolManifest $library
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $WorkDirectory 'archives'
}

$source = Initialize-RocksSourceTree -Manifest $manifest -LibraryDirectory $library -WorkDirectory $WorkDirectory
Assert-RocksCommittedFiles -Manifest $manifest -LibraryDirectory $library -SourceDirectory $source
Import-RocksMsvcEnvironment

$build = Join-Path $WorkDirectory 'build'
cmake -G Ninja -S $source -B $build -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7" "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO" `
    -DSPIRV_CROSS_SHARED=ON -DSPIRV_CROSS_STATIC=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SKIP_INSTALL=ON
cmake --build $build --target spirv-cross-c-shared -j $Jobs

$stage = Join-Path $WorkDirectory 'stage'
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}

New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin') | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'spirv-cross-c-shared.dll') -Destination (Join-Path $stage 'bin')
Copy-Item -LiteralPath (Join-Path $build 'spirv-cross-c-shared.pdb') -Destination (Join-Path $stage 'bin')
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination $stage
New-RocksToolArchive -Manifest $manifest -StageDirectory $stage -OutputDirectory $OutputDirectory -Files @{
    'bin/spirv-cross-c-shared.dll' = 'runtime'
    'bin/spirv-cross-c-shared.pdb' = 'symbols'
    'LICENSE' = 'notice'
} | Out-Null
