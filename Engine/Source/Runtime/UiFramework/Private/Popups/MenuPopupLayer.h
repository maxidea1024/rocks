#pragma once

#include <UiFramework/Popups/MenuAnchor.h>

namespace rocks::ui {

// 하나의 열린 메뉴가 차지하는 화면 레이어. 자식 영역 밖은 메뉴를 닫기 위한 입력 표면이다.
class MenuPopupLayer final : public Element {
public:
    MenuPopupLayer(Allocator& allocator, MenuAnchor& anchor) noexcept;
    void Disconnect() noexcept;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    PointerReply OnPreviewPointerDown(const PointerEvent& event) noexcept override;
    bool OnPreviewKeyDown(uint32 user, const input::InputEvent& event) noexcept override;

private:
    MenuAnchor* anchor_;
};

} // namespace rocks::ui
