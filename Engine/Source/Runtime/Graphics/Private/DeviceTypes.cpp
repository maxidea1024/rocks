#include <Base/NumericLimits.h>
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
    if (!IsValidPixelFormat(texture) || desc.plane >= info.planeCount ||
        !IsValidChannelSwizzle(desc.swizzle)) {
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

PixelFormatCaps RequiredPixelFormatCaps(const TextureDesc& desc) noexcept {
    PixelFormatCaps caps = PixelFormatCaps::None;
    switch (desc.usage) {
    case TextureUsage::Sampled:
        caps = desc.mipLevels > 1 ? PixelFormatCaps::Sampled | PixelFormatCaps::Mipmaps
                                  : PixelFormatCaps::Sampled;
        break;
    case TextureUsage::RenderTarget:
        caps = PixelFormatCaps::RenderTarget | PixelFormatCaps::Sampled;
        break;
    case TextureUsage::DepthStencil:
        caps = PixelFormatCaps::DepthStencil;
        break;
    }
    if (desc.dimension == TextureDimension::Texture3D) {
        caps = caps | PixelFormatCaps::Texture3D;
    } else if (IsCubeDimension(desc.dimension)) {
        caps = caps | PixelFormatCaps::TextureCube;
    }

    return caps;
}

uint32 TextureSliceCount(const TextureDesc& desc) noexcept {
    switch (desc.dimension) {
    case TextureDimension::Texture2D:
    case TextureDimension::Texture3D:
        return 1;
    case TextureDimension::Texture2DArray:
        return desc.arraySize;
    case TextureDimension::TextureCube:
        return CubeFaceCount;
    case TextureDimension::TextureCubeArray:
        // 층 수 상한(장치 한도 2048)을 크게 넘는 값도 넘치지 않게 64-bit로 셈한 뒤 자른다.
        return uint64{desc.arraySize} * CubeFaceCount > NumericLimits<uint32>::Max()
                   ? NumericLimits<uint32>::Max()
                   : desc.arraySize * CubeFaceCount;
    }

    return 0;
}

uint32 TextureFullMipCount(const TextureDesc& desc) noexcept {
    const uint32 planar = FullMipCount(desc.width, desc.height);
    if (desc.dimension != TextureDimension::Texture3D) {
        return planar;
    }

    const uint32 deep = FullMipCount(desc.depth, 1);
    return deep > planar ? deep : planar;
}

uint32 TextureMipImageCount(const TextureDesc& desc, uint32 level) noexcept {
    if (desc.dimension == TextureDimension::Texture3D) {
        return MipExtent(desc.depth, level);
    }

    return TextureSliceCount(desc);
}

uint64 TextureMipBytes(const TextureDesc& desc, uint32 level) noexcept {
    const uint64 image = PixelFormatImageBytes(desc.format, MipExtent(desc.width, level),
                                               MipExtent(desc.height, level));
    return image * TextureMipImageCount(desc, level);
}

uint64 TextureMipPlaneBytes(const TextureDesc& desc, uint32 level, uint32 plane) noexcept {
    if (plane >= PixelFormatPlaneCount(desc.format)) {
        return 0;
    }

    const uint64 image = PixelFormatPlaneImageBytes(
        desc.format, plane, MipExtent(desc.width, level), MipExtent(desc.height, level));
    return image * TextureMipImageCount(desc, level);
}

uint64 TextureDataBytes(const TextureDesc& desc) noexcept {
    if (desc.mipLevels == 0 || desc.mipLevels > TextureFullMipCount(desc)) {
        return 0;
    }

    uint64 total = 0;
    for (uint32 level = 0; level < desc.mipLevels; ++level) {
        const uint64 bytes = TextureMipBytes(desc, level);
        if (bytes == 0) {
            return 0;
        }

        total += bytes;
    }

    return total;
}

Result<void> ValidateTextureDesc(const TextureDesc& desc) noexcept {
    const bool usageKnown = desc.usage == TextureUsage::Sampled ||
                            desc.usage == TextureUsage::RenderTarget ||
                            desc.usage == TextureUsage::DepthStencil;
    if (!IsValidTextureDimension(desc.dimension) || !usageKnown ||
        !IsValidPixelFormat(desc.format) || desc.width == 0 || desc.height == 0 ||
        desc.depth == 0 || desc.arraySize == 0 || !IsValidSamplerDesc(desc.sampler) ||
        !IsValidChannelSwizzle(desc.swizzle)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if ((desc.dimension != TextureDimension::Texture3D && desc.depth != 1) ||
        (!IsArrayDimension(desc.dimension) && desc.arraySize != 1) ||
        (IsCubeDimension(desc.dimension) && desc.width != desc.height)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool attachment = desc.usage != TextureUsage::Sampled;
    if (desc.mipLevels == 0 || desc.mipLevels > TextureFullMipCount(desc) ||
        (attachment && desc.mipLevels != 1)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const PixelFormatInfo& info = GetPixelFormatInfo(desc.format);
    const bool depth =
        info.kind == PixelFormatKind::Depth || info.kind == PixelFormatKind::DepthStencil;
    if (desc.usage == TextureUsage::DepthStencil && !depth) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (desc.dimension != TextureDimension::Texture2D && (attachment || info.planeCount > 1)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

Result<void> CheckTextureLimits(const DeviceCaps& caps, const TextureDesc& desc) noexcept {
    const DeviceLimits& limits = caps.limits;
    uint32 side = limits.textureSize2D;
    if (desc.dimension == TextureDimension::Texture3D) {
        side = limits.textureSize3D;
    } else if (IsCubeDimension(desc.dimension)) {
        side = limits.textureSizeCube;
    }

    const bool tooLarge = desc.width > side || desc.height > side ||
                          (desc.dimension == TextureDimension::Texture3D && desc.depth > side);
    const bool tooManySlices = TextureSliceCount(desc) > limits.textureArrayLayers;
    const bool cubeArray =
        desc.dimension == TextureDimension::TextureCubeArray && !caps.features.textureCubeArray;
    if (tooLarge || tooManySlices || cubeArray) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

} // namespace rocks::graphics
