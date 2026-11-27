#pragma once
#include <UiWidget/Widget.h>
#include <UiFramework/Controls/Button.h>

namespace rocks::ui {

class CarouselWidget;

// 연결한 캐러셀의 페이지마다 버튼 하나를 만든다. 버튼을 누르면 자동 넘김을 멈추고 해당 페이지를
// 연다. 페이지가 추가·삭제되거나 코드에서 현재 페이지를 바꿔도 선택 표시가 함께 바뀐다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API CarouselNavBarWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    using ButtonFactory = Delegate<Result<SharedPtr<Button>>(usize)>;
    Result<void> SetLinkedCarousel(ObjectRegistry& objects, ObjectHandle carousel) noexcept;
    ObjectHandle LinkedCarousel() const noexcept;
    Result<void> SetButtonPadding(Vec4 padding) noexcept;
    Result<void> SetButtonFactory(ButtonFactory&& factory) noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    Result<void> Connect(ObjectRegistry& objects, const SharedPtr<Element>& native) noexcept;
    Result<void> Refresh(ObjectRegistry& objects, Element& native, bool rebuild = false) noexcept;
    void Disconnect() noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "linkedCarousel") ObjectRef<CarouselWidget> linkedCarousel_;
    ROCKS_PROPERTY(ReadOnly, Name = "buttonPadding") Vec4 buttonPadding_ {};
    ROCKS_NATIVE() ButtonFactory factory_;
    ROCKS_NATIVE() ObjectRegistry* registry_ = nullptr;
    ROCKS_NATIVE() DelegateHandle subscription_ {};
};

} // namespace rocks::ui
