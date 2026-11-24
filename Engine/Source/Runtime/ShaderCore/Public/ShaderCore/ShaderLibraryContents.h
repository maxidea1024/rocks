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

// 정점 입력 타입 하나다(RenderBase 10절 등록부가 생기기 전에는 호출자가 준다).
struct ShaderVertexInputType {
    // 이름 해시(ShaderTypeNameHash(name))다. NoVertexInputType은 쓸 수 없다.
    VertexInputTypeId id = NoVertexInputType;
    StringView name;
    // 거르기(null이면 늘 참)다.
    ShaderVertexInputShouldCompileFunction shouldCompile = nullptr;
};

// 열거한 조합 하나다.
struct ShaderLibraryCombination {
    const ShaderTypeDesc* type = nullptr;
    VertexInputTypeId vertexInputType = NoVertexInputType;
    uint32 permutationId = 0;
    // 타입이나 퍼뮤테이션이 개발 전용이다.
    bool developmentOnly = false;
};

// 정점 입력 타입 목록을 id 순으로 out에 담는다. 실패: id가 NoVertexInputType이거나 이름 해시와
// 다름·id 중복 InvalidArgument, 할당 실패 OutOfMemory.
ROCKS_SHADERCORE_API Result<void>
SortShaderVertexInputTypes(Span<const ShaderVertexInputType> inputs,
                           Array<ShaderVertexInputType>& out) noexcept;

// types(등록부 순서 — 이름 해시 순) 가운데 library 소속이고 형식 facts에서 컴파일되는 조합을 결정적
// 순서(타입 → 정점 입력 타입 → 퍼뮤테이션 ID)로 out에 담는다. 거르기: (1) 요구 기능 ⊆ 형식 기능,
// (2) 개발 전용 타입·퍼뮤테이션은 includeDevelopment일 때만, 개발 전용 PS는 형식에 debugPixelUav가
// 있을 때만(16.1절 SSD34), (3) 타입 거르기, (4) 정점 입력 축이면 정점 입력 타입 거르기. inputs는
// SortShaderVertexInputTypes 결과다. 실패: 퍼뮤테이션 도메인 위반은 그 오류, 할당 실패 OutOfMemory.
ROCKS_SHADERCORE_API Result<void> EnumerateShaderLibraryCombinations(
    Span<const ShaderTypeDesc* const> types, Span<const ShaderVertexInputType> inputs,
    const ShaderFormatFacts& facts, ShaderLibraryKind library, bool includeDevelopment,
    Array<ShaderLibraryCombination>& out) noexcept;

// 조합의 요구 기능이다(타입 요구 기능 + 개발 전용 PS의 진단 버퍼 — 16.1절 SSD34·SSD35).
constexpr uint32
ShaderLibraryCombinationFeatures(const ShaderLibraryCombination& combination) noexcept {
    const bool debugPixel =
        combination.developmentOnly && combination.type->stage == ShaderStagePixel;
    return combination.type->requiredFeatures | (debugPixel ? ShaderFeatureDebugPixelUav : 0u);
}

// 조합의 라이브러리 키다(묶음 id 0 — 엔진 셰이더, RenderBase SX5).
constexpr ShaderLibraryKey
ShaderLibraryCombinationKey(const ShaderLibraryCombination& combination) noexcept {
    return ShaderLibraryKey{0, combination.type->nameHash, combination.vertexInputType,
                            combination.permutationId};
}

} // namespace rocks::shader
