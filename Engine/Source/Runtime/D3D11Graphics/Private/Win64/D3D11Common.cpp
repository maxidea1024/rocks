#include "D3D11Common.h"

namespace rocks::graphics::d3d11 {

DXGI_FORMAT ResourceFormat(Format format) noexcept {
    switch (format) {
    case Format::RGBA8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8UnormSrgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::Unknown:
        break;
    }

    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT AttributeFormat(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float1:
        return DXGI_FORMAT_R32_FLOAT;
    case VertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VertexFormat::Byte4Snorm:
        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case VertexFormat::UByte4:
        return DXGI_FORMAT_R8G8B8A8_UINT;
    case VertexFormat::UByte4Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VertexFormat::Bgra8Unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VertexFormat::Short2:
        return DXGI_FORMAT_R16G16_SINT;
    case VertexFormat::Short4:
        return DXGI_FORMAT_R16G16B16A16_SINT;
    case VertexFormat::Short2Snorm:
        return DXGI_FORMAT_R16G16_SNORM;
    case VertexFormat::Short4Snorm:
        return DXGI_FORMAT_R16G16B16A16_SNORM;
    case VertexFormat::Half2:
        return DXGI_FORMAT_R16G16_FLOAT;
    case VertexFormat::Half4:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case VertexFormat::UShort2:
        return DXGI_FORMAT_R16G16_UINT;
    case VertexFormat::UShort4:
        return DXGI_FORMAT_R16G16B16A16_UINT;
    case VertexFormat::UShort2Unorm:
        return DXGI_FORMAT_R16G16_UNORM;
    case VertexFormat::UShort4Unorm:
        return DXGI_FORMAT_R16G16B16A16_UNORM;
    case VertexFormat::Rgb10A2Unorm:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VertexFormat::UInt1:
        return DXGI_FORMAT_R32_UINT;
    }

    return DXGI_FORMAT_UNKNOWN;
}

bool IsAttributeSemantic(const char* name) noexcept {
    constexpr char Expected[] = "ATTRIBUTE";
    usize index = 0;
    for (; Expected[index] != 0; ++index) {
        char letter = name[index];
        if (letter >= 'a' && letter <= 'z') {
            letter = static_cast<char>(letter - 'a' + 'A');
        }

        if (letter != Expected[index]) {
            return false;
        }
    }

    return name[index] == 0;
}

D3D11_CULL_MODE RasterCullMode(CullMode mode) noexcept {
    switch (mode) {
    case CullMode::Back:
        return D3D11_CULL_BACK;
    case CullMode::Front:
        return D3D11_CULL_FRONT;
    case CullMode::None:
        break;
    }

    return D3D11_CULL_NONE;
}

D3D11_FILTER SamplerFilter(Filter filter) noexcept {
    switch (filter) {
    case Filter::Nearest:
        return D3D11_FILTER_MIN_MAG_MIP_POINT;
    case Filter::Bilinear:
        return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    case Filter::Trilinear:
        return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    case Filter::Anisotropic:
        return D3D11_FILTER_ANISOTROPIC;
    }

    return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
}

D3D11_TEXTURE_ADDRESS_MODE SamplerAddress(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Wrap:
        return D3D11_TEXTURE_ADDRESS_WRAP;
    case AddressMode::Clamp:
        return D3D11_TEXTURE_ADDRESS_CLAMP;
    case AddressMode::Mirror:
        return D3D11_TEXTURE_ADDRESS_MIRROR;
    case AddressMode::Border:
        return D3D11_TEXTURE_ADDRESS_BORDER;
    }

    return D3D11_TEXTURE_ADDRESS_WRAP;
}

bool IsDeviceRemovedResult(HRESULT result) noexcept {
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
           result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

bool CopyTruncated(char* out, usize capacity, const char* text) noexcept {
    if (capacity == 0) {
        return *text != '\0';
    }

    usize length = 0;
    while (text[length] != '\0' && length + 1 < capacity) {
        out[length] = text[length];
        ++length;
    }

    out[length] = '\0';
    return text[length] != '\0';
}

} // namespace rocks::graphics::d3d11
