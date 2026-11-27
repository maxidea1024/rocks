#pragma once

#include <UiCore/Declarative.h>

#include <UiCore/WidgetSurface.h>
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

// 호스트가 렌더링한 장면 텍스처 위에 자식 UI를 배치한다. 장면 수명과 카메라는 호스트가 소유한다.
class ROCKS_UIFRAMEWORK_API Viewport final : public SizeBox, public WidgetSurface {
    ROCKS_UI_ELEMENT(Viewport, SizeBox)

public:
    struct Args {
        ElementChild content{};
        Bindable<LinearColor> background{LinearColor{0, 0, 0, 1}};
        Bindable<SlotHandle> scene{};
        ElementArgs common{};
    };

    // Build·Make로 중첩할 때 자식과 그리기 속성을 한 번에 연결한다.
    Result<void> Construct(Args&& args) noexcept;

    WidgetSurface* GetWidgetSurface() noexcept override;
    WidgetSurfaceKind SurfaceKind() const noexcept override;
    bool WantsManagedSurface() const noexcept override;
    SlotHandle ManagedSurface() const noexcept override;
    void SetManagedSurface(SlotHandle texture) noexcept override;

    explicit Viewport(Allocator& allocator) noexcept;

    // 장면 source다(Docs/Design/UiToolkit 3.11·18.3절). 표면 기록기가 viewport 크기의 렌더 타깃을
    // 만들고 source가 게시한 장면 pass로 채운다. 비우면 외부 장면 텍스처(scene)를 쓴다.
    void SetSceneSource(SharedPtr<ViewportSource> scene) noexcept;
    SharedPtr<ViewportSource> GetViewportSource() const noexcept override;
    void SetSceneTexture(SlotHandle texture) noexcept;
    Result<void> SetBackground(LinearColor color) noexcept;

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    SharedPtr<ViewportSource> source_;
    SlotHandle managed_{};
    ElementAttribute<SlotHandle, InvalidateReason::Paint> scene_;
    ElementAttribute<LinearColor, InvalidateReason::Paint> background_;
};

} // namespace rocks::ui
