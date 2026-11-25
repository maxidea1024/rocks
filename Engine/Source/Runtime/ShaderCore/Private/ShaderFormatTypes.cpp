#include <ShaderCore/ShaderFormatTypes.h>

// 셰이더 형식 공용 어휘(Docs/Design/ShaderSystem 12.2.1절 SSD1)다.
namespace rocks::shader {

const char* ShaderBackendName(ShaderBackend backend) noexcept {
    switch (backend) {
    case ShaderBackend::D3D12:
        return "D3D12";
    case ShaderBackend::D3D11:
        return "D3D11";
    case ShaderBackend::Vulkan:
        return "Vulkan";
    case ShaderBackend::Metal:
        return "Metal";
    case ShaderBackend::OpenGL:
        return "OpenGL";
    }

    return "unknown";
}

} // namespace rocks::shader
