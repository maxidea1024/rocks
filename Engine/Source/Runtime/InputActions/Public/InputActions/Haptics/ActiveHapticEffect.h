#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <InputActions/Haptics/HapticEffect.h>

namespace rocks::input {

// 재생 중인 진동 효과 하나다(UE FActiveForceFeedbackEffect — Docs/Design/InputActions 6.2절).
// HapticPlayer가 효과 사본·옵션·재생 시각을 둔다.
struct ActiveHapticEffect {
    explicit ActiveHapticEffect(Allocator& allocator) noexcept : effect(allocator), tag(allocator) {
    }

    HapticEffect effect;
    String tag;
    bool looping = false;
    bool ignoreTimeDilation = false;
    bool playWhilePaused = false;
    // 재생 시작부터 흐른 시간(초)이다.
    double playTime = 0.0;
};

} // namespace rocks::input
