# SPIRV-Tools C API 공유 라이브러리 오프라인 빌드다(Windows x64). Docs/Design/ShaderSystem.ko.md 8절 4항.
# libspirv.h의 C API(spvValidate·어셈블·역어셈블과 최적화기 C API spvOptimizer*)를 공유 라이브러리 하나(SPIRV-Tools-c.dll)로 만든다.
# 업스트림의 공유 빌드(BUILD_SHARED_LIBS)는 SPIRV-Tools-opt가 core의 C++ 내부 심볼을 DLL 경계 너머로 쓰므로 Windows에서 링크되지
# 않는다. 그래서 업스트림 정적 라이브러리(SPIRV-Tools-static·SPIRV-Tools-opt)를 SPIRV_TOOLS_SHAREDLIB·SPIRV_TOOLS_IMPLEMENTATION으로
# 컴파일해(C API에 dllexport) CMake 래퍼(같은 폴더의 SpirvToolsC.cmake)에서 whole-archive로 묶는다. 업스트림 원본은 고치지 않는다.
# SPIRV-Headers는 DXC 고정 릴리스의 submodule 커밋으로 맞춘다(manifest build.dependencies, spirv-val과 DXC의 SPIR-V 버전 정합).
# 업스트림 CMake가 Python 3(PATH)으로 문법 표를 생성한다. 게시는 하지 않는다.
#
# 사용: pwsh Engine/Source/ThirdParty/SpirvTools/Build/Build.ps1 [-WorkDirectory C:\rtb\SpirvTools] [-OutputDirectory <dir>]
# 필요: CMake 3.24 이상, Ninja, Visual Studio C++ 도구, Python 3, git(patch 적용).
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
