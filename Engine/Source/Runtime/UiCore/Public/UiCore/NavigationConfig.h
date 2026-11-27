#pragma once

#include <UiCore/KeyEvent.h>

namespace rocks::ui {

// 키 → navigation 방향·동작 대응이다(UE FNavigationConfig — Docs/Design/UiToolkit 20.7절). 기본값은
// UE와 같다: 방향키 → 상하좌우(NavigationConfig.cpp 19–29행), Tab → Next·Shift+Tab →
// Previous(Ctrl·Alt·Command가 눌려 있으면 아님 — 59–81행), 확인 Enter·Space·패드 South, 뒤로
// Escape·패드 East(31–38행), 패드 D-pad → 상하좌우, 왼쪽 스틱은 임계값 0.5(13–17행)와 반복
// 간격(첫 0.5초, 다음 0.25초, 세게 밀면 절반 — 153–162행). Application이 사용자별로 들고, 바꿀 수
// 있다.
//
// 사용 예:
//   NavigationConfig config;
//   config.tabNavigation = false;                       // Tab으로 초점을 옮기지 않는 게임 화면
//   (void)config.SetKeyRule(input::Key::W, FocusDirection::Up);
//   app.SetNavigationConfig(user, config);
class ROCKS_UICORE_API NavigationConfig {
public:
    // 키 대응 규칙 수의 상한이다.
    static constexpr usize MaxKeyRules = 32;

    NavigationConfig() noexcept;

    // 기본 대응이다(Application 밖 — Element::SendKey가 쓴다).
    static const NavigationConfig& Default() noexcept;

    // 키를 방향에 잇는다(같은 키는 바꾼다). 방향이 없으면 그 규칙을 지운다. 규칙이 가득 차면
    // CapacityExceeded다.
    Result<void> SetKeyRule(input::Key key, Optional<FocusDirection> direction) noexcept;

    // KeyDown의 navigation 방향이다(없으면 비어 있다).
    Optional<FocusDirection> DirectionFromKey(const input::InputEvent& event,
                                              const KeyChord& chord) const noexcept;
    // 키의 동작이다(KeyDown·KeyUp 모두 — 버튼은 누름과 뗌을 같은 동작으로 본다).
    NavigationAction ActionFromKey(const input::InputEvent& event) const noexcept;
    // 패드 버튼의 방향(D-pad)과 동작(South 확인·East 뒤로)이다.
    Optional<FocusDirection> DirectionFromGamepad(input::GamepadButton button) const noexcept;
    NavigationAction ActionFromGamepad(input::GamepadButton button) const noexcept;
    // 왼쪽 스틱 방향이다(x 오른쪽·y 위가 양수). 두 축이 모두 넘으면 더 크게 민 축이다.
    Optional<FocusDirection> DirectionFromAnalog(float x, float y) const noexcept;
    // 같은 방향으로 repeats번 navigation한 뒤 다음까지의 간격(초)이다(UE GetRepeatRateForPressure).
    static double AnalogRepeatInterval(float pressure, uint32 repeats) noexcept;

    bool keyNavigation = true;
    bool tabNavigation = true;
    bool analogNavigation = true;
    // 왼쪽 스틱이 navigation이 되는 세기다(UE AnalogNavigationHorizontal·VerticalThreshold).
    float analogHorizontalThreshold = 0.5f;
    float analogVerticalThreshold = 0.5f;

private:
    struct KeyRule {
        input::Key key;
        FocusDirection direction;
    };

    KeyRule rules_[MaxKeyRules]{};
    usize ruleCount_ = 0;
};

} // namespace rocks::ui
