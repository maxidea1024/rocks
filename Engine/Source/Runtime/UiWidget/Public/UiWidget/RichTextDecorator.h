#pragma once

#include <UiFramework/TextLabel.h>

namespace rocks::ui {

// 자체 닫힘 태그 하나를 인라인 글리프로 바꾼다. 예를 들어 <img id="coin"/>에서 tag는
// "img", attributes는 'id="coin"'이다. 두 문자열은 호출 동안만 유효하다. 글리프의 크기는
// 논리 단위이고, resource는 현재 화면에 등록된 아틀라스나 텍스처를 가리킨다.
// 처리할 수 없는 태그나 준비되지 않은 자원은 오류로 반환하면 현재 화면이 보존된다.
class ROCKS_UIWIDGET_API RichTextDecorator {
public:
    virtual ~RichTextDecorator() noexcept = default;
    virtual Result<PaintGlyph> Resolve(StringView tag, StringView attributes,
                                       float fontSize) noexcept = 0;
};

} // namespace rocks::ui
