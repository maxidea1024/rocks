#pragma once

#include <Core/Delegate.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 스크롤 영역에서 외부에 표시하는 독립 스크롤바다. SetState는 전체 길이에 대한
// 시작점과 보이는 길이의 비율을 받는다. SetOnScrolled에 콘텐츠의 스크롤 처리를 연결한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ScrollBarWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetState(float offset, float thumbSize) noexcept;
    float Offset() const noexcept;
    float ThumbSize() const noexcept;
    void SetHorizontal(bool horizontal) noexcept;
    void SetOnScrolled(Delegate<void(float)>&& callback) noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "offset") float offset_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "thumbSize") float thumbSize_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "horizontal") bool horizontal_ = false;
    ROCKS_NATIVE() Delegate<void(float)> scrolled_;
};

} // namespace rocks::ui
