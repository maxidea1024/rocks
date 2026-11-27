#include <WorldIntegration/Textures/PlatformTextureSource.h>

namespace rocks {

namespace {

// 장치 차원 범위 밖 값이다(GraphicsTextureDimension의 모르는 차원 결과).
inline constexpr uint8 InvalidGraphicsDimension = 0xff;

// 쿠킹 데이터 손상이다(Docs/Design/RenderBase 6.6절 — 읽은 데이터라 CorruptData).
Result<void> Corrupt() noexcept {
    return Result<void>::Failure(ErrorCode::CorruptData);
}

bool ToFilter(TextureSamplerFilter filter, graphics::Filter& out) noexcept {
    switch (filter) {
    case TextureSamplerFilter::Point:
        out = graphics::Filter::Point;
        return true;
    case TextureSamplerFilter::Bilinear:
        out = graphics::Filter::Bilinear;
        return true;
    case TextureSamplerFilter::Trilinear:
        out = graphics::Filter::Trilinear;
        return true;
    case TextureSamplerFilter::AnisotropicPoint:
        out = graphics::Filter::AnisotropicPoint;
        return true;
    case TextureSamplerFilter::AnisotropicLinear:
        out = graphics::Filter::AnisotropicLinear;
        return true;
    }

    return false;
}

bool ToAddress(TextureAddress address, graphics::AddressMode& out) noexcept {
    switch (address) {
    case TextureAddress::Wrap:
        out = graphics::AddressMode::Wrap;
        return true;
    case TextureAddress::Clamp:
        out = graphics::AddressMode::Clamp;
        return true;
    case TextureAddress::Mirror:
        out = graphics::AddressMode::Mirror;
        return true;
    case TextureAddress::Border:
        out = graphics::AddressMode::Border;
        return true;
    }

    return false;
}

bool ToBorder(TextureBorderColor color, graphics::BorderColor& out) noexcept {
    switch (color) {
    case TextureBorderColor::TransparentBlack:
        out = graphics::BorderColor::TransparentBlack;
        return true;
    case TextureBorderColor::OpaqueBlack:
        out = graphics::BorderColor::OpaqueBlack;
        return true;
    case TextureBorderColor::OpaqueWhite:
        out = graphics::BorderColor::OpaqueWhite;
        return true;
    }

    return false;
}

// 쿠킹 swizzle 한 칸이다(TextureSwizzle과 SwizzleSource는 같은 번호 — R 0 … One 5).
bool ToSwizzle(TextureSwizzle swizzle, graphics::SwizzleSource& out) noexcept {
    const auto source = static_cast<graphics::SwizzleSource>(static_cast<uint8>(swizzle));
    if (!graphics::IsValidSwizzleSource(source)) {
        return false;
    }

    out = source;
    return true;
}

} // namespace

graphics::TextureDimension GraphicsTextureDimension(TextureDimension dimension) noexcept {
    switch (dimension) {
    case TextureDimension::Texture2D:
        return graphics::TextureDimension::Texture2D;
    case TextureDimension::TextureCube:
        return graphics::TextureDimension::TextureCube;
    case TextureDimension::Texture2DArray:
        return graphics::TextureDimension::Texture2DArray;
    case TextureDimension::TextureCubeArray:
        return graphics::TextureDimension::TextureCubeArray;
    case TextureDimension::Volume:
        return graphics::TextureDimension::Texture3D;
    }

    // 범위 밖 값은 장치 차원 범위 밖으로 돌려 검사가 InvalidDimension으로 거부하게 한다.
    return static_cast<graphics::TextureDimension>(InvalidGraphicsDimension);
}

Result<void> MakePlatformTextureSource(const TexturePlatformData& data, PlatformBulkReader reader,
                                       void* context, PlatformTextureSource& out) noexcept {
    out.mips.Clear();
    out.source = render::CookedTextureSource{};
    const graphics::TextureDimension dimension = GraphicsTextureDimension(data.dimension);
    if (!graphics::IsValidTextureDimension(dimension)) {
        return Corrupt();
    }

    auto format = graphics::PixelFormatFromNumber(data.pixelFormat);
    if (!format) {
        return Corrupt();
    }

    render::CookedTextureSource& source = out.source;
    source.dimension = dimension;
    source.format = format.Value();
    source.srgb = data.srgb;
    source.width = data.sizeX;
    source.height = data.sizeY;
    const bool volume = dimension == graphics::TextureDimension::Texture3D;
    source.depth = volume ? data.sizeZ : 1u;
    source.slices = volume ? 1u : data.numSlices;
    source.residentTailMips = data.residentTailMips;
    if (!ToFilter(data.sampler.filter, source.sampler.filter) ||
        !ToAddress(data.sampler.addressU, source.sampler.addressU) ||
        !ToAddress(data.sampler.addressV, source.sampler.addressV) ||
        !ToAddress(data.sampler.addressW, source.sampler.addressW) ||
        !ToBorder(data.sampler.borderColor, source.sampler.borderColor) ||
        !ToSwizzle(data.swizzleR, out.swizzle.red) ||
        !ToSwizzle(data.swizzleG, out.swizzle.green) ||
        !ToSwizzle(data.swizzleB, out.swizzle.blue) ||
        !ToSwizzle(data.swizzleA, out.swizzle.alpha)) {
        return Corrupt();
    }

    source.sampler.maxAnisotropy = data.sampler.maxAniso;
    const uint32 count = static_cast<uint32>(data.mips.Size());
    if (count != data.numMips) {
        return Corrupt();
    }

    if (auto reserved = out.mips.TryReserve(count); !reserved) {
        return reserved;
    }

    const uint32 resident = data.residentTailMips <= count ? data.residentTailMips : count;
    const uint32 streamed = count - resident;
    for (uint32 level = 0; level < count; ++level) {
        const TextureMipData& mip = data.mips[level];
        render::CookedTextureMip converted;
        converted.width = mip.sizeX;
        converted.height = mip.sizeY;
        converted.depth = volume ? mip.sizeZ : 1u;
        converted.byteSize = mip.byteSize;
        converted.tailOffset = mip.tailOffset;
        if (level < streamed) {
            if (mip.bulk.IsEmpty()) {
                return Corrupt();
            }

            auto bytes = reader(context, mip.bulk);
            if (!bytes) {
                return Result<void>::Failure(bytes.Error());
            }

            converted.bulk = bytes.Value();
        }

        (void)out.mips.TryPushBack(converted);
    }

    if (resident != 0) {
        if (data.tailBulk.IsEmpty()) {
            return Corrupt();
        }

        auto tail = reader(context, data.tailBulk);
        if (!tail) {
            return Result<void>::Failure(tail.Error());
        }

        source.tail = tail.Value();
    }

    source.mips = out.mips.View();
    return Result<void>::Success();
}

} // namespace rocks
