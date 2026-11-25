#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Rect.h>

namespace rocks::text {

enum class GlyphAtlasFormat : uint8 {
    Alpha,
    Color,
    DistanceField
};

inline constexpr uint32 MinGlyphAtlasSize = 128;
inline constexpr uint32 MaxGlyphAtlasSize = 2048;
inline constexpr uint32 GlyphAtlasPadding = 1;

// 렌더 스레드로 넘길 페이지 복사본이다. 원본에 다음 글리프를 추가해도 픽셀이 바뀌지 않는다.
struct ROCKS_TEXTLAYOUT_API GlyphAtlasSnapshot {
    explicit GlyphAtlasSnapshot(Allocator& allocator) noexcept;

    Array<uint8> pixels;
    GlyphAtlasFormat format = GlyphAtlasFormat::Alpha;
    uint32 size = 0;
    uint64 revision = 0;
};

// 같은 형식의 글리프를 한 페이지에 넣는다. 반환하는 좌표에는 투명 여백이 포함되지 않는다.
class ROCKS_TEXTLAYOUT_API GlyphAtlasPage {
public:
    explicit GlyphAtlasPage(Allocator& allocator) noexcept;

    Result<void> Initialize(GlyphAtlasFormat format, uint32 size) noexcept;

    // 원본은 위에서 아래로 정렬된 행이다. pitch는 한 행의 시작에서 다음 행까지의 바이트 수다.
    Result<IntRect> TryInsert(uint32 width, uint32 height, usize pitch,
                              Span<const uint8> pixels) noexcept;

    uint64 Revision() const noexcept;
    Result<void> CopySnapshot(GlyphAtlasSnapshot& output) const noexcept;

private:
    struct Shelf {
        uint32 y;
        uint32 height;
        uint32 nextX;
    };

    Array<uint8> pixels_;
    Array<Shelf> shelves_;
    uint32 size_ = 0;
    uint32 nextY_ = 0;
    GlyphAtlasFormat format_ = GlyphAtlasFormat::Alpha;
    uint64 revision_ = 0;
};

} // namespace rocks::text
