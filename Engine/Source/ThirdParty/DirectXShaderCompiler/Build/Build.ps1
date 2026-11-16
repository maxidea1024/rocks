# DirectXShaderCompiler(DXC) 오프라인 빌드다(Windows x64). Docs/Design/ShaderSystem.ko.md 8절 4항, UE 5.8
# ThirdParty/ShaderConductor/Build_ShaderConductor_Win64.bat와 같은 역할(엔진 빌드 밖 CMake 빌드, 결과 공유 라이브러리만 배포)이다.
# 고정 태그의 원본과 submodule(SPIRV-Headers·SPIRV-Tools·DirectX-Headers) 원본을 manifest build에서 받아 SHA-256을 검사하고,
# 업스트림 cache(cmake/caches/PredefinedParams.cmake)에 시험만 끈 설정으로 dxcompiler.dll과 dxc.exe를 만든다. 업스트림 빌드는
# 예외·RTTI를 쓰며(LLVM 기반), Python 3(PATH)으로 표를 생성한다. 저장소에 커밋된 생성 원본을 다시 만들지 않는다(HLSL_DISABLE_SOURCE_GENERATION,
# git checkout이 아닌 archive라 줄끝 비교 HLSL_AUTOCRLF는 끈다). MSVC는 업스트림이 /Zi로 PDB를 만든다. dxil.dll은 만들지 않는다(1.8.2505부터 내장 validator가 서명한다).
# 게시는 하지 않는다.
#
# 사용: pwsh Engine/Source/ThirdParty/DirectXShaderCompiler/Build/Build.ps1 [-WorkDirectory C:\rtb\Dxc] [-OutputDirectory <dir>]
# 필요: CMake, Ninja, Visual Studio C++ 도구와 Windows SDK, Python 3, git(patch 적용). 경로가 긴 LLVM 목적 파일 때문에 작업
# 디렉터리는 짧은 경로(기본 <드라이브>\rtb\Dxc)에 둔다.
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
