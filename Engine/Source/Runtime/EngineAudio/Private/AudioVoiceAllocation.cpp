#include "AudioDeviceContext.h"

#include <Core/Algorithms.h>
#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>

// voice 배정이다(Docs/Design/AudioSystem 3.41.1절, UE FAudioDevice::GetSortedActiveWaveInstances
// 대응). 묶음마다 살아 있는 재생의 들리는 크기 v와 볼륨 가중 우선순위 W를 계산하고, 무음 컬링 뒤
// W 내림차순(동률은 먼저 수락한 쪽)으로 audio.voices.max까지 voice를 준다.
namespace rocks {

namespace {

void RemoveSlot(Array<uint32>& values, uint32 value) noexcept {
    for (usize index = 0; index < values.Size(); ++index) {
        if (values[index] == value) {
            (void)values.TryRemoveAt(index);
            return;
        }
    }
}

// loop 구간 [begin, end)다(둘 다 0이면 clip 전체).
void EffectiveLoop(const SoundPlayable& playable, uint32& begin, uint32& end) noexcept {
    const bool wholeClip = playable.loopBegin == 0 && playable.loopEnd == 0;
    begin = wholeClip ? 0 : playable.loopBegin;
    end = wholeClip ? playable.FrameCount() : playable.loopEnd;
}

// voice를 잃어도 재생이 남는다(loop, 또는 remainActive one-shot — 3.41.1절).
bool KeepsWithoutVoice(const SoundPlayable& playable, bool remainActive) noexcept {
    return playable.looping || remainActive;
}

} // namespace

bool AudioDeviceContext::IsAudible(const Record& record) const noexcept {
    const SoundPlayable& playable = *record.sound.Get();
    // NonSpatial 배치·감쇠 없음은 늘 들린다(UE bAllowSpatialization 아님 — 3.41.3절).
    if (record.placement == PlayPlacement::NonSpatial || !playable.attenuation.HasValue()) {
        return true;
    }

    const ControlScene& scene = scenes_[record.scene];
    return IsWithinAudibleRange(playable.attenuation.Value(), scene.listenerPosition,
                                record.position);
}

void AudioDeviceContext::UpdateEstimates(ControlBatch& batch, uint64 now) noexcept {
    const double mixRate = static_cast<double>(mixer_.Config().sampleRate);
    for (uint32 slot = 0; slot < records_.Size(); ++slot) {
        Record& record = records_[slot];
        if (record.state == RecordState::Inactive) {
            continue;
        }

        const SoundPlayable& playable = *record.sound.Get();
        // 논리 커서: 실효 pitch × rate 비로 진행하고 loop 구간에서 감는다(UE PlaybackTime).
        if (!record.cursorRunning) {
            record.cursorMixFrame = now;
        } else if (now > record.cursorMixFrame) {
            const double pitch =
                static_cast<double>(ClampVoicePitch(record.pitch.At(now) * playable.pitch));
            const double elapsed = static_cast<double>(now - record.cursorMixFrame);
            record.cursor += elapsed * pitch * static_cast<double>(playable.SampleRate()) / mixRate;
            record.cursorMixFrame = now;
            if (playable.looping) {
                uint32 loopBegin = 0;
                uint32 loopEnd = 0;
                EffectiveLoop(playable, loopBegin, loopEnd);
                if (record.cursor >= static_cast<double>(loopEnd)) {
                    const double length = static_cast<double>(loopEnd - loopBegin);
                    record.cursor =
                        static_cast<double>(loopBegin) +
                        math::Fmod(record.cursor - static_cast<double>(loopEnd), length);
                }
            }
        }

        // voice 없이 남은 one-shot(remainActive)이 논리 커서로 끝까지 갔다(시작한 적이 없으면 끝
        // frame이 없다 — 3.41.1절).
        if (!playable.looping && record.state == RecordState::Voiceless &&
            record.cursor >= static_cast<double>(playable.FrameCount())) {
            EndVoiceless(slot, PlaybackEndReason::NaturalEnd, batch);
            continue;
        }

        record.estimate = record.volume.At(now) * playable.volume * record.spatial.attenuationGain;
        // UE FWaveInstance::GetVolumeWeightedPriority(Audio.cpp:1328–1359).
        if (record.estimate > 0.0f) {
            record.weight = playable.bypassVolumeScaleForPriority
                                ? record.priority
                                : record.estimate * record.priority;
        } else {
            record.weight = record.priority - InaudibleWeightOffset;
        }
    }
}

void AudioDeviceContext::AssignVoices(ControlBatch& batch, uint64 now) noexcept {
    candidates_.Clear();
    for (uint32 slot = 0; slot < records_.Size(); ++slot) {
        Record& record = records_[slot];
        // 정지·steal fade 중인 재생은 경쟁하지 않고 voice를 끝까지 쥔다.
        if (record.state == RecordState::Inactive || record.stopRequested) {
            continue;
        }

        const SoundPlayable& playable = *record.sound.Get();
        const bool exempt = record.volume.IsRising(now) ||
                            playable.virtualizationMode == SoundVirtualizationMode::PlayWhenSilent;
        if (record.estimate <= SilentVolumeThreshold && !exempt) {
            HandleSilent(slot, batch, now);
            continue;
        }

        ROCKS_ASSERT(candidates_.Size() < candidates_.Capacity());
        (void)candidates_.TryPushBack(VoiceCandidate{slot, record.weight, record.sequence});
    }

    algo::Sort(Span<VoiceCandidate>(candidates_.Data(), candidates_.Size()),
               [](const VoiceCandidate& left, const VoiceCandidate& right) noexcept {
                   if (left.weight != right.weight) {
                       return left.weight > right.weight;
                   }

                   return left.sequence < right.sequence;
               });

    // 상한 밖을 먼저 처리한다(Deferred voice를 바로 놓아 자리 안 재생의 voice 여유를 늘린다).
    for (usize rank = maxActiveVoices_; rank < candidates_.Size(); ++rank) {
        DropVoice(candidates_[rank].slot, batch, now);
    }

    const usize inside =
        candidates_.Size() < maxActiveVoices_ ? candidates_.Size() : maxActiveVoices_;
    for (usize rank = 0; rank < inside; ++rank) {
        const uint32 slot = candidates_[rank].slot;
        // 자리를 받았는데 voice가 없으면 시작한다. 실패한 자리를 다음 순위가 받지 않는다(UE).
        if (records_[slot].state == RecordState::Voiceless) {
            const StartOutcome outcome = StartVoice(slot, now);
            if (outcome != StartOutcome::Started) {
                OnStartFailure(slot, outcome, batch);
            }
        }
    }
}

void AudioDeviceContext::HandleSilent(uint32 slot, ControlBatch& batch, uint64 now) noexcept {
    Record& record = records_[slot];
    const bool keep = KeepsWithoutVoice(*record.sound.Get(), record.remainActive);
    switch (record.state) {
    case RecordState::Voiceless:
        if (!keep) {
            ++stats_.inaudibleEnds;
            EndVoiceless(slot, PlaybackEndReason::Inaudible, batch);
        }

        break;
    case RecordState::Deferred:
        ReleaseDeferredVoice(slot, now);
        if (!keep) {
            ++stats_.inaudibleEnds;
            EndVoiceless(slot, PlaybackEndReason::Inaudible, batch);
        }

        break;
    case RecordState::Submitted:
        if (keep) {
            DetachVoice(slot, batch, now);
        } else {
            ++stats_.inaudibleEnds;
            RequestStop(record, audio::DefaultStopFadeFrames, PlaybackEndReason::Inaudible);
        }

        break;
    case RecordState::Inactive:
        break;
    }
}

void AudioDeviceContext::DropVoice(uint32 slot, ControlBatch& batch, uint64 now) noexcept {
    Record& record = records_[slot];
    const bool keep = KeepsWithoutVoice(*record.sound.Get(), record.remainActive);
    switch (record.state) {
    case RecordState::Voiceless:
        // voice 없는 one-shot은 이번 묶음에 수락한 새 재생뿐이다(시작하지 못함).
        if (!keep) {
            ++stats_.voiceLimitRejects;
            EndVoiceless(slot, PlaybackEndReason::VoiceLimit, batch);
        }

        break;
    case RecordState::Deferred:
        // 아직 보내지 못한 Play다: 준비를 버리고 voice를 바로 놓는다(Started는 나가지 않는다).
        ReleaseDeferredVoice(slot, now);
        if (!keep) {
            ++stats_.voiceLimitRejects;
            EndVoiceless(slot, PlaybackEndReason::VoiceLimit, batch);
        }

        break;
    case RecordState::Submitted:
        // UE FMixerSource::Stop(au.SourceFadeMin 512 frame)으로 빼앗는다.
        ++stats_.voiceSteals;
        if (keep) {
            DetachVoice(slot, batch, now);
        } else {
            RequestStop(record, audio::DefaultStopFadeFrames, PlaybackEndReason::VoiceLimit);
        }

        break;
    case RecordState::Inactive:
        break;
    }
}

AudioDeviceContext::StartOutcome AudioDeviceContext::StartVoice(uint32 slot, uint64 now) noexcept {
    Record& record = records_[slot];
    const SoundPlayable& playable = *record.sound.Get();
    // 시작 위치: SeekRestart면 논리 커서(되찾을 때 fade-in), 그 밖은 요청한 시작 위치(UE
    // RequestedStartTime — 3.41.1절).
    uint32 start = record.startFrame;
    bool fadeIn = false;
    if (playable.virtualizationMode == SoundVirtualizationMode::SeekRestart) {
        uint32 cursor = static_cast<uint32>(math::Max(record.cursor, 0.0));
        uint32 loopBegin = 0;
        uint32 loopEnd = playable.FrameCount();
        if (playable.looping) {
            EffectiveLoop(playable, loopBegin, loopEnd);
        }

        cursor = cursor < loopEnd ? cursor : loopEnd - 1;
        if (record.everStarted || cursor != record.startFrame) {
            start = cursor;
            fadeIn = true;
        }
    }

    auto voice = mixer_.AcquireVoice();
    if (!voice) {
        return StartOutcome::NoRoom;
    }

    bool slotRejected = false;
    auto clip = AcquireClip(playable, start, slotRejected);
    if (!clip) {
        // Reserved voice는 바로 놓을 수 있다.
        (void)mixer_.ReleaseVoice(voice.Value());
        return slotRejected ? StartOutcome::NoStreamSlot : StartOutcome::Failed;
    }

    record.voice = voice.Value();
    record.clip = clip.Value();
    // 복귀 fade-in은 시작 frame을 본 뒤에 시작한다(시작 대기·Deferred 동안 gain 0 — 3.41.1절).
    record.resume.Set(fadeIn ? 0.0f : 1.0f);
    record.resumePending = fadeIn;

    auto play = mixer_.PreparePlay(record.voice, clips_[record.clip].id, start, playable.looping,
                                   ParamsFor(record, now));
    if (!play) {
        (void)mixer_.ReleaseVoice(record.voice);
        ReleaseClipUser(record.clip);
        record.voice = audio::AudioVoiceHandle{};
        record.clip = 0;
        record.resume.Set(1.0f);
        record.resumePending = false;
        return StartOutcome::Failed;
    }

    record.play = play.Value();
    record.state = RecordState::Deferred;
    record.started = false;
    record.voiceStartFrame = start;
    // 시작 frame이 올 때까지 커서는 시작 위치에 멈춘다(CollectStarted가 다시 맞춘다).
    record.cursor = static_cast<double>(start);
    record.cursorMixFrame = now;
    record.cursorRunning = false;
    record.pendingStopFade = NoPendingStop;
    // 공간 재생과 ramp 중 재생은 묶음마다 다시 게시한다(PublishParams).
    record.paramsDirty =
        record.spatialized || record.volume.IsRamping(now) || record.pitch.IsRamping(now);
    voiceOwners_[record.voice.index] = slot + 1;
    // 앞에 미룬 Play가 있으면 순서를 지키려고 뒤에 붙인다.
    if (!deferred_.IsEmpty() || !TrySubmitPlay(slot)) {
        ROCKS_ASSERT(deferred_.Size() < deferred_.Capacity());
        (void)deferred_.TryPushBack(slot);
        ++stats_.deferredPlays;
    }

    return StartOutcome::Started;
}

void AudioDeviceContext::OnStartFailure(uint32 slot, StartOutcome outcome,
                                        ControlBatch& batch) noexcept {
    Record& record = records_[slot];
    if (outcome == StartOutcome::Failed) {
        ++stats_.failedPlays;
        EndVoiceless(slot, PlaybackEndReason::Failed, batch);
        return;
    }

    // 여유 부족(UE StopOldestStoppingSource): 정지 fade 중 voice 하나를 짧게 끊어 다음 묶음에
    // 자리를 만든다.
    if (CutStoppingVoice(outcome == StartOutcome::NoStreamSlot)) {
        ++stats_.headroomCuts;
    }

    if (KeepsWithoutVoice(*record.sound.Get(), record.remainActive)) {
        ++stats_.resumeFailures;
        return;
    }

    if (outcome == StartOutcome::NoStreamSlot) {
        // 스트림 슬롯이 없다(3.40.3절 — Ignored는 장치 없음이라 따로 둔다).
        ++stats_.streamSlotRejects;
    } else {
        ++stats_.voiceLimitRejects;
    }

    EndVoiceless(slot, PlaybackEndReason::VoiceLimit, batch);
}

void AudioDeviceContext::DetachVoice(uint32 slot, ControlBatch& batch, uint64 now) noexcept {
    Record& record = records_[slot];
    ROCKS_ASSERT(record.state == RecordState::Submitted);
    // Thread 렌더에서는 수거 뒤에 시작이 게시될 수 있다: 떼어 낸 voice는 이벤트를 내지 않으므로
    // 이미 시작했으면 여기서 Started를 낸다(소리가 난 재생은 Started가 있어야 한다).
    if (!record.started) {
        auto status = mixer_.QueryVoice(record.voice);
        if (status && status.Value().startMixFrame != audio::UnsetMixFrame) {
            if (!record.everStarted) {
                record.everStarted = true;
                Emit(batch, PlaybackHandle{slot, record.generation}, PlaybackEventKind::Started,
                     PlaybackEndReason::None, status.Value().startMixFrame);
            }

            record.cursor = static_cast<double>(record.voiceStartFrame);
            record.cursorMixFrame = status.Value().startMixFrame;
            record.cursorRunning = true;
        }
    }

    ROCKS_ASSERT(detached_.Size() < detached_.Capacity());
    (void)detached_.TryPushBack(DetachedVoice{record.voice, record.clip, record.weight,
                                              record.sequence, audio::DefaultStopFadeFrames});
    voiceOwners_[record.voice.index] = 0;
    (void)mixer_.RequestStop(record.voice, audio::DefaultStopFadeFrames);
    RemoveSlot(awaitingStart_, slot);
    record.voice = audio::AudioVoiceHandle{};
    record.clip = 0;
    record.started = false;
    record.paramsDirty = false;
    record.resume.Set(1.0f);
    record.resumePending = false;
    record.state = RecordState::Voiceless;
    // 시작하기 전에 떼었으면 그 voice의 시작 위치에서 커서를 돌린다. 시작했으면 지금까지 진행한다.
    if (!record.cursorRunning) {
        record.cursor = static_cast<double>(record.voiceStartFrame);
        record.cursorRunning = true;
        record.cursorMixFrame = now;
    }
}

void AudioDeviceContext::ReleaseDeferredVoice(uint32 slot, uint64 now) noexcept {
    Record& record = records_[slot];
    ROCKS_ASSERT(record.state == RecordState::Deferred);
    // Play 명령을 보내지 않았으므로 준비를 버리고(Reserved로) 바로 놓는다.
    const bool abandoned = mixer_.AbandonPreparedPlay(record.voice).IsOk();
    const bool released = abandoned && mixer_.ReleaseVoice(record.voice).IsOk();
    ROCKS_ASSERT(released);
    (void)released;
    ReleaseClipUser(record.clip);
    voiceOwners_[record.voice.index] = 0;
    RemoveSlot(deferred_, slot);
    record.voice = audio::AudioVoiceHandle{};
    record.clip = 0;
    record.started = false;
    record.paramsDirty = false;
    record.pendingStopFade = NoPendingStop;
    record.resume.Set(1.0f);
    record.resumePending = false;
    record.state = RecordState::Voiceless;
    record.cursor = static_cast<double>(record.voiceStartFrame);
    record.cursorRunning = true;
    record.cursorMixFrame = now;
}

bool AudioDeviceContext::CutStoppingVoice(bool streamOnly) noexcept {
    // 정지 fade 중 voice 가운데 W가 가장 낮은 것, 동률이면 먼저 수락한 재생의 것이다(UE
    // StopOldestStoppingSource — AD 4377–4446, 동률 규칙은 Rocks — 7절). 스트림 슬롯이 모자라면
    // 스트림 clip을 쥔 voice만 본다(다른 voice를 끊어도 슬롯이 생기지 않는다). fade가 이미 짧으면
    // 끊지 않는다.
    constexpr usize None = NumericLimits<usize>::Max();
    const auto eligible = [&](uint32 clip, uint32 stopFade) noexcept {
        return stopFade > MinStopFadeFrames && (!streamOnly || clips_[clip].hasStreamSlot);
    };
    usize bestDetached = None;
    usize bestRecord = None;
    float bestWeight = 0.0f;
    uint64 bestSequence = 0;
    const auto better = [&](float weight, uint64 sequence) noexcept {
        return (bestDetached == None && bestRecord == None) || weight < bestWeight ||
               (weight == bestWeight && sequence < bestSequence);
    };

    for (usize index = 0; index < detached_.Size(); ++index) {
        const DetachedVoice& voice = detached_[index];
        if (eligible(voice.clip, voice.stopFade) && better(voice.weight, voice.sequence)) {
            bestDetached = index;
            bestRecord = None;
            bestWeight = voice.weight;
            bestSequence = voice.sequence;
        }
    }

    for (usize index = 0; index < records_.Size(); ++index) {
        const Record& record = records_[index];
        if (record.state == RecordState::Submitted && record.stopRequested &&
            eligible(record.clip, record.stopFade) && better(record.weight, record.sequence)) {
            bestRecord = index;
            bestDetached = None;
            bestWeight = record.weight;
            bestSequence = record.sequence;
        }
    }

    if (bestDetached != None) {
        detached_[bestDetached].stopFade = MinStopFadeFrames;
        (void)mixer_.RequestStop(detached_[bestDetached].voice, MinStopFadeFrames);
        return true;
    }

    if (bestRecord != None) {
        records_[bestRecord].stopFade = MinStopFadeFrames;
        (void)mixer_.RequestStop(records_[bestRecord].voice, MinStopFadeFrames);
        return true;
    }

    return false;
}

void AudioDeviceContext::EndVoiceless(uint32 slot, PlaybackEndReason reason,
                                      ControlBatch& batch) noexcept {
    Record& record = records_[slot];
    ROCKS_ASSERT(record.state == RecordState::Voiceless);
    // 한 번도 소리를 내지 않았으면 끝 frame이 없다(3.41.4절).
    const uint64 mixFrame = record.everStarted ? mixer_.CompletedMixFrame() : audio::UnsetMixFrame;
    Emit(batch, PlaybackHandle{slot, record.generation}, PlaybackEventKind::Ended, reason,
         mixFrame);
    record = Record{};
}

} // namespace rocks
