# Offline build of DirectXShaderCompiler (DXC) (Windows x64). Docs/Design/ShaderSystem section 8, item 4. Same role as UE 5.8
# ThirdParty/ShaderConductor/Build_ShaderConductor_Win64.bat (CMake build outside the engine build, shipping only the resulting shared libraries).
# Downloads the pinned tag sources and submodule (SPIRV-Headers, SPIRV-Tools, DirectX-Headers) sources from the manifest build, verifies SHA-256,
# and builds dxcompiler.dll and dxc.exe with the upstream cache (cmake/caches/PredefinedParams.cmake) with only tests turned off. The upstream build
# uses exceptions and RTTI (LLVM based) and generates tables with Python 3 (PATH). Generated sources committed upstream are not regenerated (HLSL_DISABLE_SOURCE_GENERATION;
# it is an archive, not a git checkout, so the line ending comparison HLSL_AUTOCRLF is off). For MSVC, upstream produces PDBs with /Zi. dxil.dll is not built (since 1.8.2505 the built-in validator signs).
# Does not publish.
#
# Usage: pwsh Engine/Source/ThirdParty/DirectXShaderCompiler/Build/Build.ps1 [-WorkDirectory C:\rtb\Dxc] [-OutputDirectory <dir>]
# Requires: CMake, Ninja, Visual Studio C++ tools and the Windows SDK, Python 3, git (to apply patches). Because of long LLVM object file paths, keep the work
# directory on a short path (default <drive>\rtb\Dxc).
param(
    [string] $WorkDirectory = (Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) 'rtb\Dxc'),
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
cmake -G Ninja -S $source -B $build -C (Join-Path $source 'cmake/caches/PredefinedParams.cmake') -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG" "-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG" `
    "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO" `
    "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO" `
    -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_TESTS=OFF -DLLVM_APPEND_VC_REV=OFF `
    -DHLSL_DISABLE_SOURCE_GENERATION=ON -DHLSL_AUTOCRLF=OFF
cmake --build $build --target dxcompiler dxc -j $Jobs

$stage = Join-Path $WorkDirectory 'stage'
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}

New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin') | Out-Null
foreach ($name in 'dxcompiler.dll', 'dxcompiler.pdb', 'dxc.exe', 'dxc.pdb') {
    Copy-Item -LiteralPath (Join-Path $build "bin/$name") -Destination (Join-Path $stage 'bin')
}

foreach ($name in 'LICENSE.TXT', 'ThirdPartyNotices.txt') {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $stage
}

New-RocksToolArchive -Manifest $manifest -StageDirectory $stage -OutputDirectory $OutputDirectory -Files @{
    'bin/dxcompiler.dll' = 'runtime'
    'bin/dxcompiler.pdb' = 'symbols'
    'bin/dxc.exe' = 'tool'
    'bin/dxc.pdb' = 'symbols'
    'LICENSE.TXT' = 'notice'
    'ThirdPartyNotices.txt' = 'notice'
} | Out-Null
