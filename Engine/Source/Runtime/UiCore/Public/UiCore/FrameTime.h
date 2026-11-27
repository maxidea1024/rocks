#pragma once

#include <Core/Delegate.h>

namespace rocks::ui {

// 한 프레임의 UI 시각이다(초 — UE FPaintArgs GetCurrentTime·GetDeltaTime, Docs/Design/UiToolkit
// 20.3절). current는 시계가 준 값 그대로이고, delta는 MaxFrameDeltaSeconds로 자른 앞 프레임과의
// 간격이다. Application::BeginFrame이 정하고 그리기 목록·host 서비스 묶음이 나른다.
struct FrameTime {
    double current = 0;
    double delta = 0;
};

// 시각 공급(초, 줄지 않는 값)이다. Application::SetClock이 받는다. 비우면 단조 시계다.
using ClockFunction = Delegate<double()>;

// 프레임 간격의 상한(초)이다. 더 긴 간격은 이 값으로 자른다(UE FSlateApplication::TickTime의
// MaxQuantumBeforeClamp 1/8초 — SlateApplication.cpp 1682행). 중단점·창 끌기 뒤 애니메이션이
// 한꺼번에 뛰지 않게 한다.
inline constexpr double MaxFrameDeltaSeconds = 1.0 / 8.0;

} // namespace rocks::ui
