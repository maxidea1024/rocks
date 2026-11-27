#pragma once

#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 일정한 프레임 간격으로 자식의 텍스처를 갱신하는 컨테이너다. 캡처와 텍스처 게시를 호스트와
// 연동한다. 효과 재질은 머티리얼 셰이더가 생기면 Material 참조로 더한다(UiToolkit 3.6절·9.1절
// UW1a).
ROCKS_CLASS()
class ROCKS_UIWIDGET_API OffscreenLayerWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit OffscreenLayerWidget(Allocator& allocator) noexcept;

    Result<void> SetPhase(uint32 phase, uint32 count) noexcept;
    Result<void> SetEffectTint(LinearColor tint) noexcept;
    void RequestRender() noexcept;

protected:
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "effectTint") LinearColor effectTint_ { 1, 1, 1, 1 };
    ROCKS_PROPERTY(Name = "phase") uint32 phase_ = 0;
    ROCKS_PROPERTY(Name = "phaseCount") uint32 phaseCount_ = 1;
};

} // namespace rocks::ui
