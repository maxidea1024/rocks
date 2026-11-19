#include "D3D12DeviceImpl.h"

namespace rocks::graphics::d3d12 {

const char* RemovedReasonName(HRESULT reason) noexcept {
    switch (reason) {
    case S_OK:
        return "not removed";
    case DXGI_ERROR_DEVICE_HUNG:
        return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_REMOVED:
        return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET:
        return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL:
        return "DXGI_ERROR_INVALID_CALL";
    default:
        return "unknown reason";
    }
}

D3D12_CULL_MODE RasterCullMode(CullMode mode) noexcept {
    switch (mode) {
    case CullMode::Back:
        return D3D12_CULL_MODE_BACK;
    case CullMode::Front:
        return D3D12_CULL_MODE_FRONT;
    case CullMode::None:
        break;
    }
    return D3D12_CULL_MODE_NONE;
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

D3D12_FILTER SamplerFilter(Filter filter, bool comparison) noexcept {
    // D3D12_FILTER_MIN_MAG_ANISOTROPIC_MIP_POINT(0x54)·COMPARISON(0xD4)는 Agility SDK 1.7 이후
    // d3d12.h에만 있다(Windows SDK 10.0.22621 헤더에 없다). D3D12_ENCODE_ANISOTROPIC_FILTER처럼
    // 이방성 비트(0x40)에 min·mag 선형(0x10·0x04)을 두고 mip은 point(0)로 둔 값이다. 장치가
    // OPTIONS19 AnisoFilterWithPointMipSupported를 낼 때만 쓴다(CreateTexture가 거른다).
    constexpr D3D12_FILTER AnisotropicPointMip = static_cast<D3D12_FILTER>(0x54);
    constexpr D3D12_FILTER ComparisonAnisotropicPointMip = static_cast<D3D12_FILTER>(0xD4);
    switch (filter) {
    case Filter::Point:
        return comparison ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT
                          : D3D12_FILTER_MIN_MAG_MIP_POINT;
    case Filter::Bilinear:
        return comparison ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                          : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    case Filter::Trilinear:
        return comparison ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
                          : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case Filter::AnisotropicPoint:
        return comparison ? ComparisonAnisotropicPointMip : AnisotropicPointMip;
    case Filter::AnisotropicLinear:
        return comparison ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    }
    return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

D3D12_TEXTURE_ADDRESS_MODE SamplerAddress(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Wrap:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::Clamp:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AddressMode::Mirror:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case AddressMode::Border:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

D3D12_RESOURCE_BARRIER TransitionSubresource(ID3D12Resource* resource, uint32 subresource,
                                             D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(resource, before, after);
    barrier.Transition.Subresource = subresource;
    return barrier;
}

D3D12_RESOURCE_STATES StatesFor(ResourceAccess access) noexcept {
    if (HasAnyAccess(access, ResourceAccess::DepthWrite)) {
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    D3D12_RESOURCE_STATES states = D3D12_RESOURCE_STATE_COMMON;
    if (HasAnyAccess(access, ResourceAccess::VertexOrIndex)) {
        states |=
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }
    if (HasAnyAccess(access, ResourceAccess::Constant)) {
        states |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
    if (HasAnyAccess(access, ResourceAccess::IndirectArgs)) {
        states |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
    if (HasAnyAccess(access, ResourceAccess::SrvGraphics)) {
        states |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (HasAnyAccess(access, ResourceAccess::SrvCompute)) {
        states |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (HasAnyAccess(access, ResourceAccess::CopySource)) {
        states |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if (HasAnyAccess(access, ResourceAccess::DepthRead)) {
        states |= D3D12_RESOURCE_STATE_DEPTH_READ;
    }
    if (HasAnyAccess(access, UavAccessMask)) {
        states |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (HasAnyAccess(access, ResourceAccess::RenderTarget)) {
        states |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (HasAnyAccess(access, ResourceAccess::CopyDestination)) {
        states |= D3D12_RESOURCE_STATE_COPY_DEST;
    }
    return states;
}

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource) noexcept {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferResource(uint64 size) noexcept {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

} // namespace rocks::graphics::d3d12
