#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderFormatFacts.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderType.h>

// 라이브러리에 들어갈 조합의 열거 규칙이다(Docs/Design/ShaderSystem 12.2.1절 SSD5·SSD6,
// Docs/Design/RenderBase 9.2절 완결성 검사). 쿠커(CookShaderLibraries)가 조합을 만들고 런타임
// 셰이더 집합이 같은 규칙으로 완결성을 검사하므로 규칙은 여기 하나만 둔다.
//
// 사용 예(완결성 검사):
//   Array<ShaderLibraryCombination> combinations(allocator);
//   auto listed = EnumerateShaderLibraryCombinations(registry.Types(), inputs, library.Facts(),
//                                                    library.Kind(), false, combinations);
//   for (const ShaderLibraryCombination& combination : combinations) {
//       ShaderLibraryEntry entry;
//       if (!library.Find(ShaderLibraryCombinationKey(combination), entry)) { ... }
//   }
namespace rocks::shader {

// 열거한 조합 하나다.
struct ShaderLibraryCombination {
    const ShaderTypeDesc* type = nullptr;
    // 묶음 id다(엔진 셰이더 0, 머티리얼은 MaterialShaderBundleId — 16.5절 SSD85).
    uint64 bundleId = EngineShaderBundleId;
    VertexInputTypeId vertexInputType = NoVertexInputType;
    uint32 permutationId = 0;
    // 타입이나 퍼뮤테이션이 개발 전용이다.
    bool developmentOnly = false;
};

// types(등록부 순서 — 이름 해시 순) 가운데 library 소속이고 형식 facts에서 컴파일되는 조합을 결정적
// 순서(타입 → 정점 입력 타입 → 퍼뮤테이션 ID)로 out에 담는다(묶음 id 0). 머티리얼 셰이더 타입
// (ShaderTypeMaterialShader)은 건너뛴다 — 머티리얼 묶음마다 EnumerateMaterialShaderCombinations로
// 곱한다(16.5절 SSD88). 거르기: (1) 요구 기능 ⊆ 형식 기능,
// (2) 개발 전용 타입·퍼뮤테이션은 includeDevelopment일 때만, 개발 전용 PS는 형식에 debugPixelUav가
// 있을 때만(16.1절 SSD34), (3) 타입 거르기, (4) 정점 입력 축이면 정점 입력 타입 거르기. inputs는
// 등록부의 VertexInputTypes()(Freeze 뒤 id 순)다. 실패: 퍼뮤테이션 도메인 위반은 그 오류, 할당 실패
// OutOfMemory.
ROCKS_SHADERCORE_API Result<void> EnumerateShaderLibraryCombinations(
    Span<const ShaderTypeDesc* const> types, Span<const ShaderVertexInputType> inputs,
    const ShaderFormatFacts& facts, ShaderLibraryKind library, bool includeDevelopment,
    Array<ShaderLibraryCombination>& out) noexcept;

// 머티리얼 묶음 하나의 조합을 out 뒤에 더한다(16.5절 SSD86·SSD88 — 기존 내용은 둔다). types 가운데
// 머티리얼 셰이더 타입만, 정점 입력 축 타입은 inputs(그 머티리얼의 사용 대상 정점 입력 타입 —
// 등록부의 desc, id 순)마다, 축이 없는 타입은 정점 입력 타입 없음 하나로 곱한다. 거르기 순서는
// EnumerateShaderLibraryCombinations와 같고 (3)이 materialShouldCompile(materialTraits, …)이다.
// 순서는 타입 → 정점 입력 타입 → 퍼뮤테이션 ID이고 조합의 묶음 id는 bundleId다. MaterialCook의 job
// 목록· 퍼뮤테이션 보고와 셰이더 라이브러리 단계가 이 함수를 함께 써 수가 같다.
//
// 사용 예(MaterialCook — 형식 하나):
//   Array<ShaderLibraryCombination> plans(allocator);
//   auto listed = EnumerateMaterialShaderCombinations(registry.Types(), usedInputs, facts,
//                                                     traitsBytes, MaterialShaderBundleId(setId),
//                                                     false, plans);
//
// 실패: bundleId가 0이면 InvalidArgument, 퍼뮤테이션 도메인 위반은 그 오류, 할당 실패 OutOfMemory
// (실패하면 out에 일부가 더해져 있을 수 있다).
ROCKS_SHADERCORE_API Result<void> EnumerateMaterialShaderCombinations(
    Span<const ShaderTypeDesc* const> types, Span<const ShaderVertexInputType> inputs,
    const ShaderFormatFacts& facts, Span<const uint8> materialTraits, uint64 bundleId,
    bool includeDevelopment, Array<ShaderLibraryCombination>& out) noexcept;

// 조합의 요구 기능이다(타입 요구 기능 + 개발 전용 PS의 진단 버퍼 — 16.1절 SSD34·SSD35).
constexpr uint32
ShaderLibraryCombinationFeatures(const ShaderLibraryCombination& combination) noexcept {
    const bool debugPixel =
        combination.developmentOnly && combination.type->stage == ShaderStagePixel;
    return combination.type->requiredFeatures | (debugPixel ? ShaderFeatureDebugPixelUav : 0u);
}

// 조합의 라이브러리 키다(RenderBase SX5).
constexpr ShaderLibraryKey
ShaderLibraryCombinationKey(const ShaderLibraryCombination& combination) noexcept {
    return ShaderLibraryKey{combination.bundleId, combination.type->nameHash,
                            combination.vertexInputType, combination.permutationId};
}

} // namespace rocks::shader
