# Offline build of the SPIRV-Tools C API shared library (Windows x64). Docs/Design/ShaderSystem section 8, item 4.
# Builds the C API of libspirv.h (spvValidate, assembly, disassembly and the optimizer C API spvOptimizer*) as one shared library (SPIRV-Tools-c.dll).
# The upstream shared build (BUILD_SHARED_LIBS) does not link on Windows because SPIRV-Tools-opt uses core's internal C++ symbols across the DLL
# boundary. So the upstream static libraries (SPIRV-Tools-static, SPIRV-Tools-opt) are compiled with SPIRV_TOOLS_SHAREDLIB and SPIRV_TOOLS_IMPLEMENTATION
# (dllexport on the C API) and bundled with whole-archive in a CMake wrapper (SpirvToolsC.cmake in the same folder). Upstream sources are not modified.
# SPIRV-Headers is aligned to the submodule commit of the pinned DXC release (manifest build.dependencies; spirv-val and DXC agree on the SPIR-V version).
# The upstream CMake generates grammar tables with Python 3 (PATH). Does not publish.
#
# Usage: pwsh Engine/Source/ThirdParty/SpirvTools/Build/Build.ps1 [-WorkDirectory C:\rtb\SpirvTools] [-OutputDirectory <dir>]
# Requires: CMake 3.24 or later, Ninja, Visual Studio C++ tools, Python 3, git (to apply patches).
param(
    [string] $WorkDirectory = (Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) 'rtb\SpirvTools'),
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

$wrapper = Join-Path $WorkDirectory 'wrapper'
New-Item -ItemType Directory -Force -Path $wrapper | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'SpirvToolsC.cmake') -Destination (Join-Path $wrapper 'CMakeLists.txt') -Force
$build = Join-Path $WorkDirectory 'build'
cmake -G Ninja -S $wrapper -B $build -DCMAKE_BUILD_TYPE=Release "-DROCKS_SPIRV_TOOLS_SOURCE=$source" `
    "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7" "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO"
cmake --build $build --target SPIRV-Tools-c -j $Jobs

$stage = Join-Path $WorkDirectory 'stage'
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}

New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin') | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'SPIRV-Tools-c.dll') -Destination (Join-Path $stage 'bin')
Copy-Item -LiteralPath (Join-Path $build 'SPIRV-Tools-c.pdb') -Destination (Join-Path $stage 'bin')
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination $stage
New-RocksToolArchive -Manifest $manifest -StageDirectory $stage -OutputDirectory $OutputDirectory -Files @{
    'bin/SPIRV-Tools-c.dll' = 'runtime'
    'bin/SPIRV-Tools-c.pdb' = 'symbols'
    'LICENSE' = 'notice'
} | Out-Null
