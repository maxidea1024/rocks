#include "AudioDeviceContext.h"

#include <Core/Math/MathUtility.h>

// 차폐 목표값 입력이다(Docs/Design/AudioSystem 3.42.7절, UE FActiveSound::CheckOcclusion —
// ActiveSound.cpp:1577–1672). 질의는 게임 쪽 공급자가 하고, 제어 쪽은 간격마다 요청을 묶음에 싣고
// 응답으로 LPF·볼륨 목표를 정해 mix frame으로 보간한다.
namespace rocks {

namespace {

// UE au.EnableOcclusionFilterScale의 배수와 "20000과 거의 같음" 폭이다(Audio.cpp:576–582).
constexpr float OcclusionFilterScaleFactor = 0.25f;
constexpr float OcclusionFilterNearlyMax = 1.0e-4f;

} // namespace

void AudioDeviceContext::UpdateOcclusion(ControlBatch& batch, uint64 now) noexcept {
    const uint32 count = static_cast<uint32>(records_.Size());
    const uint32 start = count != 0 ? occlusionCursor_ % count : 0;
    for (uint32 step = 0; step < count; ++step) {
        const uint32 slot = (start + step) % count;
        Record& record = records_[slot];
        if (record.state == RecordState::Inactive || record.state == RecordState::Virtual ||
            !record.spatialized || record.occlusionPending) {
            continue;
        }

        const SoundAttenuation& attenuation = record.sound.Get()->attenuation.Value();
        if (!attenuation.enableOcclusion) {
            continue;
        }

        // audio.occlusion.disable이면 바로 차폐되지 않음으로 맞춘다(UE au.DisableOcclusion — 보간
        // 시간 0).
        if (occlusion_.disabled) {
            if (record.occlusionVolume.target != 1.0f ||
                record.occlusionFilter.target != MaxAttenuationFilterFrequency) {
                ApplyOcclusionResult(record, false, now, true);
            }

            continue;
        }

        // mute이거나 focus 볼륨 뒤가 0이면 질의만 건너뛴다(UE ActiveSound.cpp:2178 — 지금 값은
        // 둔다).
        if (occlusion_.muted || !(record.focus.volume > 0.0f)) {
            continue;
        }

        // 첫 결과가 없으면 바로, 아니면 간격을 넘었을 때(> — UE AS 1592–1593). 간격이 음수면 첫
        // 검사만이다.
        bool due = !record.occlusionChecked;
        // 재생 재정의(UE 컴포넌트 OcclusionCheckInterval — 3.43.1절)가 있으면 그것을 쓴다.
        const float intervalSeconds = record.hasOcclusionInterval ? record.occlusionInterval
                                                                  : occlusion_.checkIntervalSeconds;
        // 검사 간격은 record 시계다(일시정지 중 멈춤 — 3.44.3절).
        const uint64 at = RecordNow(record, now);
        if (!due && intervalSeconds >= 0.0f) {
            const uint64 interval = SecondsToMixFrames(static_cast<double>(intervalSeconds));
            due = at - record.occlusionCheckFrame > interval;
        }

        if (!due) {
            continue;
        }

        // 칸이 차면 다음 묶음은 이 slot부터 본다.
        if (batch.occlusionRequests.Size() >= batch.occlusionRequests.Capacity()) {
            occlusionCursor_ = slot;
            return;
        }

        OcclusionRequest request;
        request.handle = PlaybackHandle{slot, record.generation};
        request.sequence = ++record.occlusionSequence;
        request.scene = record.scene;
        request.sceneEpoch = record.sceneEpoch;
        request.source = record.position;
        request.listener = scenes_[record.scene].listenerPosition;
        request.owner = record.owner;
        (void)batch.occlusionRequests.TryPushBack(request);
        record.occlusionPending = true;
        record.occlusionCheckFrame = at;
        ++stats_.occlusionRequests;
    }

    occlusionCursor_ = 0;
}

void AudioDeviceContext::ApplySetOcclusion(const ControlCommand& command, uint64 now) noexcept {
    Record* record = FindRecord(command.handle);
    // 끝난·낡은 handle, 가상이 된 재생(대기를 지웠다), 옛 요청(순번이 다름)의 늦은 응답은 버린다.
    if (record == nullptr || record->state == RecordState::Virtual || !record->occlusionPending ||
        command.occlusionSequence != record->occlusionSequence) {
        return;
    }

    record->occlusionPending = false;
    ApplyOcclusionResult(*record, command.occluded, now, !record->occlusionChecked);
}

void AudioDeviceContext::ApplyOcclusionResult(Record& record, bool occluded, uint64 now,
                                              bool immediate) noexcept {
    const SoundAttenuation& attenuation = record.sound.Get()->attenuation.Value();
    // 첫 결과는 바로, 그 뒤는 occlusionInterpolationSeconds 동안 선형이다(UE AS 1644–1650).
    const uint32 frames =
        immediate
            ? 0u
            : SecondsToMixFrames(static_cast<double>(attenuation.occlusionInterpolationSeconds));
    float filterTarget = record.occlusionFilter.target;
    float volumeTarget = record.occlusionVolume.target;
    if (occluded) {
        // 목표는 더 낮은 쪽으로만 바뀐다(UE AS 1652–1667).
        filterTarget = math::Min(filterTarget, attenuation.occlusionLowPassFrequency);
        volumeTarget = math::Min(volumeTarget, attenuation.occlusionVolumeAttenuation);
    } else {
        filterTarget = MaxAttenuationFilterFrequency;
        volumeTarget = 1.0f;
    }

    // UE FDynamicParameter::Set은 목표나 길이가 바뀔 때만 다시 시작한다. 보간은 record 시계다
    // (일시정지 중 도착한 응답은 멈춘 시각에서 건다 — 3.44.3절).
    const uint64 at = RecordNow(record, now);
    if (filterTarget != record.occlusionFilter.target || frames == 0) {
        record.occlusionFilter.StartFrom(record.occlusionFilter.At(at), filterTarget, at, frames);
    }

    if (volumeTarget != record.occlusionVolume.target || frames == 0) {
        record.occlusionVolume.StartFrom(record.occlusionVolume.At(at), volumeTarget, at, frames);
    }

    record.occlusionChecked = true;
    record.paramsDirty = true;
}

float AudioDeviceContext::OcclusionFilterAt(const Record& record, uint64 frame) const noexcept {
    float frequency = record.occlusionFilter.At(frame);
    if (occlusion_.filterScale &&
        math::Abs(frequency - MaxAttenuationFilterFrequency) > OcclusionFilterNearlyMax) {
        frequency *= OcclusionFilterScaleFactor;
    }

    return frequency;
}

} // namespace rocks
