#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 자식들을 같은 영역에 겹친다. zOrder가 높을수록 앞에 있고 같은 값이면 추가 순서를 따른다.
class ROCKS_UIFRAMEWORK_API Overlay : public Element {
    ROCKS_UI_ELEMENT(Overlay, Element)

public:
    explicit Overlay(Allocator& allocator) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    Result<void> TryAddSlot(const SharedPtr<Element>& child, int32 zOrder) noexcept;
    Result<void> AttachPopupLayer(const PopupRequest& popup) noexcept override;
    void DismissHostedPopups() noexcept override;
    bool IsHostedPopupLayer(const Element& child) const noexcept override;
    bool RemoveChild(Element& child) noexcept override;

    Vec2 ComputeDesiredSize(float) const noexcept override;

    Result<void> Arrange(const Geometry& geometry) noexcept override;

protected:
    // 품은 팝업 층의 배치 경계(Overlay ∩ 기준 지점의 팝업 허용 영역 — UW7d, Overlay 로컬)다.
    // 팝업 층이 아니거나 허용 영역을 모르면 비어 있다(층 전체). Arrange를 대신하는 파생
    // 클래스(WindowChrome)도 팝업 층에 이 경계를 준다(19.5절).
    Optional<Box2<float>> PopupLayerArea(const Element& child) const noexcept;
    // anchor가 보이는 경로에 없는 팝업을 처리한다(22.6절): 보통 팝업은 닫고, keepWhileHidden 팝업은
    // 층을 접었다가 다시 보이면 되돌린다(고르지 않은 탭의 오류 팝업이 다른 탭 위에 남지 않게).
    // Arrange 처음에 부른다 — Arrange를 대신하는 파생 클래스(WindowChrome)도 부른다.
    void SyncPopupVisibility() noexcept;

private:
    bool StartsPaintLayer(usize previous, usize next) const noexcept override;

    void OnDescendantDetached(const Element& subtree) noexcept override;
    struct OpenPopup {
        WeakPtr<Element> anchor;
        Element* layer;
        void (*close)(Element&) noexcept;
        // 층을 붙일 때의 보이기다(anchor가 다시 보이면 되돌린다 — 22.6절).
        Visibility shown;
        bool keepWhileHidden;
    };
    Array<OpenPopup> popups_;
    bool dismissingPopups_ = false;
    Array<int32> orders_;
};

} // namespace rocks::ui
