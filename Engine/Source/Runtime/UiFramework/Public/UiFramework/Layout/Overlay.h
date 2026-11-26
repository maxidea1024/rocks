#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

// 자식들을 같은 영역에 겹친다. zOrder가 높을수록 앞에 있고 같은 값이면 추가 순서를 따른다.
class ROCKS_UIFRAMEWORK_API Overlay : public Element {
public:
    explicit Overlay(Allocator& allocator) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    Result<void> TryAddSlot(const SharedPtr<Element>& child, int32 zOrder) noexcept;
    Result<void> AttachPopupLayer(const PopupRequest& popup) noexcept override;
    void DismissHostedPopups() noexcept override;
    bool RemoveChild(Element& child) noexcept override;

    Vec2 ComputeDesiredSize(float) const noexcept override;

    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    bool StartsPaintLayer(usize previous, usize next) const noexcept override;

    void OnDescendantDetached(const Element& subtree) noexcept override;
    struct OpenPopup {
        WeakPtr<Element> anchor;
        Element* layer;
        void (*close)(Element&) noexcept;
    };
    Array<OpenPopup> popups_;
    bool dismissingPopups_ = false;
    Array<int32> orders_;
};

} // namespace rocks::ui
