#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Input/MotorValues.h>
#include <InputActions/Haptics/HapticChannel.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 게임패드 진동 효과다(UE UForceFeedbackEffect — Docs/Design/InputActions 6.1절). id가 효과의
// 정체이고(0이면 무효 — StopHapticEffect가 이것으로 고른다), 채널마다 커브 하나다. 기본은 채널이
// 없다(생성자는 할당하지 않는다 — 길이 0 효과는 재생하자마자 끝난다). 에셋은 HapticEffectAsset이다
// (2.8절).
//
// 사용 예:
//   HapticEffect hit(allocator);
//   hit.id = HitEffectId;
//   HapticChannel channel(allocator);
//   (void)channel.curve.TryAddKey(0.0f, 1.0f);
//   (void)channel.curve.TryAddKey(0.2f, 0.0f);
//   (void)hit.channels.TryPushBack(Move(channel));
//   (void)playerController.Haptics().PlayHapticEffect(hit, {});
ROCKS_STRUCT()
struct ROCKS_INPUTACTIONS_API HapticEffect {
    ROCKS_GENERATED_BODY()

    explicit HapticEffect(Allocator& allocator) noexcept : channels(allocator) {
    }

    // 모든 채널 커브의 마지막 키 시간 중 최댓값이다(UE GetDuration — 키 없는 커브는 0, 채널이
    // 없으면 0).
    float Duration() const noexcept;

    // 시각 time(초)의 채널 값을 values에 최댓값으로 합친다(UE GetValues — 모터마다
    // Clamp(값, 지금 값, 1)).
    void AccumulateValues(float time, MotorValues& values) const noexcept;

    // 깊은 사본이다(실패면 이것은 그대로다). 할당 실패는 OutOfMemory다.
    Result<void> TryCopyFrom(const HapticEffect& other) noexcept;

    ROCKS_PROPERTY() Guid id;
    ROCKS_PROPERTY() Array<HapticChannel> channels;
};

} // namespace rocks::input
