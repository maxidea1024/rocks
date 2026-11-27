#include "AudioDeviceContext.h"

// 장면 일시정지다(Docs/Design/AudioSystem 3.44절, UE FAudioDevice::HandlePause 대응). scene이
// 일시정지 되는 묶음에 그 scene의 UI 아닌 재생을 멈추고(record 시계 동결 + voice paused), 풀리는
// 묶음에 멈춘 길이만큼 record 시계 항목을 밀어 이어 간다. 판정은 전환 때만 한다(정지 요청이
// 일시정지를 풀지 않게).
namespace rocks {

void AudioDeviceContext::UpdatePause(ControlBatch& batch, uint64 now) noexcept {
    bool changed = false;
    for (const ControlScene& scene : scenes_) {
        changed = changed || scene.pauseChanged;
    }

    if (!changed) {
        return;
    }

    for (uint32 slot = 0; slot < records_.Size(); ++slot) {
        Record& record = records_[slot];
        if (record.state == RecordState::Inactive) {
            continue;
        }

        const ControlScene& scene = scenes_[record.scene];
        if (!scene.pauseChanged || scene.epoch != record.sceneEpoch) {
            continue;
        }

        if (scene.paused && !record.paused && !record.isUiSound) {
            PauseRecord(slot, batch, now);
        } else if (!scene.paused && record.paused) {
            ResumeRecord(record, now);
        }
    }

    // 분리 voice도 그 scene을 따른다. steal fade 중인 voice는 묶음마다 게시하므로 값만 바꾼다.
    for (DetachedVoice& detached : detached_) {
        const ControlScene& scene = scenes_[detached.scene];
        if (!scene.pauseChanged || scene.epoch != detached.sceneEpoch) {
            continue;
        }

        const bool paused = scene.paused && !detached.uiSound;
        if (paused == detached.params.paused) {
            continue;
        }

        detached.params.paused = paused;
        if (!detached.fading) {
            (void)mixer_.PublishParams(detached.voice, detached.params);
        }
    }
}

void AudioDeviceContext::PauseRecord(uint32 slot, ControlBatch& batch, uint64 now) noexcept {
    Record& record = records_[slot];
    record.paused = true;
    record.pauseStartMixFrame = now;
    record.paramsDirty = true;
    if (record.state == RecordState::Submitted) {
        record.voicePaused = true;
    }

    // 보내지 못한 Play는 놓는다. 자리는 voice 없는 재생으로 그대로 차지한다(UE는 시작 전 source를
    // 시작하지 않는다 — 3.44.4절). 보내기 전에 정지 요청을 받았으면 Started 없이 끝낸다.
    if (record.state == RecordState::Deferred) {
        const bool stopping = record.stopRequested;
        const PlaybackEndReason reason = record.stopReason;
        ReleaseDeferredVoice(slot, now);
        if (stopping) {
            EndVoiceless(slot, reason, batch);
        }
    }
}

void AudioDeviceContext::ResumeRecord(Record& record, uint64 now) noexcept {
    // record 시계 항목을 멈춘 길이만큼 민다(3.44.3절). voice가 시작한 재생의 커서는 mix 시계와
    // 믹서의 읽지 않은 누적으로 적분하므로 밀지 않는다.
    const uint64 shift = now - record.pauseStartMixFrame;
    record.volume.startFrame += shift;
    record.pitch.startFrame += shift;
    record.resume.startFrame += shift;
    record.occlusionVolume.startFrame += shift;
    record.occlusionFilter.startFrame += shift;
    record.focusMixFrame += shift;
    record.occlusionCheckFrame += shift;
    record.playbackMixFrame += shift;
    if (!(record.state == RecordState::Submitted && record.started)) {
        record.cursorMixFrame += shift;
    }

    record.paused = false;
    record.paramsDirty = true;
}

} // namespace rocks
