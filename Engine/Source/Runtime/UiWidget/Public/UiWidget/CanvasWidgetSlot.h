#pragma once

#include <UiWidget/WidgetSlot.h>
#include <UiFramework/Layout/AnchorCanvas.h>

namespace rocks::ui {

// 앵커와 오프셋으로 HUD를 배치한다. 늘어나는 축의 끝 오프셋은 반대쪽 여백이다. 배치는 위젯 트리의
// 슬롯에 저장되어 화면을 재구성할 때도 유지된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API CanvasWidgetSlot final : public WidgetSlot {
    ROCKS_GENERATED_BODY()
public:
    CanvasSlot Layout() const noexcept;
    Vec2 Position() const noexcept;
    Vec2 Size() const noexcept;
    Vec4 Offsets() const noexcept;
    Vec2 Minimum() const noexcept;
    Vec2 Maximum() const noexcept;
    Vec2 Alignment() const noexcept;
    float ZOrder() const noexcept;
    bool AutoSize() const noexcept;
    // 화면에 붙기 전후 모두 사용할 수 있다. 잘못된 앵커나 값은 기존 배치를 유지한다.
    Result<void> UpdateLayout(ObjectRegistry& objects, const CanvasSlot& layout) noexcept;
    Result<void> SetPosition(ObjectRegistry& objects, Vec2 position) noexcept;
    Result<void> SetSize(ObjectRegistry& objects, Vec2 size) noexcept;
    Result<void> SetOffsets(ObjectRegistry& objects, Vec4 offsets) noexcept;
    Result<void> SetAnchors(ObjectRegistry& objects, Vec2 minimum, Vec2 maximum) noexcept;
    Result<void> SetMinimum(ObjectRegistry& objects, Vec2 minimum) noexcept;
    Result<void> SetMaximum(ObjectRegistry& objects, Vec2 maximum) noexcept;
    Result<void> SetAlignment(ObjectRegistry& objects, Vec2 alignment) noexcept;
    Result<void> SetAutoSize(ObjectRegistry& objects, bool enabled) noexcept;
    Result<void> SetZOrder(ObjectRegistry& objects, float order) noexcept;

private:
    friend class CanvasPanelWidget;
    Result<void> Configure(const CanvasSlot& layout) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "anchorMin") Vec2 anchorMin_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "anchorMax") Vec2 anchorMax_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "offset") Vec4 offset_ { 0, 0, 100, 100 };
    ROCKS_PROPERTY(ReadOnly, Name = "alignment") Vec2 alignment_ { 0.5f, 0.5f };
    ROCKS_PROPERTY(ReadOnly, Name = "zOrder") float zOrder_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "autoSize") bool autoSize_ = false;
};

} // namespace rocks::ui
