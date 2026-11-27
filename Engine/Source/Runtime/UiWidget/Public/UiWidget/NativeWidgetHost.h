#pragma once

#include <UiWidget/Widget.h>

namespace rocks::ui {

// 게임 코드가 만든 Element를 위젯 트리에 삽입한다. 붙일 Element는 부모가 없어야 하며 화면 재생성 시
// 다시 연결된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API NativeWidgetHost final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetContent(const SharedPtr<Element>& content) noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    ROCKS_NATIVE() SharedPtr<Element> content_;
};

} // namespace rocks::ui
