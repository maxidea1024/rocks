#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Input/MotorValues.h>
#include <InputActions/Haptics/ActiveDynamicHaptic.h>
#include <InputActions/Haptics/ActiveHapticEffect.h>
#include <InputActions/Haptics/DynamicHaptic.h>
#include <InputActions/Haptics/HapticEffect.h>
#include <InputActions/Haptics/HapticParams.h>
#include <InputActions/Haptics/HapticUpdateContext.h>

namespace rocks {
class AssetLease;
class AssetManager;
} // namespace rocks

namespace rocks::input {

// 플레이어 하나의 게임패드 진동 재생·합성이다(UE APlayerController의 ActiveForceFeedbackEffects·
// DynamicForceFeedbacks·ProcessForceFeedbackAndHaptics — Docs/Design/InputActions 6.2·6.3절).
// Gameplay PlayerController가 하나 갖고 틱마다 Update를 부르며, 결과를 호스트가 패드에 쓴다.
//
// 사용 예:
//   HapticPlayer haptics(allocator);
//   HapticParams params;
//   params.tag = "Hit";
//   (void)haptics.PlayHapticEffect(hit, params);
//   HapticUpdateContext context;
//   context.deltaSeconds = context.unscaledDeltaSeconds = dt;
//   const MotorValues motors = haptics.Update(context);
//
// 할당: 재생(효과 사본)·동적 진동 시작 때 한다. Update는 할당하지 않는다. 스레드: 게임 스레드
// 전용이다.
class ROCKS_INPUTACTIONS_API HapticPlayer {
public:
    explicit HapticPlayer(Allocator& allocator) noexcept;
    HapticPlayer(const HapticPlayer&) = delete;
    HapticPlayer& operator=(const HapticPlayer&) = delete;

    // effect의 사본을 재생한다. params.tag가 비어 있지 않으면 같은 tag로 재생 중인 효과를 먼저
    // 지운다(사본을 다 만든 뒤 — 실패면 그대로). 오류: 무효 effect(id 0)는 InvalidArgument, 할당
    // 실패는 OutOfMemory(모두 그대로).
    Result<void> PlayHapticEffect(const HapticEffect& effect, const HapticParams& params) noexcept;

    // lease의 HapticEffectAsset을 재생한다(UE ClientPlayForceFeedback(UForceFeedbackEffect*) —
    // Docs/Design/InputActions 2.8절). CopyHapticEffect로 사본을 만들어 위 함수로 재생하므로
    // lease는 쥐지 않는다(재생 뒤 놓아도 된다). 오류: CopyHapticEffect의 오류와 위 함수의
    // 오류다(모두 그대로). AssetManager의 소유 스레드(Game)에서 부른다.
    Result<void> PlayHapticEffect(AssetManager& assets, const AssetLease& effect,
                                  const HapticParams& params) noexcept;

    // 재생 중인 효과를 멈춘다(UE ClientStopForceFeedback): effectId가 0이면 tag만, tag가 비면
    // effect만 보고, 둘 다 비면 모두 멈춘다.
    void StopHapticEffect(const Guid& effectId = {}, StringView tag = {}) noexcept;

    // 동적 진동을 시작하고 handle을 돌려준다. 오류: 비유한 intensity·duration은 InvalidArgument,
    // 할당 실패는 OutOfMemory다.
    Result<DynamicHapticHandle> StartDynamicHaptic(const DynamicHaptic& details) noexcept;

    // 세기·모터를 바꾼다(duration·경과 시간 그대로 — UE Update). 오류: 비유한 값은
    // InvalidArgument, 없는 handle은 NotFound다.
    Result<void> UpdateDynamicHaptic(DynamicHapticHandle handle,
                                     const DynamicHaptic& details) noexcept;

    // 동적 진동을 멈춘다(없으면 아무 일도 없다).
    void StopDynamicHaptic(DynamicHapticHandle handle) noexcept;

    // 효과·동적 진동을 모두 멈춘다.
    void StopAll() noexcept;

    // 한 틱 갱신하고 네 모터 값을 합친다(6.3절 — 끝난 효과·동적 진동은 지운다). enabled가
    // 거짓이어도 재생 시간은 흐른다(UE와 같다 — 값만 0).
    MotorValues Update(const HapticUpdateContext& context) noexcept;

    usize ActiveEffectCount() const noexcept {
        return effects_.Size();
    }

    usize ActiveDynamicCount() const noexcept {
        return dynamics_.Size();
    }

private:
    Allocator* allocator_;
    Array<ActiveHapticEffect> effects_;
    Array<ActiveDynamicHaptic> dynamics_;
    DynamicHapticHandle nextHandle_ = InvalidDynamicHapticHandle + 1;
};

} // namespace rocks::input
