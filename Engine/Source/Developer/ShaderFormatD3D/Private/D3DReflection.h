#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/String.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <ShaderToolLoader/ShaderToolCalls.h>

// D3D 리플렉션 → 바인딩 layout 값 1(Docs/Design/ShaderSystem 16.1절 SSD30·SSD33)이다. DXIL은 DXC
// IDxcUtils::CreateReflection(ID3D12ShaderReflection), DXBC는 FXC
// D3DReflect(ID3D11ShaderReflection)로 읽어 공용 모델(ShaderReflection)로 옮긴 뒤
// NormalizeShaderReflection을 거쳐 값 1 byte를 out에 쓴다. Windows 구현만 있다(그 밖은
// UnsupportedHost — D3D 도구가 Windows에만 있다). 결과: Success(out에 값 1), CompileError(규칙 위반
// — diagnostics), ToolError(리플렉션 실패).
namespace rocks::shader {

enum class D3DCodeKind : uint8 {
    Dxil,
    Dxbc
};

ShaderToolOutcome BuildD3DBindingLayout(D3DCodeKind kind, const ShaderToolSet& tools,
                                        const ShaderFormatDefinition& format, uint32 stage,
                                        Span<const ShaderParameterLayout> layouts,
                                        Span<const uint8> code, Array<uint8>& out,
                                        String& diagnostics) noexcept;

} // namespace rocks::shader
