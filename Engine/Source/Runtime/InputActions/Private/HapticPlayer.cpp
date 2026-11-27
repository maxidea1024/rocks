#include <Core/Math/MathUtility.h>
#include <InputActions/Haptics/HapticChannel.generated.h>
#include <InputActions/Haptics/HapticEffect.generated.h>
#include <InputActions/Haptics/HapticPlayer.h>
#include <InputActions/InputAssetAccess.h>
#include <Object/ValueCopy.h>

// 게임패드 진동 재생·합성이다(Docs/Design/InputActions 6.1–6.3절 — UE
// ForceFeedbackEffect.cpp:47-184, PlayerController.cpp:4191-4267·4416-4470·4704-4780).
namespace rocks::input {

namespace {

// UE Clamp(value, current, 1): 지금 값보다 작으면 지금 값, 1보다 크면 1이다(최댓값 합성). NaN은
// 지금 값에 진다(UE Clamp는 NaN이면 1 — Rocks는 진동을 키우지 않는 쪽을 고른다, 6.2절).
float HapticMax(float value, float current) noexcept {
    if (!(value > current)) {
        return current;
    }

    return value < 1.0f ? value : 1.0f;
}

// 배율을 곱하고 [0, 1]로 자른다(UE ForceFeedbackScale).
float HapticScaled(float value, float scale) noexcept {
    const float scaled = value * scale;
    if (!(scaled > 0.0f)) {
        return 0.0f;
    }

    return scaled < 1.0f ? scaled : 1.0f;
}

bool DynamicIsFinite(const DynamicHaptic& details) noexcept {
    return math::IsFinite(details.intensity) && math::IsFinite(details.duration);
}

void AccumulateMotors(float value, bool leftLarge, bool leftSmall, bool rightLarge, bool rightSmall,
                      MotorValues& values) noexcept {
    if (leftLarge) {
        values.leftLarge = HapticMax(value, values.leftLarge);
    }

    if (leftSmall) {
        values.leftSmall = HapticMax(value, values.leftSmall);
    }

    if (rightLarge) {
        values.rightLarge = HapticMax(value, values.rightLarge);
    }

    if (rightSmall) {
        values.rightSmall = HapticMax(value, values.rightSmall);
    }
}

} // namespace

float HapticEffect::Duration() const noexcept {
    float duration = 0.0f;
    for (const HapticChannel& channel : channels) {
        if (auto range = channel.curve.TryGetTimeRange(); range && range.Value().max > duration) {
            duration = range.Value().max;
        }
    }

    return duration;
}

void HapticEffect::AccumulateValues(float time, MotorValues& values) const noexcept {
    for (const HapticChannel& channel : channels) {
        AccumulateMotors(channel.curve.Evaluate(time, 0.0f), channel.affectsLeftLarge,
                         channel.affectsLeftSmall, channel.affectsRightLarge,
                         channel.affectsRightSmall, values);
    }
}

Result<void> HapticEffect::TryCopyFrom(const HapticEffect& other) noexcept {
    // 생성 깊은 복사다(실패면 이것은 그대로다 — Object/ValueCopy.h).
    return detail::TryCopyValue(*this, other, channels.GetAllocator());
}

HapticPlayer::HapticPlayer(Allocator& allocator) noexcept
    : allocator_(&allocator), effects_(allocator), dynamics_(allocator) {
}

Result<void> HapticPlayer::PlayHapticEffect(const HapticEffect& effect,
                                            const HapticParams& params) noexcept {
    if (effect.id == Guid{}) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 사본을 다 만든 뒤 바꾼다(실패면 그대로).
    ActiveHapticEffect active(*allocator_);
    if (auto copied = active.effect.TryCopyFrom(effect); !copied) {
        return copied;
    }

    if (auto assigned = active.tag.TryAssign(params.tag); !assigned) {
        return assigned;
    }

    active.looping = params.looping;
    active.ignoreTimeDilation = params.ignoreTimeDilation;
    active.playWhilePaused = params.playWhilePaused;
    if (auto reserved = effects_.TryEnsureCapacity(effects_.Size() + 1); !reserved) {
        return reserved;
    }

    // 같은 tag 교체(UE :4195-4206).
    if (!params.tag.IsEmpty()) {
        for (usize index = effects_.Size(); index > 0; --index) {
            if (effects_[index - 1].tag.View() == params.tag) {
                (void)effects_.TryRemoveAt(index - 1);
            }
        }
    }

    (void)effects_.TryPushBack(Move(active));
    return Result<void>::Success();
}

Result<void> HapticPlayer::PlayHapticEffect(AssetManager& assets, const AssetLease& effect,
                                            const HapticParams& params) noexcept {
    HapticEffect copy(*allocator_);
    if (auto copied = CopyHapticEffect(assets, effect, copy); !copied) {
        return copied;
    }

    return PlayHapticEffect(copy, params);
}

void HapticPlayer::StopHapticEffect(const Guid& effectId, StringView tag) noexcept {
    const bool anyEffect = effectId == Guid{};
    for (usize index = effects_.Size(); index > 0; --index) {
        const ActiveHapticEffect& active = effects_[index - 1];
        if ((anyEffect || active.effect.id == effectId) &&
            (tag.IsEmpty() || active.tag.View() == tag)) {
            (void)effects_.TryRemoveAt(index - 1);
        }
    }
}

Result<DynamicHapticHandle>
HapticPlayer::StartDynamicHaptic(const DynamicHaptic& details) noexcept {
    if (!DynamicIsFinite(details)) {
        return Result<DynamicHapticHandle>::Failure(ErrorCode::InvalidArgument);
    }

    ActiveDynamicHaptic active;
    active.handle = nextHandle_;
    active.details = details;
    if (auto pushed = dynamics_.TryPushBack(active); !pushed) {
        return Result<DynamicHapticHandle>::Failure(pushed.Error());
    }

    ++nextHandle_;
    return Result<DynamicHapticHandle>::Success(active.handle);
}

Result<void> HapticPlayer::UpdateDynamicHaptic(DynamicHapticHandle handle,
                                               const DynamicHaptic& details) noexcept {
    if (!DynamicIsFinite(details)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (ActiveDynamicHaptic& active : dynamics_) {
        if (active.handle == handle) {
            // duration·경과 시간은 그대로다(UE :4449-4458 — Start일 때만 정한다).
            const float duration = active.details.duration;
            active.details = details;
            active.details.duration = duration;
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

void HapticPlayer::StopDynamicHaptic(DynamicHapticHandle handle) noexcept {
    for (usize index = 0; index < dynamics_.Size(); ++index) {
        if (dynamics_[index].handle == handle) {
            (void)dynamics_.TryRemoveAt(index);
            return;
        }
    }
}

void HapticPlayer::StopAll() noexcept {
    effects_.Clear();
    dynamics_.Clear();
}

MotorValues HapticPlayer::Update(const HapticUpdateContext& context) noexcept {
    MotorValues values;
    // 효과: pause 중에는 playWhilePaused만 갱신·기여한다(UE :4741-4754).
    for (usize index = effects_.Size(); index > 0; --index) {
        ActiveHapticEffect& active = effects_[index - 1];
        if (context.paused && !active.playWhilePaused) {
            continue;
        }

        const float duration = active.effect.Duration();
        active.playTime +=
            active.ignoreTimeDilation ? context.unscaledDeltaSeconds : context.deltaSeconds;
        const float playTime = static_cast<float>(active.playTime);
        // UE FActiveForceFeedbackEffect::Update(cpp:154-184): 길이를 넘었고 반복이 아니거나 길이가
        // 0이면 끝난다.
        if (playTime > duration && (!active.looping || duration == 0.0f)) {
            (void)effects_.TryRemoveAt(index - 1);
            continue;
        }

        if (playTime <= duration || active.looping) {
            // UE GetValues: PlayTime - Duration * Floor(PlayTime / Duration). 길이 0(재생 시각도
            // 0)이면 0/0을 피해 0이다.
            const float evalTime =
                duration > 0.0f ? playTime - duration * math::Floor(playTime / duration) : 0.0f;
            active.effect.AccumulateValues(evalTime, values);
        }
    }

    // 동적 진동: pause 중에는 처리하지 않는다(UE :4756-4770 — 경과도 멈춘다).
    if (!context.paused) {
        for (usize index = dynamics_.Size(); index > 0; --index) {
            ActiveDynamicHaptic& active = dynamics_[index - 1];
            active.elapsed += context.deltaSeconds;
            const double duration = active.details.duration;
            if (duration >= 0.0 && active.elapsed >= duration) {
                (void)dynamics_.TryRemoveAt(index - 1);
                continue;
            }

            const DynamicHaptic& details = active.details;
            AccumulateMotors(details.intensity, details.affectsLeftLarge, details.affectsLeftSmall,
                             details.affectsRightLarge, details.affectsRightSmall, values);
        }
    }

    // 끔·배율(UE bForceFeedbackEnabled·ForceFeedbackScale :4776-4780).
    if (!context.enabled) {
        return MotorValues{};
    }

    const float scale = math::IsFinite(context.scale) ? context.scale : 0.0f;
    values.leftLarge = HapticScaled(values.leftLarge, scale);
    values.leftSmall = HapticScaled(values.leftSmall, scale);
    values.rightLarge = HapticScaled(values.rightLarge, scale);
    values.rightSmall = HapticScaled(values.rightSmall, scale);
    return values;
}

} // namespace rocks::input
