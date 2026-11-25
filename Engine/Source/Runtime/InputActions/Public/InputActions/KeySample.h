#pragma once

#include <InputActions/InputValue.h>

namespace rocks::input {

// 키 하나를 이번 프레임 입력에서 읽은 결과다(2.2절). 터치의 raw는 화면 좌표라 Boolean으로 바꾸면
// x = 0 열의 터치가 비작동이 된다. Boolean 액션은 raw 대신 down || pressed로 1·0을 만든다(한 프레임
// 탭 포함 — IA2 평가기).
struct KeySample {
    // 원시값이다(디지털 1·0, 패드 축은 dead zone 전 값, 마우스 축은 이번 프레임 delta, 터치는 화면
    // 좌표·놓으면 0). 한 프레임에 눌렀다 뗀 디지털 키는 1이다(눌림 값 — UE와 같다).
    InputValue raw;
    // 디지털은 눌림, 축은 nonzero, 터치는 닿아 있음이다.
    bool down = false;
    // 이번 프레임에 눌렸다·놓였다(디지털·터치만 — 축은 거짓).
    bool pressed = false;
    bool released = false;
};

} // namespace rocks::input
