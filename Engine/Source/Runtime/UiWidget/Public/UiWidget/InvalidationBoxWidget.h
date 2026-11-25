#pragma once

#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 자식 하나의 그리기 기록을 재사용한다. 자식의 레이아웃이나 그리기 속성이 바뀌면 캐시가 갱신된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API InvalidationBoxWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit InvalidationBoxWidget(Allocator& allocator) noexcept;

protected:
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
};

} // namespace rocks::ui
