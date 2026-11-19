#include <Base/Checked.h>
#include <Base/NumericLimits.h>
#include <Graphics/DeviceTypes.h>
#include <Graphics/ResourceAccess.h>
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

Result<PixelFormat> ResolveTextureViewFormat(PixelFormat texture, const TextureViewDesc& desc,
                                             bool comparisonSampler) noexcept {
    auto resolved = ResolveTextureViewFormat(texture, desc);
    if (!resolved || !comparisonSampler) {
        return resolved;
    }

    const PixelFormatKind kind = GetPixelFormatInfo(resolved.Value()).kind;
    if (kind != PixelFormatKind::Depth && kind != PixelFormatKind::DepthStencil) {
        return Result<PixelFormat>::Failure(ErrorCode::InvalidArgument);
    }

    return resolved;
}

Result<ResolvedSubresourceRange> ResolveTextureViewRange(TextureDimension dimension,
                                                         TextureUsage usage,
                                                         const TextureSubresources& shape,
                                                         const TextureViewDesc& desc) noexcept {
    const auto invalid = []() noexcept {
        return Result<ResolvedSubresourceRange>::Failure(ErrorCode::InvalidArgument);
    };
    if (desc.access != ViewAccess::Read && desc.access != ViewAccess::ReadWrite) {
        return invalid();
    }

    // 개수 0은 끝까지다. 시작이 밖이거나 끝이 넘치면 거부한다(잘라 받지 않는다).
    if (desc.firstMip >= shape.mipLevels || desc.firstSlice >= shape.slices) {
        return invalid();
    }

    ResolvedSubresourceRange range;
    range.firstMip = desc.firstMip;
    range.mipCount = desc.mipCount != 0 ? desc.mipCount : shape.mipLevels - desc.firstMip;
    range.firstSlice = desc.firstSlice;
    range.sliceCount = desc.sliceCount != 0 ? desc.sliceCount : shape.slices - desc.firstSlice;
    if (range.mipCount > shape.mipLevels - range.firstMip ||
        range.sliceCount > shape.slices - range.firstSlice) {
        return invalid();
    }

    // cube 계열의 쓰기 view는 면 단위 2D 배열이라 면 범위가 자유롭다(D3D·Vulkan에 cube UAV가 없다).
    const bool write = desc.access == ViewAccess::ReadWrite;
    const bool wholeSlices = range.firstSlice == 0 && range.sliceCount == shape.slices;
    if ((dimension == TextureDimension::Texture3D ||
         (dimension == TextureDimension::TextureCube && !write)) &&
        !wholeSlices) {
        return invalid();
    }

    if (dimension == TextureDimension::TextureCubeArray && !write &&
        (range.firstSlice % CubeFaceCount != 0 || range.sliceCount % CubeFaceCount != 0)) {
        return invalid();
    }

    if (write && (!HasTextureUsage(usage, TextureUsage::Storage) || range.mipCount != 1 ||
                  desc.plane != 0 || !desc.swizzle.IsIdentity())) {
        return invalid();
    }

    return Result<ResolvedSubresourceRange>::Success(range);
}

bool IsAttachmentInRange(TextureDimension dimension, uint32 depth, uint32 mipLevels, uint32 slices,
                         uint32 mipLevel, uint32 slice) noexcept {
    if (mipLevel >= mipLevels) {
        return false;
    }

    const uint32 limit =
        dimension == TextureDimension::Texture3D ? MipExtent(depth, mipLevel) : slices;
    return slice < limit;
}

ResolvedSubresourceRange AttachmentSubresource(TextureDimension dimension, uint32 mipLevel,
                                               uint32 slice) noexcept {
    // 3D는 mip 하나가 subresource 하나다(깊이 면은 subresource가 아니다).
    return {mipLevel, 1, dimension == TextureDimension::Texture3D ? 0u : slice, 1};
}

uint32 TextureViewBaseMip(const DeviceFeatures& features, const TextureViewDesc& desc) noexcept {
    return features.textureViewRanges ? 0u : desc.firstMip;
}

