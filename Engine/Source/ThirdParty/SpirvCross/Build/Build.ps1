# SPIRV-Cross C API 공유 라이브러리(spirv-cross-c-shared) 오프라인 빌드다(Windows x64). Docs/Design/ShaderSystem.ko.md 8절 4항.
# 입력은 같은 폴더 위 ThirdParty.json의 build(고정 태그·원본 SHA-256·patch)이고, 업스트림 빌드 설정(예외·RTTI)으로 엔진 빌드 밖에서
# 컴파일한다(경고 정책의 미리 빌드한 바이너리 절). 결과는 tar.gz archive와 manifest hosts 항목 JSON이다. 게시는 하지 않는다.
#
# 사용: pwsh Engine/Source/ThirdParty/SpirvCross/Build/Build.ps1 [-WorkDirectory C:\rtb\SpirvCross] [-OutputDirectory <dir>]
# 필요: CMake, Ninja, Visual Studio C++ 도구, git(patch 적용).
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
