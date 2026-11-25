#include "DockTabHeader.h"

namespace rocks::ui {

DockTabHeader::DockTabHeader(Allocator& allocator, const DockTabStyle& style) noexcept
    : Element(allocator), style_(style) {
    SetClipToBounds(true);
}

void DockTabHeader::SetForeground(bool foreground) noexcept {
    if (foreground_ != foreground) {
        foreground_ = foreground;
        InvalidatePaint();
    }
}

bool DockTabHeader::IsForeground() const noexcept {
    return foreground_;
}

bool DockTabHeader::IsHovered() const noexcept {
    return hoverCount_ != 0;
}

void DockTabHeader::OnMouseEnter(uint32) noexcept {
    if (++hoverCount_ == 1) {
        InvalidatePaint();
    }
}

void DockTabHeader::OnMouseLeave(uint32) noexcept {
    if (hoverCount_ && --hoverCount_ == 0) {
        InvalidatePaint();
    }
}

Vec4 DockTabHeader::Padding() const noexcept {
    const Vec4 padding = style_.tabPadding;
    return {padding.x, padding.y * style_.verticalPaddingScale, padding.z,
            padding.w * style_.verticalPaddingScale};
}

Vec2 DockTabHeader::ComputeDesiredSize(float) const noexcept {
    const Element* child = ChildAt(0);
    const Vec2 content =
        child && child->GetVisibility() != Visibility::Collapsed ? child->GetDesiredSize() : Vec2{};
    const Vec4 padding = Padding();
    return {content.x + padding.x + padding.z, content.y + padding.y + padding.w};
}

Result<void> DockTabHeader::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    Element* child = ChildAt(0);
    if (!child || child->GetVisibility() == Visibility::Collapsed) {
        return Result<void>::Success();
    }

    // 내용은 여백 안에서 세로 가운데다(UE SDockTab 내용 슬롯 VAlign_Center).
    const Vec4 padding = Padding();
    const Vec2 inner{math::Max(0.0f, geometry.size.x - padding.x - padding.z),
                     math::Max(0.0f, geometry.size.y - padding.y - padding.w)};
    const float height = math::Min(inner.y, child->GetDesiredSize().y);
    const float top = padding.y + (inner.y - height) * 0.5f;
    return child->ArrangeIfNeeded(
        GetGeometry().MakeChild({inner.x, height}, {1, 0, 0, 1, padding.x, top}, {}, {}));
}

Result<void> DockTabHeader::OnPaint(PaintList& list, int32 layer) const noexcept {
    // UE SDockTab::GetImageBrush: 앞 탭 → ForegroundBrush, 올림 → HoveredBrush, 그 밖 →
    // NormalBrush.
    const StyleBrush& brush = foreground_   ? style_.foreground
                              : IsHovered() ? style_.hovered
                                            : style_.normal;
    if (brush.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    return list.TryAddBrush(GetGeometry(), brush.Resolve(list.Theme(), list.ColorContext()),
                            {1, 1, 1, 1}, layer);
}

ColorRef DockTabHeader::PaintForegroundColor(bool) const noexcept {
    // 사용자가 정한 전경색이 있으면 그것이다(UE SDockTab ForegroundColor 인자).
    const ColorRef assigned = GetForegroundColor();
    if (assigned.source != ColorSource::Foreground) {
        return assigned;
    }

    // UE SDockTab::GetForegroundColor(비활성 창의 탭 기준): 앞 탭 ForegroundForeground, 올림
    // HoveredForeground, 그 밖 NormalForeground. 키보드 초점이 있는 탭(activeForeground)은 초점
    // 경로가 생기면 더한다.
    return foreground_   ? style_.foregroundForeground
           : IsHovered() ? style_.hoveredForeground
                         : style_.normalForeground;
}

} // namespace rocks::ui