PixelFormatCaps RequiredPixelFormatCaps(const TextureDesc& desc) noexcept {
    // 용도 비트마다의 능력을 더한다(RenderTarget | Storage는 둘의 합).
    PixelFormatCaps caps = PixelFormatCaps::None;
    if (HasTextureUsage(desc.usage, TextureUsage::Sampled)) {
        caps = caps | PixelFormatCaps::Sampled;
    }

    if (HasTextureUsage(desc.usage, TextureUsage::RenderTarget)) {
        caps = caps | PixelFormatCaps::RenderTarget | PixelFormatCaps::Sampled;
    }

    if (HasTextureUsage(desc.usage, TextureUsage::DepthStencil)) {
        caps = caps | PixelFormatCaps::DepthStencil;
    }

    if (HasTextureUsage(desc.usage, TextureUsage::Storage)) {
        caps = caps | PixelFormatCaps::Sampled | PixelFormatCaps::StorageWrite;
    }

    // mip 사슬은 Mipmaps가 필요하다(Sampled·Storage, 텍스처 GX2 C부의 렌더 타깃·깊이).
    if (desc.mipLevels > 1) {
        caps = caps | PixelFormatCaps::Mipmaps;
    }

    if (desc.sampleCount > 1) {
        caps = caps | PixelFormatCaps::Multisample;
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
    uint64 bytes = 0;
    return TryMultiply(image, uint64{TextureMipImageCount(desc, level)}, bytes) ? bytes : 0u;
}

uint64 TextureMipPlaneBytes(const TextureDesc& desc, uint32 level, uint32 plane) noexcept {
    if (plane >= PixelFormatPlaneCount(desc.format)) {
        return 0;
    }

    const uint64 image = PixelFormatPlaneImageBytes(
        desc.format, plane, MipExtent(desc.width, level), MipExtent(desc.height, level));
    uint64 bytes = 0;
    return TryMultiply(image, uint64{TextureMipImageCount(desc, level)}, bytes) ? bytes : 0u;
}

uint64 TextureDataBytes(const TextureDesc& desc) noexcept {
    if (desc.mipLevels == 0 || desc.mipLevels > TextureFullMipCount(desc)) {
        return 0;
    }

    uint64 total = 0;
    for (uint32 level = 0; level < desc.mipLevels; ++level) {
        const uint64 bytes = TextureMipBytes(desc, level);
        if (bytes == 0 || !TryAdd(total, bytes, total)) {
            return 0;
        }
    }

    return total;
}

bool IsValidSampleCount(uint32 sampleCount) noexcept {
    return sampleCount != 0 && sampleCount <= MaxSampleCount &&
           (sampleCount & (sampleCount - 1)) == 0;
}

Result<void> ValidateTextureDesc(const TextureDesc& desc) noexcept {
    if (!IsValidTextureDimension(desc.dimension) || !IsValidTextureUsage(desc.usage) ||
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

    const bool attachment =
        HasTextureUsage(desc.usage, TextureUsage::RenderTarget | TextureUsage::DepthStencil);
    const bool storage = HasTextureUsage(desc.usage, TextureUsage::Storage);
    const bool depthTarget = HasTextureUsage(desc.usage, TextureUsage::DepthStencil);
    if (desc.mipLevels == 0 || desc.mipLevels > TextureFullMipCount(desc)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 3D 깊이 attachment는 없다(D3D에 3D DSV가 없다).
    if (depthTarget && desc.dimension == TextureDimension::Texture3D) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const PixelFormatInfo& info = GetPixelFormatInfo(desc.format);
    const bool depth =
        info.kind == PixelFormatKind::Depth || info.kind == PixelFormatKind::DepthStencil;
    if ((depthTarget || desc.sampler.comparison) && !depth) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (storage && (depth || info.planeCount > 1)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // MSAA는 렌더 타깃·깊이의 2D, mip 1만이다(초기 픽셀·UAV·mip 사슬이 없다).
    if (!IsValidSampleCount(desc.sampleCount) ||
        (desc.sampleCount > 1 &&
         (!attachment || storage || desc.dimension != TextureDimension::Texture2D ||
          desc.mipLevels != 1))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 다중 plane 형식(D24S8·D32S8·비디오)의 2D 밖 차원은 이 단계의 계약 밖이다.
    if (desc.dimension != TextureDimension::Texture2D && info.planeCount > 1) {
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
