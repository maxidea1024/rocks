#pragma once

#include <UiFramework/Controls/Slider.h>

namespace rocks::ui {

// 원의 아래가 0도, 왼쪽이 90도다. 가동 범위 밖을 누르면 가까운 끝점을 고르며,
// 드래그 중에는 이전 각도와의 차이를 따라가므로 0도 경계에서 반대쪽 끝으로 튀지 않는다.
class ROCKS_UIFRAMEWORK_API RadialSlider final : public Slider {
public:
    struct Args {
        float minimum = 0;
        float maximum = 1;
        float step = 0.01f;
        float value = 0;
        float startAngle = 60;
        float endAngle = 300;
        float angularOffset = 0;
        bool snapToStep = false;
        bool showHandle = true;
        bool showHand = false;
        // 기본은 슬라이더 스타일과 같은 테마 색이다(트랙 Input, 진행 Primary, 손잡이 Foreground).
        LinearColor track = DefaultThemeColors().Get(ThemeSlot::Input);
        LinearColor progress = DefaultThemeColors().Get(ThemeSlot::Primary);
        LinearColor handle = DefaultThemeColors().Get(ThemeSlot::Foreground);
        Delegate<void(float)> onChanged{};
        ElementArgs common{};
    };

    explicit RadialSlider(Allocator& allocator) noexcept;
    Result<void> Construct(Args&& args) noexcept;
    static bool IsValidArc(float startDegrees, float endDegrees, float offsetDegrees) noexcept;
    Result<void> SetArc(float startDegrees, float endDegrees, float offsetDegrees = 0) noexcept;
    void SetHandVisibility(bool showHandle, bool showHand) noexcept;
    void SetHandleColor(LinearColor color) noexcept;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;

protected:
    void UpdateFromPointer(Vec2 position, bool firstPoint) noexcept override;

private:
    float start_ = 60;
    float end_ = 300;
    float offset_ = 0;
    float previousAngle_ = 0;
    float dragAngle_ = 0;
    bool hasAngle_ = false;
    bool showHandle_ = true;
    bool showHand_ = false;
    LinearColor handleColor_{1, 1, 1, 1};
};

} // namespace rocks::ui
