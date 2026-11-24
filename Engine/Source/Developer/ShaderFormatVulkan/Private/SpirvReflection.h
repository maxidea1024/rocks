#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/String.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderToolLoader/ShaderToolCalls.h>

// SPIR-V 리플렉션 → 바인딩 layout 값 1(Docs/Design/ShaderSystem 16.1절 SSD30·SSD33)이다.
// raw는 고쳐 쓰기 전 DXC 출력(-fspv-reflect의 시맨틱·user type), rewritten은 고쳐 쓴
// 모듈(set·binding· location)이다. 고쳐 쓰기는 id를 바꾸지 않으므로 같은 SPIR-V id로 둘을 잇는다.
// SPIRV-Reflect로 읽어 공용 모델로 옮기고 NormalizeShaderReflection을 거쳐 값 1 byte를 out에 쓴다.
// 결과: Success, CompileError(규칙 위반), ToolError(리플렉션 실패).
namespace rocks::shader {

ShaderToolOutcome BuildSpirvBindingLayout(const ShaderFormatDefinition& format, uint32 stage,
                                          Span<const ShaderParameterLayout> layouts,
                                          Span<const uint32> raw, Span<const uint32> rewritten,
                                          Array<uint8>& out, String& diagnostics) noexcept;

} // namespace rocks::shader
