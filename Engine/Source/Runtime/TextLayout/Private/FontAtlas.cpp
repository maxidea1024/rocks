// 글꼴 atlas 페이지다(Docs/Design/Font 6.6절). 칸 탐색·분할은 UE TextureAtlas.cpp 321–410행
// (FindSlotForTexture·GetFreeSlotSearchIndex·AddFreeSlot)과 같다.
#include <TextLayout/FontAtlas.h>

#include <string.h>

namespace rocks::text {

namespace {

// uint32의 bit 수다.
constexpr uint32 Uint32Bits = 32;

// UE FMath::CeilLogTwo다(0과 1은 0).
uint32 CeilLogTwo(uint32 value) noexcept {
    uint32 log = 0;
    while (log < Uint32Bits && (static_cast<uint64>(1) << log) < value) {
        ++log;
    }

    return log;
}

} // namespace

FontAtlasPage::FontAtlasPage(Allocator& allocator, uint32 width, uint32 height,
                             FontAtlasContent content) noexcept
    : width_(width), height_(height), content_(content), pixels_(allocator), freeSlots_(allocator) {
}

Result<UniquePtr<FontAtlasPage>> FontAtlasPage::TryCreate(Allocator& allocator, uint32 width,
                                                          uint32 height,
                                                          FontAtlasContent content) noexcept {
    if (width == 0 || height == 0) {
        return Result<UniquePtr<FontAtlasPage>>::Failure(ErrorCode::InvalidArgument);
    }

    auto made = UniquePtr<FontAtlasPage>::TryMake(allocator, allocator, width, height, content);
    if (!made) {
        return made;
    }

    if (auto initialized = made.Value()->Initialize(); !initialized) {
        return Result<UniquePtr<FontAtlasPage>>::Failure(initialized.Error());
    }

    return made;
}

Result<void> FontAtlasPage::Initialize() noexcept {
    const usize bytes = static_cast<usize>(width_) * height_ * FontAtlasBytesPerPixel(content_);
    if (auto sized = pixels_.TryResize(bytes); !sized) {
        return sized;
    }

    memset(pixels_.Data(), 0, pixels_.Size());
    const uint32 bucketCount = CeilLogTwo(width_) + 1;
    for (uint32 bucket = 0; bucket < bucketCount; ++bucket) {
        if (auto added = freeSlots_.TryPushBack(Array<FreeSlot>(pixels_.GetAllocator())); !added) {
            return added;
        }
    }

    return freeSlots_[CeilLogTwo(width_)].TryPushBack({0, 0, width_, height_});
}

Result<bool> FontAtlasPage::Add(uint32 width, uint32 height, Span<const uint8> pixels,
                                AtlasRect& outSlot) noexcept {
    const uint32 bytesPerPixel = FontAtlasBytesPerPixel(content_);
    if (pixels.Size() != static_cast<usize>(width) * height * bytesPerPixel) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    const uint32 paddedWidth = width + FontAtlasPadding * 2;
    const uint32 paddedHeight = height + FontAtlasPadding * 2;
    if (paddedWidth > width_ || paddedHeight > height_) {
        return Result<bool>::Success(false);
    }

    for (uint32 bucket = CeilLogTwo(paddedWidth); bucket < freeSlots_.Size(); ++bucket) {
        Array<FreeSlot>& slots = freeSlots_[bucket];
        for (usize index = slots.Size(); index-- > 0;) {
            const FreeSlot current = slots[index];
            if (paddedWidth > current.width || paddedHeight > current.height) {
                continue;
            }

            // 남는 영역을 둘로 나눈다(UE와 같이 남는 높이 ≤ 남는 너비면 세로, 아니면 가로).
            const uint32 remainingWidth = current.width - paddedWidth;
            const uint32 remainingHeight = current.height - paddedHeight;
            FreeSlot split[2];
            uint32 splitCount = 0;
            if (remainingHeight >= MinAtlasSlotDimension ||
                remainingWidth >= MinAtlasSlotDimension) {
                if (remainingHeight <= remainingWidth) {
                    split[splitCount++] = {current.x + paddedWidth, current.y, remainingWidth,
                                           current.height};
                    if (remainingHeight >= MinAtlasSlotDimension) {
                        split[splitCount++] = {current.x, current.y + paddedHeight, paddedWidth,
                                               remainingHeight};
                    }
                } else {
                    split[splitCount++] = {current.x, current.y + paddedHeight, current.width,
                                           remainingHeight};
                    if (remainingWidth >= MinAtlasSlotDimension) {
                        split[splitCount++] = {current.x + paddedWidth, current.y, remainingWidth,
                                               paddedHeight};
                    }
                }
            }

            // 실패해도 페이지가 그대로이도록 들어갈 목록의 용량을 먼저 확보한다.
            for (uint32 part = 0; part < splitCount; ++part) {
                Array<FreeSlot>& target = freeSlots_[CeilLogTwo(split[part].width)];
                const usize extra =
                    (splitCount == 2 && CeilLogTwo(split[0].width) == CeilLogTwo(split[1].width))
                        ? 2
                        : 1;
                if (auto reserved = target.TryEnsureCapacity(target.Size() + extra); !reserved) {
                    return Result<bool>::Failure(reserved.Error());
                }
            }

            (void)slots.TryRemoveAt(index);
            for (uint32 part = 0; part < splitCount; ++part) {
                (void)freeSlots_[CeilLogTwo(split[part].width)].TryPushBack(split[part]);
            }

            outSlot = {current.x, current.y, paddedWidth, paddedHeight};
            if (width > 0 && height > 0) {
                const usize rowBytes = static_cast<usize>(width) * bytesPerPixel;
                for (uint32 row = 0; row < height; ++row) {
                    const usize target =
                        (static_cast<usize>(current.y + FontAtlasPadding + row) * width_ +
                         current.x + FontAtlasPadding) *
                        bytesPerPixel;
                    memcpy(pixels_.Data() + target, pixels.Data() + row * rowBytes, rowBytes);
                }

                MarkDirty(outSlot);
            }

            return Result<bool>::Success(true);
        }
    }

    return Result<bool>::Success(false);
}

Result<void> FontAtlasPage::Replace(const AtlasRect& inner, Span<const uint8> pixels) noexcept {
    const uint32 bytesPerPixel = FontAtlasBytesPerPixel(content_);
    if (inner.x + inner.width > width_ || inner.y + inner.height > height_ ||
        pixels.Size() != static_cast<usize>(inner.width) * inner.height * bytesPerPixel) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize rowBytes = static_cast<usize>(inner.width) * bytesPerPixel;
    for (uint32 row = 0; row < inner.height; ++row) {
        const usize target = (static_cast<usize>(inner.y + row) * width_ + inner.x) * bytesPerPixel;
        memcpy(pixels_.Data() + target, pixels.Data() + row * rowBytes, rowBytes);
    }

    if (inner.width > 0 && inner.height > 0) {
        MarkDirty(inner);
    }

    return Result<void>::Success();
}

void FontAtlasPage::MarkDirty(const AtlasRect& rect) noexcept {
    if (!hasDirty_) {
        dirty_ = rect;
        hasDirty_ = true;
        return;
    }

    const uint32 left = rect.x < dirty_.x ? rect.x : dirty_.x;
    const uint32 top = rect.y < dirty_.y ? rect.y : dirty_.y;
    const uint32 right = rect.x + rect.width > dirty_.x + dirty_.width ? rect.x + rect.width
                                                                       : dirty_.x + dirty_.width;
    const uint32 bottom = rect.y + rect.height > dirty_.y + dirty_.height
                              ? rect.y + rect.height
                              : dirty_.y + dirty_.height;
    dirty_ = {left, top, right - left, bottom - top};
}

bool FontAtlasPage::TakeDirtyRegion(AtlasRect& out) noexcept {
    if (!hasDirty_) {
        return false;
    }

    out = dirty_;
    hasDirty_ = false;
    return true;
}

} // namespace rocks::text
