#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 비교 함수다. 깊이·스텐실 시험(Graphics PipelineState의 DepthStencilState)과 깊이 비교
// 샘플러(SamplerDesc::compare, 그림자)가 함께 쓴다(RenderBase SX13). 쿠킹 데이터의 샘플러가 장치
// 없이 담는 값이라 GraphicsTypes에 둔다.
namespace rocks::graphics {

// "reference 연산 기준값"이 참이면 통과다. 깊이 시험의 reference는 새 깊이, 기준값은 깊이
// 버퍼 값이고, 비교 샘플러의 reference는 셰이더가 넘긴 비교 값, 기준값은 텍스처 깊이다(D3D·Vulkan·
// OpenGL ES 공통 정의). 값 순서는 D3D12_COMPARISON_FUNC − 1·VkCompareOp와 같다. UE는 reverse-Z라
// 깊이 시험 기본이 GreaterEqual이다.
//
// 사용 예(그림자 비교 샘플러):
//   SamplerDesc shadow;
//   shadow.comparison = true;
//   shadow.compare = CompareFunction::LessEqual;
enum class CompareFunction : uint8 {
    Never = 0,
    Less = 1,
    Equal = 2,
    LessEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterEqual = 6,
    Always = 7
};

// 마지막 열거값이다(범위 검사용).
inline constexpr CompareFunction MaxCompareFunction = CompareFunction::Always;

// 열거값 범위 안인지다.
constexpr bool IsValidCompareFunction(CompareFunction function) noexcept {
    return static_cast<uint32>(function) <= static_cast<uint32>(MaxCompareFunction);
}

// 진단·도구용 이름이다(열거자 이름, 범위 밖은 "Unknown"). Parse는 대소문자를 무시하며 모르는
// 이름은 InvalidArgument다.
const char* CompareFunctionName(CompareFunction function) noexcept;
Result<CompareFunction> ParseCompareFunction(StringView name) noexcept;

} // namespace rocks::graphics
