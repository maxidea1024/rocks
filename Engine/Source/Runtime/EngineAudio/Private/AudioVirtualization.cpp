#include "AudioDeviceContext.h"

#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>

// 가상화다(Docs/Design/AudioSystem 3.41.3절, UE FAudioVirtualLoop 대응). 가상 재생은
// voice·clip·스트림 슬롯 없이 논리 커서만 진행하고, 검사 간격(거리로 보간)마다 가청 거리를 본다.
// 들리면 동시 발음을 다시 평가해 voice 배정 후보로 돌아간다(Restart는 startFrame, SeekRestart는
// 논리 커서 — 3.41.1절 복귀).
namespace rocks {

bool AudioDeviceContext::IsVirtualizable(const Record& record) const noexcept {
    // UE FAudioVirtualLoop::Virtualize(AudioVirtualLoop.cpp:101–129): 모드가 Disabled가 아니고,
    // 켜져 있고, looping이고, 정지 중이 아니다.
    const SoundPlayable& playable = *record.sound.Get();
    return virtual_.enabled && playable.looping &&
           playable.virtualizationMode != SoundVirtualizationMode::Disabled &&
           !record.stopRequested;
}

uint64 AudioDeviceContext::VirtualCheckDelay(const Record& record) const noexcept {
    // UE AVL 195–205: lerp(min, max, clamp((리스너 거리 − 최대 거리) ÷ perfDistance, 0, 1)).
    const SoundPlayable& playable = *record.sound.Get();
    double beyond = 0.0;
    if (record.placement != PlayPlacement::NonSpatial && playable.attenuation.HasValue()) {
        const double distance = math::Sqrt(ListenerDistanceSquared(record));
        const double maxDistance = static_cast<double>(
            MaxAudibleDistance(playable.attenuation.Value(), virtual_.maxWorldDistance));
        beyond = distance - maxDistance;
    }

    const double perf = math::Max(static_cast<double>(virtual_.perfDistance), 1.0);
    const double t = math::Clamp(beyond / perf, 0.0, 1.0);
    const double seconds = static_cast<double>(virtual_.updateMinSeconds) +
                           (static_cast<double>(virtual_.updateMaxSeconds) -
                            static_cast<double>(virtual_.updateMinSeconds)) *
                               t;
    return SecondsToMixFrames(seconds);
}

void AudioDeviceContext::Virtualize(uint32 slot, ControlBatch& batch, uint64 now,
                                    uint32 stealFadeFrames) noexcept {
    Record& record = records_[slot];
    // voice가 있으면 떼어 정지 fade(steal이면 steal fade)로 보내고, 그룹에서 뺀다(실현 때 다시 평가
    // — UE CreateVirtualCopy).
    switch (record.state) {
    case RecordState::Submitted:
        DetachVoice(slot, batch, now, stealFadeFrames);
        break;
    case RecordState::Deferred:
        ReleaseDeferredVoice(slot, now);
        break;
    case RecordState::Voiceless:
    case RecordState::Virtual:
    case RecordState::Inactive:
        break;
    }

    LeaveConcurrencyGroups(slot, now);
    EnterVirtual(slot, now);
}

void AudioDeviceContext::EnterVirtual(uint32 slot, uint64 now) noexcept {
    Record& record = records_[slot];
    record.state = RecordState::Virtual;
    // 가상 사본은 소리 낸 시간을 0부터 센다(UE CreateVirtualCopy — PlaybackTimeNonVirtualized = 0,
    // AS 217).
    record.voicedTime = 0.0;
    record.virtualInterval = VirtualCheckDelay(record);
    record.virtualCheckFrame = now + record.virtualInterval;
    ++stats_.virtualizations;
}

void AudioDeviceContext::UpdateVirtualization(ControlBatch& batch, uint64 now) noexcept {
    for (uint32 slot = 0; slot < records_.Size(); ++slot) {
        Record& record = records_[slot];
        if (record.state == RecordState::Virtual) {
            // 가상화를 끄면 남은 가상 재생을 새 재생처럼 다시 수락한다(UE UpdateVirtualLoops의
            // disabled 분기 — AD 7480–7495, 실현 표지 없는 AddNewActiveSound). 거부되면 끝난다.
            if (!virtual_.enabled) {
                ConcurrencyOutcome outcome = ConcurrencyOutcome::Accepted;
                if (record.concurrencyCount != 0) {
                    outcome = EvaluateConcurrency(slot, batch, now, false);
                }

                if (outcome == ConcurrencyOutcome::Accepted) {
                    record.state = RecordState::Voiceless;
                    ++stats_.realizations;
                } else if (outcome == ConcurrencyOutcome::TableFull) {
                    ++stats_.failedPlays;
                    EndVoiceless(slot, PlaybackEndReason::Failed, batch);
                } else {
                    ++stats_.concurrencyRejects;
                    EndVoiceless(slot, PlaybackEndReason::Concurrency, batch);
                }

                continue;
            }

            // 검사 간격이 됐거나 리스너가 크게 움직였을 때 본다(UE AVL 298–352·451–456). 간격은
            // 여전히 들리지 않을 때만 다시 계산한다.
            if (now < record.virtualCheckFrame && !scenes_[record.scene].listenerJumped) {
                continue;
            }

            if (!IsAudible(record)) {
                record.virtualInterval = VirtualCheckDelay(record);
                record.virtualCheckFrame = now + record.virtualInterval;
                continue;
            }

            // 실현(UE RetriggerVirtualLoop — AD 5567): 동시 발음을 다시 평가하고, 실패하면 가상으로
            // 남아 같은 간격 뒤에 다시 본다(UE AD 5410–5432 — 표 가득도 가상 유지).
            ConcurrencyOutcome outcome = ConcurrencyOutcome::Accepted;
            if (record.concurrencyCount != 0) {
                outcome = EvaluateConcurrency(slot, batch, now, true);
            }

            if (outcome == ConcurrencyOutcome::Accepted) {
                record.state = RecordState::Voiceless;
                ++stats_.realizations;
            } else {
                if (outcome == ConcurrencyOutcome::TableFull) {
                    ++stats_.failedRealizations;
                }

                record.virtualCheckFrame = now + record.virtualInterval;
            }

            continue;
        }

        // 살아 있는 loop가 가청 거리 밖이면 가상으로 바꾼다(UE VirtualizeInactiveLoops — AD
        // 3250–3302).
        if ((record.state == RecordState::Voiceless || record.state == RecordState::Deferred ||
             record.state == RecordState::Submitted) &&
            IsVirtualizable(record) && !IsAudible(record)) {
            Virtualize(slot, batch, now);
        }
    }
}

} // namespace rocks
