#include <Graphics/DeviceTypes.h>

namespace rocks::graphics {

const char* BackendName(Backend backend) noexcept {
    switch (backend) {
    case Backend::D3D12:
        return "D3D12";
    case Backend::OpenGL:
        return "OpenGL";
    case Backend::Vulkan:
        return "Vulkan";
    case Backend::Metal:
        return "Metal";
    }

    return "Unknown";
}

uint32 VertexFormatSize(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float2:
        return 8;
    case VertexFormat::Float3:
        return 12;
    case VertexFormat::Float4:
        return 16;
    }

    return 0;
}

} // namespace rocks::graphics
