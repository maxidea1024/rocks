#include <TextLayout/GlyphAtlasCache.h>

namespace rocks::text {

GlyphAtlasCache::GlyphAtlasCache(Allocator& allocator) noexcept
    : allocator_(allocator), pages_(allocator) {
}

Result<void> GlyphAtlasCache::Initialize(GlyphAtlasFormat format, uint32 pageSize,
                                         uint32 maxPages) noexcept {
    if (pageSize_ != 0) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }
    if (pageSize < MinGlyphAtlasSize || pageSize > MaxGlyphAtlasSize || maxPages == 0 ||
        (format != GlyphAtlasFormat::Alpha && format != GlyphAtlasFormat::Color &&
         format != GlyphAtlasFormat::DistanceField)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    format_ = format;
    pageSize_ = pageSize;
    maxPages_ = maxPages;
    return Result<void>::Success();
}

Result<GlyphAtlasLocation> GlyphAtlasCache::TryInsert(uint32 width, uint32 height, usize pitch,
                                                      Span<const uint8> pixels) noexcept {
    if (pageSize_ == 0) {
        return Result<GlyphAtlasLocation>::Failure(ErrorCode::NotReady);
    }
    if (width > pageSize_ - 2 * GlyphAtlasPadding || height > pageSize_ - 2 * GlyphAtlasPadding) {
        return Result<GlyphAtlasLocation>::Failure(ErrorCode::CapacityExceeded);
    }
    for (auto it = pages_.begin(); it != pages_.end(); ++it) {
        auto placed = (*it)->TryInsert(width, height, pitch, pixels);
        if (placed) {
            return Result<GlyphAtlasLocation>::Success({it.Handle(), placed.Value()});
        }
        if (placed.Error() != ErrorCode::CapacityExceeded) {
            return Result<GlyphAtlasLocation>::Failure(placed.Error());
        }
    }
    if (pages_.Size() >= maxPages_) {
        return Result<GlyphAtlasLocation>::Failure(ErrorCode::CapacityExceeded);
    }

    auto page = UniquePtr<GlyphAtlasPage>::TryMake(allocator_, allocator_);
    if (!page) {
        return Result<GlyphAtlasLocation>::Failure(page.Error());
    }
    auto initialized = page.Value()->Initialize(format_, pageSize_);
    if (!initialized) {
        return Result<GlyphAtlasLocation>::Failure(initialized.Error());
    }
    auto placed = page.Value()->TryInsert(width, height, pitch, pixels);
    if (!placed) {
        return Result<GlyphAtlasLocation>::Failure(placed.Error());
    }
    auto stored = pages_.TryAdd(Move(page.Value()));
    if (!stored) {
        return Result<GlyphAtlasLocation>::Failure(stored.Error());
    }
    return Result<GlyphAtlasLocation>::Success({stored.Value(), placed.Value()});
}

const GlyphAtlasPage* GlyphAtlasCache::FindPage(SlotHandle handle) const noexcept {
    const auto* found = pages_.Find(handle);
    return found ? found->Get() : nullptr;
}

usize GlyphAtlasCache::PageCount() const noexcept {
    return pages_.Size();
}

void GlyphAtlasCache::Clear() noexcept {
    pages_.Clear();
}

} // namespace rocks::text
