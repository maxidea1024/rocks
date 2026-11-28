#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderFormatTypes.h>

// 셰이더 형식 사실이다(Docs/Design/ShaderSystem 12.2.1절 SSD5·SSD25, UE FShaderTarget·
// FStaticShaderPlatform 질의의 셰이더 쪽 대응).
//
// 셰이더 타입·정점 입력 타입의 거르기 함수가 읽는 형식 값의 묶음이다. 쿠커와 Development 공급자는
// 형식 정의에서(ShaderCompilerCommon MakeShaderFormatFacts), 런타임은 셰이더 라이브러리 머리에서
// (ShaderLibraryView::Facts) 채운다. 그래서 같은 형식이면 두 쪽 값이 같다(거르기 결정성 — 12.2절).
//
// 사용 예(거르기 함수):
//   bool ShouldCompile(VertexInputTypeId, uint32 permutationId,
//                      const ShaderFormatFacts& format) noexcept {
//       return format.backend != ShaderBackend::OpenGL || permutationId == 0;
//   }
namespace rocks::shader {

struct ShaderFormatFacts {
    // 형식 id(NUL 종료, 남는 byte는 0)다. SetShaderFormatFactsId로 채운다.
    char formatId[MaxShaderFormatIdBytes] = {};
    ShaderBackend backend = ShaderBackend::D3D12;
    // 셰이더 모델(형식 정의 compiler.shaderModel)이다.
    ShaderVersion shaderModel;
    // 단계별 상한(형식 정의 limits 그대로)이다.
    ShaderFormatLimits limits;
    // ShaderFeatureBits다.
    uint32 features = 0;
    // ShaderDeviceRequirementBits다.
    uint32 deviceRequirements = 0;
    // ShaderPlatformClassBits다(형식 정의 platformClass — 16.5절 SSD89).
    uint32 platformClasses = 0;

    StringView FormatId() const noexcept {
        return StringView(formatId);
    }
};

// facts.formatId를 id로 채운다(남는 byte는 0). id가 비었거나 MaxShaderFormatIdBytes - 1 byte를
// 넘거나 [A-Za-z0-9_] 밖 문자가 있으면 false이고 facts를 바꾸지 않는다.
ROCKS_SHADERCORE_API bool SetShaderFormatFactsId(ShaderFormatFacts& facts, StringView id) noexcept;

} // namespace rocks::shader
