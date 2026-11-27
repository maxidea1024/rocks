#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/Declarative.h>

namespace rocks::ui {

// offset의 x/y는 위치, z/w는 크기다. 늘어나는 축에서는 z/w가 끝 여백이다.
struct ROCKS_UIFRAMEWORK_API CanvasSlot {
    Vec2 anchorMin{};
    Vec2 anchorMax{};
    Vec4 offset{0, 0, 100, 100};
    Vec2 alignment{0.5f, 0.5f};
    float zOrder = 0;
    bool autoSize = false;

    bool IsValid() const noexcept;
    bool operator==(const CanvasSlot&) const noexcept = default;
};

// anchor와 offset으로 자식을 배치한다. 같은 zOrder는 추가 순서를 따른다.
class ROCKS_UIFRAMEWORK_API AnchorCanvas : public Element {
    ROCKS_UI_ELEMENT(AnchorCanvas, Element)

public:
    struct SlotArgs {
        CanvasSlot layout{};
        ElementChild content{};
    };
    struct Args {
        Span<const SlotArgs> slots{};
        ElementArgs common{};
    };

    // slots는 Construct 중에만 빌린다. 각 슬롯은 기준점과 오프셋을 복사하고 자식을 소유한다.
    // 같은 zOrder의 슬롯은 선언 순서대로 그려진다.
    Result<void> Construct(const Args& args) noexcept;

    explicit AnchorCanvas(Allocator& allocator) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    bool RemoveChild(Element& child) noexcept override;
    Result<void> TryAddSlot(const SharedPtr<Element>& child, const CanvasSlot& slot) noexcept;
    // 기존 자식을 유지하면서 배치 및 페인트·히트 테스트 순서를 함께 바꾼다.
    Result<void> SetSlot(Element& child, const CanvasSlot& slot) noexcept;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;

private:
    bool StartsPaintLayer(usize previous, usize next) const noexcept override;

    Array<CanvasSlot> slots_;
};

} // namespace rocks::ui
