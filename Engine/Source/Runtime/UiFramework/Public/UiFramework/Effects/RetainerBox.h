#pragma once

#include <UiCore/Declarative.h>

#include <UiCore/WidgetSurface.h>
#include <UiCore/CacheRoot.h>
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

// 자식의 그리기 결과를 텍스처로 보관한다. 배치와 입력은 보관 중에도 실제 자식 트리를 사용한다.
class ROCKS_UIFRAMEWORK_API RetainerBox final : public SizeBox, public WidgetSurface {
public:
    struct Args {
        ElementChild content{};
        uint32 phase = 0;
        uint32 phaseCount = 1;
        LinearColor effectTint{1, 1, 1, 1};
        ElementArgs common{};
    };

    // Build·Make로 중첩할 때 자식과 그리기 속성을 한 번에 연결한다.
    Result<void> Construct(Args&& args) noexcept;

    WidgetSurface* GetWidgetSurface() noexcept override;
    WidgetSurfaceKind SurfaceKind() const noexcept override;
    bool WantsManagedSurface() const noexcept override;
    SlotHandle ManagedSurface() const noexcept override;
    void SetManagedSurface(SlotHandle texture) noexcept override;

    explicit RetainerBox(Allocator& allocator) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    bool RemoveChild(Element& child) noexcept override;

    // 전체 주기 중 phase 프레임에 생략 가능한 캡처를 예약한다. 명시적 갱신은 즉시 허용한다.
    Result<void> SetPhase(uint32 phase, uint32 count) noexcept;
    bool NeedsCapture(uint64 frame) const noexcept override;
    Result<void> SetEffectTint(LinearColor tint) noexcept;
    void RequestRender() noexcept;
    // 자식을 타깃 로컬 좌표로 기록한다. 성공적으로 그린 뒤 SetRetainedTexture로 게시한다.
    Result<void> RecordContent(PaintList& output) const noexcept override;
    void SetRetainedTexture(SlotHandle texture) noexcept;

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

protected:
    Span<const SharedPtr<Element>> PaintChildren() const noexcept override;
    // 보관 텍스처로 그리는 동안에도 자식은 같은 자리에 보이므로 hit 대상이다(20.4절).
    Span<const SharedPtr<Element>> HitTestChildren() const noexcept override;

private:
    LinearColor effectTint_{1, 1, 1, 1};
    mutable CacheRoot contentCache_;
    SlotHandle retained_{};
    uint32 phase_ = 0;
    uint32 phaseCount_ = 1;
    bool requested_ = true;
};

} // namespace rocks::ui
