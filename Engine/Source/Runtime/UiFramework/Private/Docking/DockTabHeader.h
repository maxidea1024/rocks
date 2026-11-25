#pragma once

#include <UiCore/Element.h>
#include <UiCore/Style.h>

// 도킹 탭 머리다(UE SDockTab의 탭 부분, Docs/Design/UiToolkit 9.1절 UW4). 사용자가 준 머리 내용을
// 탭 여백 안에 두고, 선택·올림 상태에 따라 탭 brush를 그리며 자식 전경색을 정한다. 내용이 탭 밖에
// 그리지 않게 자른다. DockManager만 만든다.
namespace rocks::ui {

class DockTabHeader final : public Element {
public:
    DockTabHeader(Allocator& allocator, const DockTabStyle& style) noexcept;

    // 탭 묶음에서 앞에 보이는 탭인지다(UE SDockTab::IsForeground).
    void SetForeground(bool foreground) noexcept;
    bool IsForeground() const noexcept;
    bool IsHovered() const noexcept;

    void OnMouseEnter(uint32 user) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

protected:
    ColorRef PaintForegroundColor(bool disabled) const noexcept override;

private:
    // 세로 배율을 곱한 탭 여백이다(UE SDockTab::GetTabPadding).
    Vec4 Padding() const noexcept;

    const DockTabStyle& style_;
    uint32 hoverCount_ = 0;
    bool foreground_ = false;
};

} // namespace rocks::ui
