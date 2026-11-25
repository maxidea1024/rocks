#pragma once

#include <Core/Containers/SlotArray.h>
#include <Core/Memory/UniquePtr.h>
#include <TextLayout/GlyphAtlas.h>

namespace rocks::text {

struct GlyphAtlasLocation {
    SlotHandle page;
    IntRect pixels;
};

// 같은 형식의 페이지 묶음이다. 소유자는 프레임 경계에서만 Clear를 호출한다.
// 반환한 페이지 핸들은 Clear 후 무효화되므로 같은 인덱스에 새 글리프가 생겨도 구별된다.
class ROCKS_TEXTLAYOUT_API GlyphAtlasCache {
public:
    explicit GlyphAtlasCache(Allocator& allocator) noexcept;

    Result<void> Initialize(GlyphAtlasFormat format, uint32 pageSize, uint32 maxPages) noexcept;
    Result<GlyphAtlasLocation> TryInsert(uint32 width, uint32 height, usize pitch,
                                         Span<const uint8> pixels) noexcept;

    const GlyphAtlasPage* FindPage(SlotHandle handle) const noexcept;
    usize PageCount() const noexcept;
    void Clear() noexcept;

private:
    Allocator& allocator_;
    SlotArray<UniquePtr<GlyphAtlasPage>> pages_;
    GlyphAtlasFormat format_ = GlyphAtlasFormat::Alpha;
    uint32 pageSize_ = 0;
    uint32 maxPages_ = 0;
};

} // namespace rocks::text
