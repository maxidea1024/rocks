#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderFormatTypes.h>

// ShaderCore 안에서 함께 쓰는 검사다.
namespace rocks::shader::core_detail {

// ASCII 식별자([A-Za-z_][A-Za-z0-9_]*, maxBytes 이하)인가.
bool IsShaderIdentifier(StringView name, usize maxBytes) noexcept;

// 형식 id 철자([A-Za-z0-9_], 1 byte 이상 MaxShaderFormatIdBytes - 1 byte 이하)인가.
bool IsShaderFormatId(StringView id) noexcept;

// 한 비트만 켜졌는가.
constexpr bool IsSingleBit(uint32 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

// 단계 비트 하나(VS·PS·CS 가운데)인가.
constexpr bool IsSingleShaderStage(uint32 stage) noexcept {
    constexpr uint32 AllStages = ShaderStageVertex | ShaderStagePixel | ShaderStageCompute;
    return IsSingleBit(stage) && (stage & ~AllStages) == 0;
}

} // namespace rocks::shader::core_detail
