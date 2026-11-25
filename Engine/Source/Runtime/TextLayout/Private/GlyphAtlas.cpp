#include <TextLayout/GlyphAtlas.h>
#include <Platform/Memory.h>

namespace rocks::text {

GlyphAtlasSnapshot::GlyphAtlasSnapshot(Allocator& allocator) noexcept : pixels(allocator) {
}

GlyphAtlasPage::GlyphAtlasPage(Allocator& allocator) noexcept
    : pixels_(allocator), shelves_(allocator) {
}

Result<void> GlyphAtlasPage::Initialize(GlyphAtlasFormat format, uint32 size) noexcept {
    if (size_ != 0) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }
    if (size < MinGlyphAtlasSize || size > MaxGlyphAtlasSize ||
        (format != GlyphAtlasFormat::Alpha && format != GlyphAtlasFormat::Color &&
         format != GlyphAtlasFormat::DistanceField)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize channels = format == GlyphAtlasFormat::Alpha ? 1 : 4;
    auto allocated = pixels_.TryResize(static_cast<usize>(size) * size * channels);
    if (!allocated) {
        return allocated;
    }

    size_ = size;
    format_ = format;
    revision_ = 1;
    return Result<void>::Success();
}

Result<IntRect> GlyphAtlasPage::TryInsert(uint32 width, uint32 height, usize pitch,
                                          Span<const uint8> pixels) noexcept {
    if (size_ == 0) {
        return Result<IntRect>::Failure(ErrorCode::NotReady);
    }
    if (width == 0 || height == 0) {
        return Result<IntRect>::Failure(ErrorCode::InvalidArgument);
    }
    if (width > size_ - 2 * GlyphAtlasPadding || height > size_ - 2 * GlyphAtlasPadding) {
        return Result<IntRect>::Failure(ErrorCode::CapacityExceeded);
    }

    const usize channels = format_ == GlyphAtlasFormat::Alpha ? 1 : 4;
    const usize rowBytes = width * channels;
    if (pitch < rowBytes || pixels.Size() < rowBytes ||
        height - 1 > (pixels.Size() - rowBytes) / pitch) {
        return Result<IntRect>::Failure(ErrorCode::InvalidArgument);
    }

    const uint32 paddedWidth = width + 2 * GlyphAtlasPadding;
    const uint32 paddedHeight = height + 2 * GlyphAtlasPadding;
    usize best = shelves_.Size();
    for (usize i = 0; i < shelves_.Size(); ++i) {
        const Shelf& shelf = shelves_[i];
        if (paddedWidth <= size_ - shelf.nextX && paddedHeight <= shelf.height &&
            (best == shelves_.Size() || shelf.height < shelves_[best].height)) {
            best = i;
        }
    }
    if (best == shelves_.Size()) {
        if (paddedHeight > size_ - nextY_) {
            return Result<IntRect>::Failure(ErrorCode::CapacityExceeded);
        }
        auto added = shelves_.TryPushBack({nextY_, paddedHeight, 0});
        if (!added) {
            return Result<IntRect>::Failure(added.Error());
        }
        nextY_ += paddedHeight;
    }

    Shelf& shelf = shelves_[best];
    const uint32 x = shelf.nextX + GlyphAtlasPadding;
    const uint32 y = shelf.y + GlyphAtlasPadding;
    for (uint32 row = 0; row < height; ++row) {
        const usize offset = (static_cast<usize>(y + row) * size_ + x) * channels;
        platform::MemoryCopy(pixels_.Data() + offset, pixels.Data() + row * pitch, rowBytes);
    }
    shelf.nextX += paddedWidth;
    ++revision_;
    return Result<IntRect>::Success(
        IntRect({static_cast<int32>(x), static_cast<int32>(y)},
                {static_cast<int32>(x + width), static_cast<int32>(y + height)}));
}

uint64 GlyphAtlasPage::Revision() const noexcept {
    return revision_;
}

Result<void> GlyphAtlasPage::CopySnapshot(GlyphAtlasSnapshot& output) const noexcept {
    if (size_ == 0) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    auto resized = output.pixels.TryResize(pixels_.Size());
    if (!resized) {
        return resized;
    }
    platform::MemoryCopy(output.pixels.Data(), pixels_.Data(), pixels_.Size());
    output.size = size_;
    output.format = format_;
    output.revision = revision_;
    return Result<void>::Success();
}

} // namespace rocks::text
