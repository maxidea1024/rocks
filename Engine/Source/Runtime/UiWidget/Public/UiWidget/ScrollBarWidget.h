#pragma once

#include <Core/Delegate.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 스크롤 영역에서 외부에 표시하는 독립 스크롤바다. SetState는 전체 길이에 대한
// 시작점과 보이는 길이의 비율을 받는다. OnScrolled 이벤트에 콘텐츠의 스크롤 처리를 연결한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ScrollBarWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    // 이벤트 필드가 allocator를 받는다(EventReflection ED14).
    explicit ScrollBarWidget(Allocator& allocator) noexcept;
    Result<void> SetState(float offset, float thumbSize) noexcept;
    float Offset() const noexcept;
    float ThumbSize() const noexcept;
    void SetHorizontal(bool horizontal) noexcept;
    // 사건은 리플렉션 이벤트다(Docs/Design/UiToolkit 21.5a절): C++는 TryAddLambda·TryAdd(native
    // 구독), 에셋·Sparks는 TryBind(리플렉션 함수)로 받는다. 구독은 여럿일 수 있다.
    Event<float>& OnScrolled() noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "offset") float offset_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "thumbSize") float thumbSize_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "horizontal") bool horizontal_ = false;
    ROCKS_PROPERTY(Transient, Name = "scrolled", Params = "offset") Event<float> scrolled_;
};

} // namespace rocks::ui
