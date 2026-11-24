#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// ShaderCore 안에서 함께 쓰는 검사다.
namespace rocks::shader::core_detail {

// ASCII 식별자([A-Za-z_][A-Za-z0-9_]*, maxBytes 이하)인가.
bool IsShaderIdentifier(StringView name, usize maxBytes) noexcept;

} // namespace rocks::shader::core_detail
