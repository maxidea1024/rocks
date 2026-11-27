#pragma once

#include <UiWidget/RichTextDecorator.h>

namespace rocks::ui {

// <img id="coin"/>을 등록된 아틀라스 영역 하나로 치환한다. 같은 id를 다시 등록하면 다음
// RefreshText부터 새 영역을 쓴다. 화면의 텍스처 등록자가 resource의 수명을 관리한다.
// size만큼 줄 너비를 차지하고, 이미지가 글꼴보다 크면 해당 텍스트 블록의 줄 높이도 커진다.
class ROCKS_UIWIDGET_API RichTextImageDecorator final : public RichTextDecorator {
public:
    explicit RichTextImageDecorator(Allocator& allocator) noexcept;
    Result<void> SetImage(StringView id, const PaintGlyph& image) noexcept;
    Result<PaintGlyph> Resolve(StringView tag, StringView attributes,
                               float fontSize) noexcept override;

private:
    struct Entry {
        explicit Entry(Allocator& allocator) noexcept : id(allocator) {
        }

        String id;
        PaintGlyph image{};
    };
    Array<Entry> entries_;
};

} // namespace rocks::ui
