#pragma once

#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetCacheSettings.h>
#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Platform/File.h>

// 셰이더 job의 에셋 너겟 Record 값과 너겟 위치다(Docs/Design/ShaderSystem.ko.md 10절).
//
// Record: bucket NuggetBucketShader, namespace rocks.nuggets, key = job 키(ShaderJobKey.h),
// inputs·meta는 비어 있다. 값은 ShaderNuggetValue 번호 오름차순의 부분이다. 진단은 Record에 넣지
// 않고 실패한 job은 게시하지 않는다. 적중했는데 값 0(Code)이 없으면 손상이므로 다시 컴파일한다.
//
// 위치: root는 프로젝트가 있으면 프로젝트 루트, 없으면(엔진 셰이더만 — Sandbox·self-test·엔진 내장
// 라이브러리) 엔진 폴더(HostConfig FindEngineDirectory)다.
// `<root>/Config/AssetNuggets.json`(선택)을 읽고 기본 위치는 `<root>/Intermediate/AssetNuggets/`다.
//
// 사용 예:
//   String root(allocator);
//   if (auto found = ResolveShaderNuggetRoot(projectRoot, root); !found) { ... }
//   NuggetSystemClock clock;
//   NuggetCache cache(allocator, files, clock);
//   NuggetSettingsFailure failure;
//   if (auto opened = OpenShaderNuggetCache(cache, allocator, files, root.View(), &failure);
//   !opened) {
//       // 설정 파일 오류(failure.field·line·column) 또는 required backend 열기 실패
//   }
//   ShaderJobValues values(allocator);
//   ... values.code에 산출물 ...
//   Array<NuggetValue> record(allocator);
//   if (auto moved = MoveShaderValuesToNugget(values, record); moved) {
//       cache.Put(NuggetCachePut{NuggetBucketShader, key, record.View()});
//   }
namespace rocks::shader {

// 셰이더 Record의 ValueId다(10절 값 표). 번호는 고정이다(바꾸면 처리기 버전 Guid를 바꾼다).
enum class ShaderNuggetValue : uint32 {
    // 형식 산출물: DXIL·DXBC·SPIR-V byte, ESSL·MSL UTF-8 텍스트, metallib byte(SH1).
    Code = 0,
    // 정규화된 바인딩 layout(7.2절, SH2).
    BindingLayout = 1,
    // GLES 바인딩 머리·상수 버퍼 복사 표(7.2절 4항, SH2).
    GlesCopyTable = 2,
    // 디버그 심볼(SH4).
    DebugSymbols = 3
};

// 값 종류 수다(번호는 0부터 이어진다).
inline constexpr uint32 ShaderNuggetValueCount = 4;

// job 하나의 값이다. present는 ShaderNuggetValue 번호의 비트다(값이 있지만 빈 byte일 수 있다 —
// Code는 늘 있어야 한다).
struct ROCKS_SHADERCOMPILER_API ShaderJobValues {
    explicit ShaderJobValues(Allocator& allocator) noexcept
        : code(allocator), bindingLayout(allocator), glesCopyTable(allocator),
          debugSymbols(allocator) {
    }

    // 번호의 byte 배열이다.
    Array<uint8>& Part(ShaderNuggetValue value) noexcept;
    const Array<uint8>& Part(ShaderNuggetValue value) const noexcept;

    bool Has(ShaderNuggetValue value) const noexcept {
        return (present & (1u << static_cast<uint32>(value))) != 0;
    }

    // 값을 둔다(bytes를 옮긴다).
    void Set(ShaderNuggetValue value, Array<uint8>&& bytes) noexcept;

    void Reset() noexcept;

    Array<uint8> code;
    Array<uint8> bindingLayout;
    Array<uint8> glesCopyTable;
    Array<uint8> debugSymbols;
    uint32 present = 0;
};

// values를 Put 값 목록(번호 오름차순)으로 옮긴다(byte를 옮긴다 — values는 비워진다). out은 먼저
// 비운다. 오류: Code가 없으면 InvalidArgument(실패한 job은 게시하지 않는다), 할당 실패 OutOfMemory.
ROCKS_SHADERCOMPILER_API Result<void> MoveShaderValuesToNugget(ShaderJobValues& values,
                                                               Array<NuggetValue>& out) noexcept;

// 적중한 Record의 값 목록을 values로 옮긴다(byte를 옮긴다). values는 먼저 비운다. 오류(모두 손상 —
// 다시 컴파일한다): 값 0(Code)이 없으면 NotFound, 모르는 번호·중복·오름차순 아님은 InvalidArgument.
ROCKS_SHADERCOMPILER_API Result<void> MoveShaderValuesFromNugget(Array<NuggetValue>& record,
                                                                 ShaderJobValues& values) noexcept;

// 너겟 root를 out에 둔다. projectRoot가 비지 않으면 그것, 비면 엔진 폴더(FindEngineDirectory —
// 구분자
// '/')다. 오류: 엔진 폴더를 찾지 못하면 그 오류(NotFound 등), 할당 실패.
ROCKS_SHADERCOMPILER_API Result<void> ResolveShaderNuggetRoot(StringView projectRoot,
                                                              String& out) noexcept;

// root의 설정(ReadNuggetCacheSettings)을 읽어 cache를 연다(NuggetCache::Open). 오류: 설정 읽기 오류
// (failure에 칸), Open의 오류(required backend 열기 실패, 두 번째 Open).
ROCKS_SHADERCOMPILER_API Result<void>
OpenShaderNuggetCache(NuggetCache& cache, Allocator& allocator, platform::FileSystem& files,
                      StringView root, NuggetSettingsFailure* failure) noexcept;

} // namespace rocks::shader
