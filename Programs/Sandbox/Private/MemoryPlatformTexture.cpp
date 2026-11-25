#include "MemoryPlatformTexture.h"

#include "TexelCodec.h"

#include <Core/Move.h>
#include <RenderBase/CookedTexture.h>

namespace sandbox {

namespace {

// 형식 texel 하나의 최대 byte다(RGBA32 float).
constexpr usize MaxTexelBytes = 16;

} // namespace

MemoryPlatformTexture::MemoryPlatformTexture(Allocator& allocator) noexcept
    : allocator_(allocator), data_(allocator), bulks_(allocator) {
}

Result<void> MemoryPlatformTexture::Build(const MemoryTextureSpec& spec, MemoryTexelFunction fill,
                                          void* context) noexcept {
    data_.mips.Clear();
    bulks_.Clear();
    const graphics::PixelFormatInfo& info = graphics::GetPixelFormatInfo(spec.format);
    if (!IsTexelCodecFormat(spec.format) || spec.width == 0 || spec.height == 0 ||
        spec.depth == 0 || spec.slices == 0 || spec.mips == 0 || info.blockBytes > MaxTexelBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool volume = spec.dimension == TextureDimension::Volume;
    const uint32 resident = spec.residentTailMips == 0 || spec.residentTailMips > spec.mips
                                ? spec.mips
                                : spec.residentTailMips;
    const uint32 streamed = spec.mips - resident;
    Array<uint8> tail(allocator_);
    uint64 tailOffset = 0;
    for (uint32 level = 0; level < spec.mips; ++level) {
        const uint32 width = graphics::MipExtent(spec.width, level);
        const uint32 height = graphics::MipExtent(spec.height, level);
        const uint32 depth = volume ? graphics::MipExtent(spec.depth, level) : 1u;
        Array<uint8> bytes(allocator_);
        const usize texelBytes = info.blockBytes;
        const usize count = static_cast<usize>(width) * height * depth * spec.slices;
        if (auto sized = bytes.TryResize(count * texelBytes); !sized) {
            return sized;
        }

        usize offset = 0;
        // 배치(6.5절): slice(배열 층·cube 면) 다음 z 다음 행(위부터) 다음 열.
        for (uint32 slice = 0; slice < spec.slices; ++slice) {
            for (uint32 z = 0; z < depth; ++z) {
                for (uint32 y = 0; y < height; ++y) {
                    for (uint32 x = 0; x < width; ++x) {
                        float value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        fill(context, level, slice, x, y, z, value);
                        (void)EncodeTexel(spec.format, value, bytes.Data() + offset);
                        offset += texelBytes;
                    }
                }
            }
        }

        TextureMipData mip;
        mip.sizeX = width;
        mip.sizeY = height;
        mip.sizeZ = depth;
        mip.byteSize = bytes.Size();
        if (level < streamed) {
            mip.bulk.bulkId = static_cast<uint32>(bulks_.Size() + 1);
            mip.bulk.rawBytes = bytes.Size();
            if (auto pushed = bulks_.TryPushBack(Move(bytes)); !pushed) {
                return pushed;
            }
        } else {
            tailOffset = (tailOffset + render::CookedTextureTailAlignment - 1) &
                         ~(render::CookedTextureTailAlignment - 1);
            mip.tailOffset = tailOffset;
            if (auto padded = tail.TryResize(static_cast<usize>(tailOffset)); !padded) {
                return padded;
            }

            if (auto appended = tail.TryAppend(bytes.View()); !appended) {
                return appended;
            }

            tailOffset += mip.byteSize;
        }

        if (auto pushed = data_.mips.TryPushBack(mip); !pushed) {
            return pushed;
        }
    }

    data_.dimension = spec.dimension;
    data_.pixelFormat = static_cast<uint8>(graphics::PixelFormatNumber(spec.format));
    data_.srgb = info.srgb;
    data_.swizzleR = spec.swizzle[0];
    data_.swizzleG = spec.swizzle[1];
    data_.swizzleB = spec.swizzle[2];
    data_.swizzleA = spec.swizzle[3];
    data_.sizeX = spec.width;
    data_.sizeY = spec.height;
    data_.sizeZ = volume ? spec.depth : 1u;
    data_.numMips = spec.mips;
    data_.numSlices = volume ? 1u : spec.slices;
    data_.residentTailMips = resident;
    data_.sampler = spec.sampler;
    data_.tailBulk.bulkId = static_cast<uint32>(bulks_.Size() + 1);
    data_.tailBulk.rawBytes = tail.Size();
    return bulks_.TryPushBack(Move(tail));
}

Result<Span<const uint8>> MemoryPlatformTexture::ReadBulk(void* context,
                                                          const BulkDataRef& ref) noexcept {
    const auto* self = static_cast<const MemoryPlatformTexture*>(context);
    if (ref.bulkId == 0 || ref.bulkId > self->bulks_.Size()) {
        return Result<Span<const uint8>>::Failure(ErrorCode::NotFound);
    }

    return Result<Span<const uint8>>::Success(self->bulks_[ref.bulkId - 1].View());
}

} // namespace sandbox
