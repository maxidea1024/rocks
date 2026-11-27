#pragma once

#include <Core/Math/Color.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

namespace rocks::ui {

// <warning>…</>와 같은 이름 있는 범위의 색이다. 중첩 태그는 가장 안쪽 색을 사용한다.
ROCKS_STRUCT()
struct ROCKS_UIWIDGET_API RichTextStyle {
    ROCKS_GENERATED_BODY()

    explicit RichTextStyle(Allocator& allocator) noexcept : name(allocator) {
    }

    ROCKS_PROPERTY() String name;
    // 밑줄과 취소선은 글리프가 없는 공백에도 이어지며 줄바꿈 경계에서 끊긴다.
    ROCKS_PROPERTY() bool underline = false;
    ROCKS_PROPERTY() bool strikeThrough = false;
    ROCKS_PROPERTY() LinearColor tint { 1, 1, 1, 1 };
};

} // namespace rocks::ui
