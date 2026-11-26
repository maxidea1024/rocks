#pragma once

namespace rocks::input {

// 컨텍스트 추가·제거 옵션이다(UE FModifyContextOptions — Docs/Design/InputActions 4.1절).
struct ContextOptions {
    // 다시 만들 때 눌려 있던 키의 새 Boolean 바인딩을 놓을 때까지 무시한다. 예약된 요청들끼리 AND로
    // 합친다.
    bool ignorePressedKeysUntilRelease = true;
    // 다음 평가를 기다리지 않고 그 자리에서 다시 만든다.
    bool forceImmediately = false;
    // 사용자 설정에 컨텍스트를 등록·해제한다(5.2절 — IA4).
    bool notifyUserSettings = false;
};

} // namespace rocks::input
