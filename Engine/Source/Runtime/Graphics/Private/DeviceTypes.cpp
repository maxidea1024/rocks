#include <Graphics/DeviceTypes.h>
#include <Platform/Atomics.h>

namespace rocks::graphics {

namespace {

// 마지막으로 나눠 준 장치 incarnation 번호다.
uint64 lastDeviceEpoch = 0;

} // namespace

uint64 NewDeviceEpoch() noexcept {
    return platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&lastDeviceEpoch, 1) + 1;
}

const char* BackendName(Backend backend) noexcept {
    switch (backend) {
    case Backend::D3D12:
        return "D3D12";
    case Backend::D3D11:
        return "D3D11";
    case Backend::OpenGL:
        return "OpenGL";
    case Backend::Vulkan:
        return "Vulkan";
    case Backend::Metal:
        return "Metal";
    }

    return "Unknown";
}

Result<void> ValidateVertexLayout(const VertexLayout& layout) noexcept {
    if (layout.attributes.Size() > MaxVertexAttributes ||
        layout.streams.Size() > MaxVertexStreams) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const VertexStream& stream : layout.streams) {
        if (stream.stride == 0 || stream.stride > MaxVertexStride ||
            stream.stride % VertexLayoutAlignment != 0 ||
            (stream.stepRate != VertexStepRate::Vertex &&
             stream.stepRate != VertexStepRate::Instance)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    uint32 locations = 0;
    uint32 usedStreams = 0;
    for (const VertexAttribute& attribute : layout.attributes) {
        const uint32 size = VertexFormatSize(attribute.format);
        if (attribute.location >= MaxVertexAttributes || size == 0 ||
            attribute.stream >= layout.streams.Size() ||
            attribute.offset > MaxVertexAttributeOffset ||
            attribute.offset % VertexLayoutAlignment != 0 ||
            (locations & (1u << attribute.location)) != 0 ||
            attribute.offset + size > layout.streams[attribute.stream].stride) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        locations |= 1u << attribute.location;
        usedStreams |= 1u << attribute.stream;
    }

    // 선언한 stream마다 속성이 하나 이상 있어야 한다.
    const uint32 declared = layout.streams.Size() == 0 ? 0u : (1u << layout.streams.Size()) - 1u;
    if (usedStreams != declared) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<PixelFormat> ResolveTextureViewFormat(PixelFormat texture,
                                             const TextureViewDesc& desc) noexcept {
    const PixelFormatInfo& info = GetPixelFormatInfo(texture);
    if (!IsValidPixelFormat(texture) || desc.plane >= info.planeCount) {
        return Result<PixelFormat>::Failure(ErrorCode::InvalidArgument);
    }
    if (info.planeCount == 2) {
        const PixelFormat planeFormat = info.planes[desc.plane].format;
        if (desc.format != PixelFormat::Unknown && desc.format != planeFormat) {
            return Result<PixelFormat>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<PixelFormat>::Success(planeFormat);
    }
    if (desc.format == PixelFormat::Unknown || desc.format == texture) {
        return Result<PixelFormat>::Success(texture);
    }
    if (info.srgbPair != PixelFormat::Unknown && desc.format == info.srgbPair) {
        return Result<PixelFormat>::Success(info.srgbPair);
    }

    return Result<PixelFormat>::Failure(ErrorCode::InvalidArgument);
}

PixelFormatCaps RequiredPixelFormatCaps(TextureUsage usage, uint32 mipLevels) noexcept {
    switch (usage) {
    case TextureUsage::Sampled:
        return mipLevels > 1 ? PixelFormatCaps::Sampled | PixelFormatCaps::Mipmaps
                             : PixelFormatCaps::Sampled;
    case TextureUsage::RenderTarget:
        return PixelFormatCaps::RenderTarget | PixelFormatCaps::Sampled;
    case TextureUsage::DepthStencil:
        return PixelFormatCaps::DepthStencil;
    }

    return PixelFormatCaps::None;
}

} // namespace rocks::graphics
