#include "D3DReflection.h"

// D3D 리플렉션의 Windows 밖 구현이다(D3D 도구가 Windows에만 있다).
namespace rocks::shader {

ShaderToolOutcome BuildD3DBindingLayout(D3DCodeKind, const ShaderToolSet&,
                                        const ShaderFormatDefinition&, uint32, uint32,
                                        Span<const ShaderParameterLayout>,
                                        Span<const ShaderSamplerUse>, bool, Span<const uint8>,
                                        Array<uint8>& out, String& diagnostics) noexcept {
    out.Clear();
    (void)diagnostics.TryAppend("D3D shader reflection is only available on Windows");
    return ShaderToolOutcome::UnsupportedHost;
}

} // namespace rocks::shader
