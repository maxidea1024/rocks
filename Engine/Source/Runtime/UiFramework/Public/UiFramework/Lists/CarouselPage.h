#pragma once
#include <Core/Delegate.h>
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

// 보이지 않는 페이지를 비워 둘 수 있도록 화면 생성기를 보관한다. 생성 결과는 표시 중에 재사용한다.
class ROCKS_UIFRAMEWORK_API CarouselPage final : public SizeBox {
    ROCKS_UI_ELEMENT(CarouselPage, SizeBox)

public:
    using Factory = Delegate<Result<SharedPtr<Element>>()>;
    CarouselPage(Allocator& allocator, Factory&& factory) noexcept;
    Result<void> Materialize() noexcept;
    void ReleaseContent() noexcept;

private:
    Factory factory_;
};

} // namespace rocks::ui
