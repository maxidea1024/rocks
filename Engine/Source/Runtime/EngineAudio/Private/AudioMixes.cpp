#include "AudioDeviceContext.h"

#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

// 믹스 스냅숏 상태·push·pop·base·override·passive다(Docs/Design/AudioSystem 3.43.6·3.43.7절, UE
// FSoundMixState·UpdateSoundClassProperties·PushSoundMixModifier·SetSoundMixClassOverride·
// UpdatePassiveSoundMixModifiers — AudioDevice.cpp:2692–3246·3735–4055). 시간은 mix clock이다.
namespace rocks {

namespace {

using platform::PlatformMath;

// UE InterpolateAdjuster(AudioDevice.h:1673–1676): 1에서 a까지 진폭 선형이다.
float InterpolateAdjuster(float adjuster, float t) noexcept {
    return adjuster * t + 1.0f - t;
}

// UE GetInterpolatedFrequency(AudioDevice.cpp:3015–3019): 20000에서 a까지 로그 주파수로 간다.
float InterpolateFrequency(float frequency, float t) noexcept {
    const double clamped = static_cast<double>(
        math::Clamp(frequency, MinAttenuationFilterFrequency, MaxAttenuationFilterFrequency));
    const double result = PlatformMath::Pow(static_cast<double>(MaxAttenuationFilterFrequency),
                                            1.0 - static_cast<double>(t)) *
                          PlatformMath::Pow(clamped, static_cast<double>(t));
    return static_cast<float>(result);
}

} // namespace

uint64 AudioDeviceContext::MixFrames(float seconds) const noexcept {
    // 초는 MaxMixSnapshotSeconds 이하로 검사돼 있다(ValidateMixSnapshotData) — uint64로 넘치지
    // 않는다.
    const double frames =
        static_cast<double>(seconds) * static_cast<double>(mixer_.Config().sampleRate);
    return frames <= 0.0 ? 0u : static_cast<uint64>(frames + 0.5);
}

AudioDeviceContext::MixState* AudioDeviceContext::FindMix(uint64 key) noexcept {
    for (MixState& state : mixes_) {
        if (state.ref->key == key) {
            return &state;
        }
    }

    return nullptr;
}

void AudioDeviceContext::StartMix(MixState& state, uint64 now) noexcept {
    // UE ApplySoundMix(AD 2692–2719).
    const MixSnapshotData& data = *state.ref.Get();
    state.fadeInStart = now + MixFrames(data.initialDelay);
    state.fadeInEnd = state.fadeInStart + MixFrames(data.fadeInTime);
    state.hasFadeOut = data.duration >= 0.0f;
    if (state.hasFadeOut) {
        state.fadeOutStart = state.fadeInEnd + MixFrames(data.duration);
        state.end = state.fadeOutStart + MixFrames(data.fadeOutTime);
    }

    state.phase = MixPhase::Inactive;
    state.t = 0.0f;
}

void AudioDeviceContext::RetriggerMix(MixState& state, uint64 now) noexcept {
    // UE UpdateSoundMix(AD 2721–2773). fade-out 중 다시 걸기는 지금 레벨에서 이어 간다(Rocks —
    // UE는 레벨이 튄다, 7절).
    const MixSnapshotData& data = *state.ref.Get();
    switch (state.phase) {
    case MixPhase::Inactive:
        // 지연 중이면 아무 일도 없다(UE는 StartTime만 바꾸고 FadeInStartTime은 그대로 — AD
        // 2724–2729).
        return;
    case MixPhase::FadingIn:
    case MixPhase::Active:
        if (data.duration >= 0.0f) {
            state.fadeOutStart = now + MixFrames(data.fadeInTime) + MixFrames(data.duration);
            state.end = state.fadeOutStart + MixFrames(data.fadeOutTime);
            state.hasFadeOut = true;
        } else {
            // 무한 믹스는 잡힌 fade-out을 거둔다(UE FOS = End = −1).
            state.hasFadeOut = false;
        }

        return;
    case MixPhase::FadingOut:
    case MixPhase::AwaitingRemoval: {
        const uint64 back = static_cast<uint64>(
            static_cast<double>(state.t) * static_cast<double>(MixFrames(data.fadeInTime)) + 0.5);
        state.fadeInStart = now > back ? now - back : 0;
        state.fadeInEnd = state.fadeInStart + MixFrames(data.fadeInTime);
        state.hasFadeOut = data.duration >= 0.0f;
        if (state.hasFadeOut) {
            state.fadeOutStart = state.fadeInEnd + MixFrames(data.duration);
            state.end = state.fadeOutStart + MixFrames(data.fadeOutTime);
        }

        state.phase = MixPhase::FadingIn;
        return;
    }
    }
}

void AudioDeviceContext::PushMixState(const MixSnapshotRef& ref, bool passive, bool retrigger,
                                      uint64 now) noexcept {
    MixState* state = FindMix(ref->key);
    if (state == nullptr) {
        if (mixes_.Size() >= mixes_.Capacity()) {
            ++stats_.mixRejects;
            return;
        }

        MixState created;
        created.ref = ref;
        StartMix(created, now);
        (void)mixes_.TryPushBack(Move(created));
        state = &mixes_[mixes_.Size() - 1];
    } else {
        // 같은 키의 새 값(에셋을 고친 뒤의 push·passive 사본)으로 바꾼다(3.43.7절). 이번 묶음에
        // 지우려던 믹스면 되살린다.
        state->ref = ref;
        state->remove = false;
        RetriggerMix(*state, now);
    }

    if (!retrigger) {
        ++(passive ? state->passiveRefs : state->activeRefs);
    }
}

void AudioDeviceContext::TryClearMix(MixState& state, uint64 now) noexcept {
    // UE TryClearingSoundMix(AD 2832–2879).
    const MixSnapshotData& data = *state.ref.Get();
    if (data.duration >= 0.0f || state.activeRefs != 0 || state.passiveRefs != 0 || state.base) {
        return;
    }

    if (data.fadeOutTime <= 0.0f || state.phase == MixPhase::Inactive) {
        state.remove = true;
        return;
    }

    const uint64 fadeOut = MixFrames(data.fadeOutTime);
    if (state.phase == MixPhase::FadingIn) {
        // 지금 레벨에서 이어 내려간다. fade-in은 여기서 끝낸다(UE FIS = FIE = FOS − 1 — AD
        // 2853–2856).
        const uint64 back = static_cast<uint64>(
            (1.0 - static_cast<double>(state.t)) * static_cast<double>(fadeOut) + 0.5);
        state.fadeOutStart = now > back ? now - back : 0;
        state.fadeInStart = state.fadeOutStart;
        state.fadeInEnd = state.fadeOutStart;
    } else if (state.phase == MixPhase::Active) {
        state.fadeOutStart = now;
    } else {
        return;
    }

    state.end = state.fadeOutStart + fadeOut;
    state.hasFadeOut = true;
    // 같은 묶음의 다시 걸기가 연속 경로를 타도록 상태를 바로 바꾼다(t는 그대로).
    state.phase = MixPhase::FadingOut;
}

void AudioDeviceContext::PopMixState(uint64 key, bool passive, uint64 now) noexcept {
    MixState* state = FindMix(key);
    if (state == nullptr) {
        return;
    }

    uint32& refs = passive ? state->passiveRefs : state->activeRefs;
    if (refs == 0) {
        return;
    }

    --refs;
    TryClearMix(*state, now);
}

void AudioDeviceContext::ApplyBaseMix(const MixSnapshotRef& mix, uint64 now) noexcept {
    // UE SetBaseSoundMix(AD 3735–3783): 같은 믹스면 아무 일도 없다.
    MixState* current = nullptr;
    for (MixState& state : mixes_) {
        if (state.base) {
            current = &state;
        }
    }

    if (current != nullptr && current->ref->key == mix->key) {
        current->ref = mix;
        return;
    }

    // 새 base를 둘 칸이 없으면 이전 base도 건드리지 않는다.
    MixState* state = FindMix(mix->key);
    if (state == nullptr && mixes_.Size() >= mixes_.Capacity()) {
        ++stats_.mixRejects;
        return;
    }

    if (current != nullptr) {
        current->base = false;
        TryClearMix(*current, now);
    }

    if (state == nullptr) {
        PushMixState(mix, false, true, now);
        state = FindMix(mix->key);
    } else {
        state->ref = mix;
        state->remove = false;
        if (state->phase == MixPhase::FadingOut || state->phase == MixPhase::AwaitingRemoval) {
            RetriggerMix(*state, now);
        }
    }

    if (state != nullptr) {
        state->base = true;
    }
}

bool AudioDeviceContext::IsMixCommand(ControlCommandType type) noexcept {
    switch (type) {
    case ControlCommandType::PushMix:
    case ControlCommandType::PopMix:
    case ControlCommandType::ClearMix:
    case ControlCommandType::ClearAllMixes:
    case ControlCommandType::SetBaseMix:
    case ControlCommandType::SetMixCategoryOverride:
    case ControlCommandType::ClearMixCategoryOverride:
    case ControlCommandType::SetCategoryDistanceScale:
        return true;
    case ControlCommandType::Play:
    case ControlCommandType::Stop:
    case ControlCommandType::SetParameter:
    case ControlCommandType::RemoveScene:
    case ControlCommandType::RemoveEmitter:
    case ControlCommandType::SetOcclusion:
    case ControlCommandType::SetBusSend:
        return false;
    }

    return false;
}

void AudioDeviceContext::ApplyMixCommand(const ControlCommand& command, uint64 now) noexcept {
    switch (command.type) {
    case ControlCommandType::PushMix:
        PushMixState(command.mix, false, false, now);
        break;
    case ControlCommandType::PopMix:
        PopMixState(command.mix->key, false, now);
        break;
    case ControlCommandType::ClearMix:
        if (MixState* state = FindMix(command.mix->key)) {
            state->activeRefs = 0;
            TryClearMix(*state, now);
        }

        break;
    case ControlCommandType::ClearAllMixes:
        for (MixState& state : mixes_) {
            state.activeRefs = 0;
            TryClearMix(state, now);
        }

        break;
    case ControlCommandType::SetBaseMix:
        ApplyBaseMix(command.mix, now);
        break;
    case ControlCommandType::SetMixCategoryOverride:
        ApplySetMixOverride(command, now);
        break;
    case ControlCommandType::ClearMixCategoryOverride:
        ApplyClearMixOverride(command, now);
        break;
    case ControlCommandType::SetCategoryDistanceScale:
        ApplySetCategoryDistanceScale(command, now);
        break;
    case ControlCommandType::Play:
    case ControlCommandType::Stop:
    case ControlCommandType::SetParameter:
    case ControlCommandType::RemoveScene:
    case ControlCommandType::RemoveEmitter:
    case ControlCommandType::SetOcclusion:
    case ControlCommandType::SetBusSend:
        break;
    }
}

AudioDeviceContext::MixOverride* AudioDeviceContext::FindOverride(uint64 mixKey,
                                                                  uint64 categoryKey) noexcept {
    for (MixOverride& entry : overrides_) {
        if (entry.mixKey == mixKey && entry.categoryKey == categoryKey) {
            return &entry;
        }
    }

    return nullptr;
}

float AudioDeviceContext::AdjusterValueOf(uint64 mixKey, uint64 categoryKey, bool pitch,
                                          bool interpolated) noexcept {
    // 믹스의 그 분류 첫 조정(없으면 1)이다. interpolated면 지금 t로 보간한 값, 아니면 원래 값이다.
    MixState* state = FindMix(mixKey);
    if (state == nullptr) {
        return 1.0f;
    }

    const MixSnapshotData& data = *state->ref.Get();
    for (uint32 index = 0; index < data.adjusterCount; ++index) {
        const MixAdjusterValue& adjuster = data.adjusters[index];
        if (adjuster.categoryKey == categoryKey) {
            const float value = pitch ? adjuster.pitch : adjuster.volume;
            return interpolated ? InterpolateAdjuster(value, state->t) : value;
        }
    }

    return 1.0f;
}

void AudioDeviceContext::ApplySetMixOverride(const ControlCommand& command, uint64 now) noexcept {
    // UE SetSoundMixClassOverride(AD 3839–3964·2955–3013).
    const uint64 mixKey = command.mix->key;
    const uint32 frames = SecondsToMixFrames(static_cast<double>(command.seconds));
    MixOverride* entry = FindOverride(mixKey, command.categoryKey);
    if (entry == nullptr) {
        if (overrides_.Size() >= overrides_.Capacity()) {
            ++stats_.mixOverrideRejects;
            return;
        }

        MixOverride created;
        created.mixKey = mixKey;
        created.categoryKey = command.categoryKey;
        (void)overrides_.TryPushBack(created);
        entry = &overrides_[overrides_.Size() - 1];
    }

    entry->targetVolume = command.value;
    entry->targetPitch = command.overridePitch;
    entry->applyToChildren = command.applyToChildren;
    entry->fadeFrames = frames;
    // 적용 중이면 지금 값에서 새 목표로 다시 간다(Clear 진행 중·같은 묶음의 Clear는 취소 — AD
    // 2962–2966·3866).
    if (entry->applied) {
        entry->volume.StartFrom(entry->volume.At(now), command.value, now, frames);
        entry->pitch.StartFrom(entry->pitch.At(now), command.overridePitch, now, frames);
    }

    entry->clearing = false;
    entry->remove = false;
}

void AudioDeviceContext::ApplyClearMixOverride(const ControlCommand& command, uint64 now) noexcept {
    // UE ClearSoundMixClassOverride: 적용 전이면 바로, 적용 중이면 원래 조정값으로 돌아간 뒤
    // 지운다.
    MixOverride* entry = FindOverride(command.mix->key, command.categoryKey);
    if (entry == nullptr) {
        return;
    }

    if (!entry->applied) {
        entry->remove = true;
        return;
    }

    const uint32 frames = SecondsToMixFrames(static_cast<double>(command.seconds));
    entry->volume.StartFrom(entry->volume.At(now),
                            AdjusterValueOf(entry->mixKey, entry->categoryKey, false, false), now,
                            frames);
    entry->pitch.StartFrom(entry->pitch.At(now),
                           AdjusterValueOf(entry->mixKey, entry->categoryKey, true, false), now,
                           frames);
    entry->clearing = true;
}

void AudioDeviceContext::UpdateMixes(uint64 now) noexcept {
    mixesChanging_ = false;
    // 1) 상태와 t(UE UpdateSoundClassProperties — AD 3181–3246).
    for (MixState& state : mixes_) {
        if (state.remove) {
            continue;
        }

        const uint32 refs = state.activeRefs + state.passiveRefs;
        if (now < state.fadeInStart) {
            state.phase = MixPhase::Inactive;
            state.t = 0.0f;
        } else if (now < state.fadeInEnd) {
            state.phase = MixPhase::FadingIn;
            state.t = static_cast<float>(static_cast<double>(now - state.fadeInStart) /
                                         static_cast<double>(state.fadeInEnd - state.fadeInStart));
        } else if (state.base || (refs > 0 && !state.hasFadeOut) ||
                   (state.hasFadeOut && now < state.fadeOutStart)) {
            state.phase = MixPhase::Active;
            state.t = 1.0f;
        } else if (state.hasFadeOut && now < state.end) {
            state.phase = MixPhase::FadingOut;
            state.t =
                1.0f - static_cast<float>(static_cast<double>(now - state.fadeOutStart) /
                                          static_cast<double>(state.end - state.fadeOutStart));
        } else {
            state.phase = MixPhase::AwaitingRemoval;
            state.t = 0.0f;
            // passive 참조가 없으면 지운다(active 참조가 남아도 — UE AD 3236–3245).
            if (state.passiveRefs == 0) {
                state.remove = true;
            }
        }

        mixesChanging_ = mixesChanging_ || state.phase == MixPhase::FadingIn ||
                         state.phase == MixPhase::FadingOut;
    }

    for (usize index = mixes_.Size(); index != 0; --index) {
        if (mixes_[index - 1].remove) {
            (void)mixes_.TryRemoveAt(index - 1, 1);
        }
    }

    // 2) override: 믹스가 있으면 적용(처음이면 지금 보간 값에서 fade-in), 없으면 적용을 푼다. Clear
    // 중에 믹스가 사라지면 해제가 끝난 것으로 지운다. Clear가 끝나면 지운다(UE는 Volume 파라미터
    // 완료만 본다).
    for (MixOverride& entry : overrides_) {
        if (FindMix(entry.mixKey) == nullptr) {
            entry.remove = entry.remove || entry.clearing;
            entry.applied = false;
            entry.clearing = false;
            continue;
        }

        if (!entry.applied && !entry.remove) {
            entry.volume.StartFrom(AdjusterValueOf(entry.mixKey, entry.categoryKey, false, true),
                                   entry.targetVolume, now, entry.fadeFrames);
            entry.pitch.StartFrom(AdjusterValueOf(entry.mixKey, entry.categoryKey, true, true),
                                  entry.targetPitch, now, entry.fadeFrames);
            entry.applied = true;
        }

        if (entry.clearing && !entry.volume.IsRamping(now)) {
            entry.remove = true;
        }

        mixesChanging_ =
            mixesChanging_ || entry.volume.IsRamping(now) || entry.pitch.IsRamping(now);
    }

    for (usize index = overrides_.Size(); index != 0; --index) {
        if (overrides_[index - 1].remove) {
            (void)overrides_.TryRemoveAt(index - 1, 1);
        }
    }

    // 3) 이번 묶음 조정 목록(분류 실효값이 쓴다).
    adjustments_.Clear();
    categoryCache_.Clear();
    for (const MixState& state : mixes_) {
        const MixSnapshotData& data = *state.ref.Get();
        for (uint32 index = 0; index < data.adjusterCount; ++index) {
            const MixAdjusterValue& adjuster = data.adjusters[index];
            MixAdjustment adjustment;
            adjustment.categoryKey = adjuster.categoryKey;
            adjustment.lowPassFrequency = InterpolateFrequency(adjuster.lowPassFrequency, state.t);
            // override는 그 분류의 첫 조정만 덮는다(UE는 첫 항목에서 멈춘다).
            bool first = true;
            for (uint32 earlier = 0; earlier < index; ++earlier) {
                first = first && data.adjusters[earlier].categoryKey != adjuster.categoryKey;
            }

            const MixOverride* entry =
                first ? FindOverride(data.key, adjuster.categoryKey) : nullptr;
            if (entry != nullptr && entry->applied) {
                // override 값은 믹스 t를 받지 않는다(UE 그대로).
                adjustment.volume = entry->volume.At(now);
                adjustment.pitch = entry->pitch.At(now);
                adjustment.applyToChildren = entry->applyToChildren;
            } else {
                adjustment.volume = InterpolateAdjuster(adjuster.volume, state.t);
                adjustment.pitch = InterpolateAdjuster(adjuster.pitch, state.t);
                adjustment.applyToChildren = adjuster.applyToChildren;
            }

            (void)adjustments_.TryPushBack(adjustment);
        }

        // 조정 항목이 없는 분류의 override는 기본 1 항목처럼 들어간다.
        for (const MixOverride& entry : overrides_) {
            if (entry.mixKey != data.key || !entry.applied) {
                continue;
            }

            bool listed = false;
            for (uint32 index = 0; index < data.adjusterCount; ++index) {
                listed = listed || data.adjusters[index].categoryKey == entry.categoryKey;
            }

            if (!listed) {
                MixAdjustment adjustment;
                adjustment.categoryKey = entry.categoryKey;
                adjustment.volume = entry.volume.At(now);
                adjustment.pitch = entry.pitch.At(now);
                adjustment.applyToChildren = entry.applyToChildren;
                (void)adjustments_.TryPushBack(adjustment);
            }
        }
    }

    stats_.activeMixes = static_cast<uint32>(mixes_.Size());
}

void AudioDeviceContext::UpdatePassiveMixes(uint64 now) noexcept {
    // UE UpdatePassiveSoundMixModifiers(AD 2775–2830): 배정 자리 안의 재생(정지 요청·steal fade
    // 중은 뺀다), 잎 분류만, 경계 포함.
    for (PassiveMix& passive : passive_) {
        passive.seen = false;
    }

    for (Record& record : records_) {
        if ((record.state != RecordState::Submitted && record.state != RecordState::Deferred) ||
            record.stopRequested) {
            continue;
        }

        // 수락 뒤 처음 배정된 재생이다(UE PlaybackTime == 0 — 가상에서 실현된 재생은 아니다).
        const bool firstAssignment = !record.everAssigned && record.playbackTime <= 0.0;
        record.everAssigned = true;
        const SoundCategoryChain* chain = record.category.Get();
        if (chain == nullptr || chain->depth == 0) {
            continue;
        }

        const SoundCategoryLink& leaf = chain->links[0];
        for (uint32 index = 0; index < leaf.passiveCount; ++index) {
            const SoundCategoryPassiveMixValue& entry = leaf.passive[index];
            if (record.estimate < entry.minVolumeThreshold ||
                record.estimate > entry.maxVolumeThreshold) {
                continue;
            }

            PassiveMix* found = nullptr;
            for (PassiveMix& passive : passive_) {
                if (passive.mixKey == entry.mix->key) {
                    found = &passive;
                }
            }

            if (found == nullptr) {
                // 믹스가 실제로 걸렸을 때만 passive 집합에 넣는다(표가 차면 다음 묶음에 다시 본다).
                if (passive_.Size() >= passive_.Capacity()) {
                    ++stats_.mixRejects;
                    continue;
                }

                PushMixState(entry.mix, true, false, now);
                if (FindMix(entry.mix->key) == nullptr) {
                    continue;
                }

                PassiveMix added;
                added.mixKey = entry.mix->key;
                added.seen = true;
                (void)passive_.TryPushBack(added);
            } else {
                found->seen = true;
            }

            // 새 재생이면 다시 걸기(duration 연장 — UE PlaybackTime == 0).
            if (firstAssignment) {
                PushMixState(entry.mix, true, true, now);
            }
        }
    }

    for (usize index = passive_.Size(); index != 0; --index) {
        if (!passive_[index - 1].seen) {
            PopMixState(passive_[index - 1].mixKey, true, now);
            (void)passive_.TryRemoveAt(index - 1, 1);
        }
    }
}

} // namespace rocks
