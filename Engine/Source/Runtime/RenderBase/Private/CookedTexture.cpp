#include <RenderBase/CookedTexture.h>

#include <Base/NumericLimits.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Platform/Memory.h>

namespace rocks::render {

namespace {

inline constexpr LogCategory LogCookedTexture{"RenderBase.CookedTexture"};

// 이방성 배율의 범위다(GraphicsTypes IsValidSamplerDesc와 같다).
constexpr uint32 MinAnisotropy = 1;
constexpr uint32 MaxAnisotropy = 16;

template <class T>
Result<T> Reject(CookedTextureIssue* out, CookedTextureIssue issue, ErrorCode code) noexcept {
    if (out != nullptr) {
        *out = issue;
    }

    return Result<T>::Failure(code);
}

Result<graphics::TextureDesc> Corrupt(CookedTextureIssue* out, CookedTextureIssue issue) noexcept {
    return Reject<graphics::TextureDesc>(out, issue, ErrorCode::CorruptData);
}

Result<graphics::TextureDesc> Unsupported(CookedTextureIssue* out,
                                          CookedTextureIssue issue) noexcept {
    return Reject<graphics::TextureDesc>(out, issue, ErrorCode::UnsupportedCapability);
}

// 차원별 크기 규칙이다(TextureDesc 주석, 6.5절 메타).
bool ValidExtent(const CookedTextureSource& source) noexcept {
    if (source.width == 0 || source.height == 0 || source.depth == 0 || source.slices == 0) {
        return false;
    }

    switch (source.dimension) {
    case graphics::TextureDimension::Texture2D:
        return source.depth == 1 && source.slices == 1;
    case graphics::TextureDimension::Texture2DArray:
        return source.depth == 1;
    case graphics::TextureDimension::Texture3D:
        return source.slices == 1;
    case graphics::TextureDimension::TextureCube:
        return source.depth == 1 && source.width == source.height &&
               source.slices == graphics::CubeFaceCount;
    case graphics::TextureDimension::TextureCubeArray:
        return source.depth == 1 && source.width == source.height &&
               source.slices % graphics::CubeFaceCount == 0;
    }

    return false;
}

} // namespace

const char* CookedTextureIssueName(CookedTextureIssue issue) noexcept {
    switch (issue) {
    case CookedTextureIssue::None:
        return "None";
    case CookedTextureIssue::InvalidDimension:
        return "InvalidDimension";
    case CookedTextureIssue::InvalidFormat:
        return "InvalidFormat";
    case CookedTextureIssue::NonColorFormat:
        return "NonColorFormat";
    case CookedTextureIssue::SrgbMismatch:
        return "SrgbMismatch";
    case CookedTextureIssue::InvalidExtent:
        return "InvalidExtent";
    case CookedTextureIssue::MipCount:
        return "MipCount";
    case CookedTextureIssue::MipExtent:
        return "MipExtent";
    case CookedTextureIssue::MipByteSize:
        return "MipByteSize";
    case CookedTextureIssue::ResidentTail:
        return "ResidentTail";
    case CookedTextureIssue::BulkSize:
        return "BulkSize";
    case CookedTextureIssue::TailAlignment:
        return "TailAlignment";
    case CookedTextureIssue::TailRange:
        return "TailRange";
    case CookedTextureIssue::BlockAlignment:
        return "BlockAlignment";
    case CookedTextureIssue::InvalidSampler:
        return "InvalidSampler";
    case CookedTextureIssue::UnsupportedFormat:
        return "UnsupportedFormat";
    case CookedTextureIssue::UnsupportedDimension:
        return "UnsupportedDimension";
    case CookedTextureIssue::DeviceLimit:
        return "DeviceLimit";
    }

    return "Unknown";
}

Result<graphics::TextureDesc> CheckCookedTexture(const CookedTextureSource& source,
                                                 const graphics::DeviceCaps& caps,
                                                 graphics::PixelFormatCaps formatCaps,
                                                 CookedTextureIssue* issue) noexcept {
    if (issue != nullptr) {
        *issue = CookedTextureIssue::None;
    }

    // --- 데이터 계약(CorruptEntry) ---
    if (!graphics::IsValidTextureDimension(source.dimension)) {
        return Corrupt(issue, CookedTextureIssue::InvalidDimension);
    }

    if (!graphics::IsValidPixelFormat(source.format)) {
        return Corrupt(issue, CookedTextureIssue::InvalidFormat);
    }

    const graphics::PixelFormatInfo& info = graphics::GetPixelFormatInfo(source.format);
    if (info.kind != graphics::PixelFormatKind::Color &&
        info.kind != graphics::PixelFormatKind::Compressed) {
        return Corrupt(issue, CookedTextureIssue::NonColorFormat);
    }

    if (info.srgb != source.srgb) {
        return Corrupt(issue, CookedTextureIssue::SrgbMismatch);
    }

    if (!ValidExtent(source)) {
        return Corrupt(issue, CookedTextureIssue::InvalidExtent);
    }

    if (info.topLevelBlockAligned &&
        (source.width % info.blockWidth != 0 || source.height % info.blockHeight != 0)) {
        return Corrupt(issue, CookedTextureIssue::BlockAlignment);
    }

    graphics::TextureDesc desc;
    desc.dimension = source.dimension;
    desc.width = source.width;
    desc.height = source.height;
    desc.depth = source.depth;
    desc.arraySize = source.dimension == graphics::TextureDimension::TextureCubeArray
                         ? source.slices / graphics::CubeFaceCount
                         : source.slices;
    if (source.dimension == graphics::TextureDimension::TextureCube) {
        desc.arraySize = 1;
    }

    desc.format = source.format;
    desc.usage = graphics::TextureUsage::Sampled;
    const uint32 count = static_cast<uint32>(source.mips.Size());
    if (count == 0 || count > graphics::TextureFullMipCount(desc)) {
        return Corrupt(issue, CookedTextureIssue::MipCount);
    }

    desc.mipLevels = count;
    if (source.residentTailMips == 0 || source.residentTailMips > count) {
        return Corrupt(issue, CookedTextureIssue::ResidentTail);
    }

    // 샘플러: 쿠킹 값 그대로, 이방성 0은 전역 값.
    desc.sampler = source.sampler;
    if (desc.sampler.maxAnisotropy == 0) {
        desc.sampler.maxAnisotropy = CookedTextureDefaultMaxAnisotropy;
    }

    if (desc.sampler.maxAnisotropy > MaxAnisotropy) {
        desc.sampler.maxAnisotropy = MaxAnisotropy;
    }

    if (desc.sampler.maxAnisotropy < MinAnisotropy || !graphics::IsValidSamplerDesc(desc.sampler) ||
        desc.sampler.comparison) {
        return Corrupt(issue, CookedTextureIssue::InvalidSampler);
    }

    const bool volume = source.dimension == graphics::TextureDimension::Texture3D;
    const uint32 streamed = count - source.residentTailMips;
    uint64 tailEnd = 0;
    for (uint32 level = 0; level < count; ++level) {
        const CookedTextureMip& mip = source.mips[level];
        if (mip.width != graphics::MipExtent(source.width, level) ||
            mip.height != graphics::MipExtent(source.height, level) ||
            mip.depth != (volume ? graphics::MipExtent(source.depth, level) : 1u)) {
            return Corrupt(issue, CookedTextureIssue::MipExtent);
        }

        const uint64 bytes = graphics::TextureMipBytes(desc, level);
        if (bytes == 0 || mip.byteSize != bytes) {
            return Corrupt(issue, CookedTextureIssue::MipByteSize);
        }

        if (level < streamed) {
            if (mip.bulk.Size() != mip.byteSize) {
                return Corrupt(issue, CookedTextureIssue::BulkSize);
            }

            continue;
        }

        if (mip.tailOffset % CookedTextureTailAlignment != 0) {
            return Corrupt(issue, CookedTextureIssue::TailAlignment);
        }

        // 꼬리 안 mip은 큰 것부터 겹치지 않게 이어진다. 끝은 넘침 없이 덩어리 안이어야 한다.
        if (mip.tailOffset < tailEnd || mip.tailOffset > source.tail.Size() ||
            mip.byteSize > source.tail.Size() - mip.tailOffset) {
            return Corrupt(issue, CookedTextureIssue::TailRange);
        }

        tailEnd = mip.tailOffset + mip.byteSize;
    }

    // --- 장치 능력(UnsupportedCapability) ---
    if (!graphics::HasPixelFormatCaps(formatCaps, graphics::RequiredPixelFormatCaps(desc))) {
        return Unsupported(issue, CookedTextureIssue::UnsupportedFormat);
    }

    if (source.dimension == graphics::TextureDimension::TextureCubeArray &&
        !caps.features.textureCubeArray) {
        return Unsupported(issue, CookedTextureIssue::UnsupportedDimension);
    }

    if (!graphics::CheckTextureLimits(caps, desc)) {
        return Unsupported(issue, CookedTextureIssue::DeviceLimit);
    }

    return Result<graphics::TextureDesc>::Success(desc);
}

Result<RenderBlob> PackCookedTexturePixels(Allocator& allocator,
                                           const CookedTextureSource& source) noexcept {
    const uint32 count = static_cast<uint32>(source.mips.Size());
    if (source.residentTailMips > count) {
        return Result<RenderBlob>::Failure(ErrorCode::CorruptData);
    }

    uint64 total = 0;
    for (const CookedTextureMip& mip : source.mips) {
        if (mip.byteSize > NumericLimits<uint64>::Max() - total) {
            return Result<RenderBlob>::Failure(ErrorCode::Overflow);
        }

        total += mip.byteSize;
    }

    if (total > static_cast<uint64>(NumericLimits<usize>::Max())) {
        return Result<RenderBlob>::Failure(ErrorCode::Overflow);
    }

    auto blob = RenderBlob::TryAllocate(allocator, static_cast<usize>(total));
    if (!blob) {
        return blob;
    }

    const uint32 streamed = count - source.residentTailMips;
    Span<uint8> out = blob.Value().MutableView();
    usize offset = 0;
    for (uint32 level = 0; level < count; ++level) {
        const CookedTextureMip& mip = source.mips[level];
        const usize bytes = static_cast<usize>(mip.byteSize);
        const uint8* from = nullptr;
        if (level < streamed) {
            if (mip.bulk.Size() != bytes) {
                return Result<RenderBlob>::Failure(ErrorCode::CorruptData);
            }

            from = mip.bulk.Data();
        } else {
            if (mip.tailOffset > source.tail.Size() ||
                mip.byteSize > source.tail.Size() - mip.tailOffset) {
                return Result<RenderBlob>::Failure(ErrorCode::CorruptData);
            }

            from = source.tail.Data() + mip.tailOffset;
        }

        if (bytes != 0) {
            platform::MemoryCopy(out.Data() + offset, from, bytes);
        }

        offset += bytes;
    }

    return blob;
}

Result<CookedTextureUpload> PrepareCookedTexture(Allocator& allocator,
                                                 const CookedTextureSource& source,
                                                 const graphics::Device& device,
                                                 RenderHandle handle,
                                                 CookedTextureIssue* issue) noexcept {
    auto desc =
        CheckCookedTexture(source, device.Caps(), device.GetPixelFormatCaps(source.format), issue);
    if (!desc) {
        return Result<CookedTextureUpload>::Failure(desc.Error());
    }

    auto pixels = PackCookedTexturePixels(allocator, source);
    if (!pixels) {
        return Result<CookedTextureUpload>::Failure(pixels.Error());
    }

    CookedTextureUpload upload;
    upload.handle = handle;
    upload.desc = desc.Value();
    upload.pixels = Move(pixels.Value());
    return Result<CookedTextureUpload>::Success(Move(upload));
}

void CreateCookedTextureOnLane(RenderLaneContext& context, CookedTextureUpload& upload) noexcept {
    auto created = context.Device().CreateTexture(upload.desc, upload.pixels.View());
    upload.pixels = RenderBlob();
    RenderNotice notice;
    notice.handle = upload.handle;
    if (!created) {
        Log::Error(LogCookedTexture, "A cooked texture could not be created ({Format}): {Error}",
                   graphics::PixelFormatName(upload.desc.format), ErrorCodeName(created.Error()));
        if (!context.Handles().MarkFailed(upload.handle, created.Error())) {
            return;
        }

        notice.kind = RenderNoticeKind::ResourceFailed;
        notice.error = created.Error();
        context.Notify(notice);
        return;
    }

    if (!context.Handles().BindTexture(upload.handle, created.Value())) {
        // Retire가 먼저 왔다(handle이 이미 회수 중) — 만든 텍스처를 놓고 통지하지 않는다.
        context.Device().Retire(created.Value());
        return;
    }

    notice.kind = RenderNoticeKind::ResourceReady;
    context.Notify(notice);
}

} // namespace rocks::render
