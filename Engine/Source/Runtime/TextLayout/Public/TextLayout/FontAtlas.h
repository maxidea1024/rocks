#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <TextLayout/GlyphRaster.h>

// 글꼴 atlas 페이지(CPU 화소 + 칸 할당)다(Docs/Design/Font 6.6·6.8절 FT2d). 할당은 UE
// FSlateTextureAtlas::FindSlotForTexture와 같은 guillotine 분할이고 여백은 0으로 채운 1 px다.
//
// 사용 예:
//   auto page = text::FontAtlasPage::TryCreate(allocator, 1024, 1024, content);
//   text::AtlasRect slot;
//   auto added = page.Value()->Add(width, height, pixels, slot);
namespace rocks::text {

// 글리프 칸 둘레의 여백(화소)이다(UE PadWithZero — 1 px).
inline constexpr uint32 FontAtlasPadding = 1;

// 나누고 남은 조각의 최소 변(화소)이다. 더 좁은 조각은 버린다(UE MinSlotDim).
inline constexpr uint32 MinAtlasSlotDimension = 2;

struct AtlasRect {
    uint32 x = 0;
    uint32 y = 0;
    uint32 width = 0;
    uint32 height = 0;

    friend constexpr bool operator==(const AtlasRect&, const AtlasRect&) noexcept = default;
};

class ROCKS_TEXTLAYOUT_API FontAtlasPage {
public:
    // width × height 페이지를 만든다(화소는 0). 크기가 0이면 InvalidArgument, 할당 실패는
    // OutOfMemory다.
    static Result<UniquePtr<FontAtlasPage>>
    TryCreate(Allocator& allocator, uint32 width, uint32 height, FontAtlasContent content) noexcept;

    // width × height 화소(내용 종류의 byte 수, 첫 행이 위)를 넣을 칸을 찾아 복사한다. 칸은 여백을
    // 포함한 사각형이다(UE와 같이 크기 0도 여백만 있는 칸을 받는다). 자리가 없으면 false, 화소
    // 수가 맞지 않으면 InvalidArgument, 할당 실패는 OutOfMemory(페이지는 그대로)다.
    Result<bool> Add(uint32 width, uint32 height, Span<const uint8> pixels,
                     AtlasRect& outSlot) noexcept;

    // 마지막으로 가져간 뒤 바뀐 사각형을 out에 쓰고 비운다. 바뀐 것이 없으면 false다.
    bool TakeDirtyRegion(AtlasRect& out) noexcept;

    uint32 Width() const noexcept {
        return width_;
    }

    uint32 Height() const noexcept {
        return height_;
    }

    FontAtlasContent Content() const noexcept {
        return content_;
    }

    Span<const uint8> Pixels() const noexcept {
        return pixels_.View();
    }

    FontAtlasPage(Allocator& allocator, uint32 width, uint32 height,
                  FontAtlasContent content) noexcept;

private:
    struct FreeSlot {
        uint32 x = 0;
        uint32 y = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    Result<void> Initialize() noexcept;
    void MarkDirty(const AtlasRect& rect) noexcept;

    uint32 width_;
    uint32 height_;
    FontAtlasContent content_;
    Array<uint8> pixels_;
    // 빈 칸 목록을 너비의 CeilLogTwo 구간마다 둔다(뒤가 최근 — UE LinkHead 순서).
    Array<Array<FreeSlot>> freeSlots_;
    AtlasRect dirty_ = {};
    bool hasDirty_ = false;
};

} // namespace rocks::text
