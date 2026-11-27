#include "AudioDeviceContext.h"

#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>

namespace rocks {

namespace {

// CollectFinished 한 번에 받는 terminal 수다(다 받을 때까지 반복한다).
constexpr usize TerminalBatch = 64;

float ClampGain(float value) noexcept {
    return math::Clamp(value, 0.0f, audio::MaxVoiceGain);
}

void RemoveValue(Array<uint32>& values, uint32 value) noexcept {
    for (usize index = 0; index < values.Size(); ++index) {
        if (values[index] == value) {
            (void)values.TryRemoveAt(index);
            return;
        }
    }
}

} // namespace

float AudioDeviceContext::ValueRamp::At(uint64 frame) const noexcept {
    if (frames == 0 || frame >= startFrame + frames) {
        return target;
    }

    const float t = frame <= startFrame
                        ? 0.0f
                        : static_cast<float>(frame - startFrame) / static_cast<float>(frames);
    return from + (target - from) * t;
}

bool AudioDeviceContext::ValueRamp::IsRamping(uint64 frame) const noexcept {
    return frames != 0 && frame < startFrame + frames;
}

bool AudioDeviceContext::ValueRamp::IsRising(uint64 frame) const noexcept {
    return IsRamping(frame) && target > from;
}

void AudioDeviceContext::ValueRamp::StartFrom(float current, float value, uint64 frame,
                                              uint32 length) noexcept {
    from = current;
    target = value;
    startFrame = frame;
    frames = length;
}

AudioDeviceContext::AudioDeviceContext(Allocator& allocator, audio::AudioOutputDevice& device,
                                       audio::AudioStreamService* streams,
                                       const audio::AudioDecoderTable* decoders) noexcept
    : allocator_(allocator), device_(device), mixer_(device.Mixer()), streams_(streams),
      decoders_(decoders), records_(allocator), voiceOwners_(allocator), clips_(allocator),
      scenes_(allocator), emitters_(allocator), releasing_(allocator), detached_(allocator),
      deferred_(allocator), awaitingStart_(allocator), terminalScratch_(allocator),
      candidates_(allocator), groups_(allocator), groupScratch_(allocator) {
}

AudioDeviceContext::~AudioDeviceContext() noexcept = default;

Result<void> AudioDeviceContext::Initialize(uint32 maxActiveVoices) noexcept {
    const uint32 mixerVoices = mixer_.Config().maxVoices;
    const uint32 mixerClips = mixer_.Config().maxClips;
    if (!records_.TryResize(MaxPlaybacks) || !voiceOwners_.TryResize(mixerVoices) ||
        !clips_.TryReserve(mixerClips) || !releasing_.TryReserve(mixerVoices) ||
        !detached_.TryReserve(mixerVoices) || !candidates_.TryReserve(MaxPlaybacks) ||
        !groups_.TryResize(MaxActiveConcurrencyGroups) || !groupScratch_.TryReserve(MaxPlaybacks) ||
        !deferred_.TryReserve(MaxPlaybacks) || !awaitingStart_.TryReserve(MaxPlaybacks) ||
        !terminalScratch_.TryResize(TerminalBatch) || !scenes_.TryResize(MaxAudioScenes) ||
        !emitters_.TryResize(MaxAudioEmitters)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    // 엔진 scene은 종료까지 산다(엔진 소유 재생 — 리스너 원점·항등 회전).
    scenes_[EngineSceneIndex].epoch = EngineSceneEpoch;

    for (uint32& owner : voiceOwners_) {
        owner = 0;
    }

    maxActiveVoices_ = maxActiveVoices;
    return Result<void>::Success();
}

void AudioDeviceContext::Process(ControlBatch& batch) noexcept {
    const uint64 now = mixer_.CompletedMixFrame();
    minDistanceDelta_ = batch.concurrencyMinDistanceDelta;
    ApplySnapshots(batch);
    RetryDeferred();
    // 수거를 먼저 한다(3.41.4절 — 이번 묶음에 끝난 voice가 배정의 상한 수에 들어가지 않게).
    CollectStarted(batch);
    CollectTerminals(batch);
    ReclaimVoices();
    ReclaimClips();
    for (ControlCommand& command : batch.commands) {
        switch (command.type) {
        case ControlCommandType::Play:
            ApplyPlay(command, batch);
            break;
        case ControlCommandType::Stop:
            ApplyStop(command, batch);
            break;
        case ControlCommandType::SetParameter:
            ApplySetParameter(command, now);
            break;
        case ControlCommandType::RemoveScene:
            ApplyRemoveScene(command, batch);
            break;
        case ControlCommandType::RemoveEmitter:
            ApplyRemoveEmitter(command, batch);
            break;
        }
    }

    for (Record& record : records_) {
        if (record.state != RecordState::Inactive && record.spatialized) {
            UpdateSpatial(record);
        }
    }

    if (!shuttingDown_) {
        UpdateEstimates(batch, now);
        UpdateConcurrency(batch, now);
        UpdateWeights(now);
        AssignVoices(batch, now);
    }

    ApplyMasterGain(batch.masterGain);
    PublishParams(now);
    // Inline 서비스는 묶음마다 이 자리에서 링을 채운다(엔진 Inline 시험이 훅 없이 결정적
    // — 3.40.2절).
    if (streams_ != nullptr && streams_->Mode() == audio::AudioStreamServiceMode::Inline) {
        streams_->PumpAll();
    }

    // 게임 스레드가 원자값 없이 읽도록 사본을 묶음에 싣는다.
    batch.stats = Stats();
}

void AudioDeviceContext::StopAll(uint32 fadeFrames) noexcept {
    mixer_.StopAll(fadeFrames);
    RetryDeferred();
    // voice 없는 재생은 믹서가 모른다. 종료에서는 콜백을 부르지 않으므로 결과 없이
    // 비운다(그룹에서도 뺀다 — voice를 쥔 재생은 terminal에서 뺀다).
    for (uint32 slot = 0; slot < records_.Size(); ++slot) {
        if (records_[slot].state == RecordState::Voiceless) {
            LeaveConcurrencyGroups(slot, mixer_.CompletedMixFrame());
            records_[slot] = Record{};
        }
    }

    shuttingDown_ = true;
}

bool AudioDeviceContext::IsIdle() const noexcept {
    if (!releasing_.IsEmpty() || !detached_.IsEmpty()) {
        return false;
    }

    for (const Record& record : records_) {
        if (record.state != RecordState::Inactive) {
            return false;
        }
    }

    for (const ClipEntry& entry : clips_) {
        if (entry.registered || entry.draining) {
            return false;
        }
    }

    return true;
}

void AudioDeviceContext::Abandon() noexcept {
    for (Record& record : records_) {
        record = Record{};
    }

    for (ClipEntry& entry : clips_) {
        ResetStream(entry);
        entry = ClipEntry{};
    }

    for (ControlScene& scene : scenes_) {
        scene = ControlScene{};
    }

    for (ControlEmitter& emitter : emitters_) {
        emitter = ControlEmitter{};
    }

    for (uint32& owner : voiceOwners_) {
        owner = 0;
    }

    releasing_.Clear();
    detached_.Clear();
    deferred_.Clear();
    awaitingStart_.Clear();
    for (ConcurrencyGroupState& group : groups_) {
        group = ConcurrencyGroupState{};
    }
}

AudioControlStats AudioDeviceContext::Stats() const noexcept {
    AudioControlStats stats = StatsWithoutStreams();
    stats.activeStreams = streams_ != nullptr ? streams_->AcquiredSlots() : 0u;
    return stats;
}

AudioControlStats AudioDeviceContext::StatsWithoutStreams() const noexcept {
    AudioControlStats stats = stats_;
    stats.activeVoices = 0;
    stats.stoppingVoices = static_cast<uint32>(detached_.Size());
    stats.voicelessPlaybacks = 0;
    for (const Record& record : records_) {
        if (record.state == RecordState::Voiceless) {
            ++stats.voicelessPlaybacks;
        } else if (record.state != RecordState::Inactive) {
            if (record.stopRequested) {
                ++stats.stoppingVoices;
            } else {
                ++stats.activeVoices;
            }
        }
    }

    stats.releasingVoices = static_cast<uint32>(releasing_.Size());
    stats.pendingDeferred = static_cast<uint32>(deferred_.Size());
    stats.registeredClips = 0;
    for (const ClipEntry& entry : clips_) {
        if (entry.registered) {
            ++stats.registeredClips;
        }
    }

    return stats;
}

void AudioDeviceContext::RetryDeferred() noexcept {
    usize submitted = 0;
    while (submitted < deferred_.Size()) {
        const uint32 slot = deferred_[submitted];
        if (!TrySubmitPlay(slot)) {
            break;
        }

        ++submitted;
    }

    if (submitted != 0) {
        (void)deferred_.TryRemoveAt(0, submitted);
    }
}

void AudioDeviceContext::ApplyPlay(ControlCommand& command, ControlBatch& batch) noexcept {
    const uint32 slot = command.handle.slot;
    ROCKS_ASSERT(slot < records_.Size());
    Record& record = records_[slot];
    // 게임은 Ended를 받은 슬롯만 다시 쓴다(Ended는 기록을 비운 뒤에 나간다).
    ROCKS_ASSERT(record.state == RecordState::Inactive);
    const SoundPlayable& playable = *command.sound.Get();
    // 보내는 사이에 scene·emitter가 사라졌으면 소리 없이 끝낸다(게임은 죽은 scene에 재생을 보내지
    // 않지만, 같은 묶음 안의 해제 순서를 지킨다).
    if (command.scene >= scenes_.Size() || scenes_[command.scene].epoch != command.sceneEpoch) {
        Emit(batch, command.handle, PlaybackEventKind::Ended, PlaybackEndReason::SceneRemoved,
             audio::UnsetMixFrame);
        return;
    }

    if (command.placement == PlayPlacement::Emitter &&
        (command.emitter.slot >= emitters_.Size() ||
         emitters_[command.emitter.slot].generation != command.emitter.generation)) {
        Emit(batch, command.handle, PlaybackEventKind::Ended, PlaybackEndReason::EmitterRemoved,
             audio::UnsetMixFrame);
        return;
    }

    if (shuttingDown_) {
        Emit(batch, command.handle, PlaybackEventKind::Ended, PlaybackEndReason::ShuttingDown,
             audio::UnsetMixFrame);
        return;
    }

    // voice는 아직 잡지 않는다(묶음 끝 AssignVoices — 3.41.1절).
    const uint64 now = mixer_.CompletedMixFrame();
    record.generation = command.handle.generation;
    record.sound = Move(command.sound);
    // fade-in은 0에서 옵션 volume까지의 사용자 ramp다(3.35.6절).
    if (command.fadeInFrames != 0) {
        record.volume.StartFrom(0.0f, command.volume, now, command.fadeInFrames);
    } else {
        record.volume.Set(command.volume);
    }

    record.pitch.Set(command.pitch);
    record.resume.Set(1.0f);
    record.scene = command.scene;
    record.sceneEpoch = command.sceneEpoch;
    record.placement = command.placement;
    record.position = command.position;
    record.emitter = command.emitter;
    // Play2D(NonSpatial)는 감쇠가 있어도 2D다(UE PlaySound2D). 위치 재생에 감쇠가 없으면 2D다.
    record.spatialized =
        command.placement != PlayPlacement::NonSpatial && playable.attenuation.HasValue();
    record.spatial = SpatialParams{};
    if (record.spatialized) {
        UpdateSpatial(record);
    }

    record.started = false;
    record.everStarted = false;
    record.paramsDirty = false;
    record.pendingStopFade = NoPendingStop;
    record.stopRequested = false;
    record.stopReason = PlaybackEndReason::Stopped;
    record.stopFade = NoPendingStop;
    record.resumePending = false;
    record.sequence = ++nextSequence_;
    record.priority = command.priority;
    record.remainActive = command.remainActiveIfDropped;
    record.startFrame = command.startFrame;
    record.voiceStartFrame = command.startFrame;
    record.cursor = static_cast<double>(command.startFrame);
    record.cursorMixFrame = now;
    record.cursorRunning = true;
    record.playbackTime = 0.0;
    record.playbackMixFrame = now;
    record.voicedTime = 0.0;
    record.linkCount = 0;
    record.concurrencyScale = 1.0f;
    record.stealing = false;
    record.state = RecordState::Voiceless;
    // 가청 거리 밖의 짧은 one-shot은 만들지 않는다(UE au.SoundDistanceOptimizationLength
    // — 3.41.1절).
    const uint64 shortFrames = static_cast<uint64>(playable.SampleRate()) * ShortSoundSeconds;
    if (!playable.looping && playable.FrameCount() <= shortFrames && !IsAudible(record)) {
        ++stats_.inaudibleEnds;
        EndVoiceless(slot, PlaybackEndReason::Inaudible, batch);
        return;
    }

    // 동시 발음 그룹(3.41.2절): 하나라도 거부하면 시작하지 못한다(Ended(Concurrency)).
    if (command.concurrencyCount != 0) {
        const ConcurrencyOutcome outcome = EvaluateConcurrency(slot, command, batch, now, false);
        if (outcome == ConcurrencyOutcome::Rejected) {
            ++stats_.concurrencyRejects;
            EndVoiceless(slot, PlaybackEndReason::Concurrency, batch);
        } else if (outcome == ConcurrencyOutcome::TableFull) {
            ++stats_.failedPlays;
            EndVoiceless(slot, PlaybackEndReason::Failed, batch);
        }
    }
}

void AudioDeviceContext::ApplyStop(const ControlCommand& command, ControlBatch& batch) noexcept {
    Record* record = FindRecord(command.handle);
    if (record == nullptr) {
        return;
    }

    // voice가 없으면 믹서를 부르지 않고 바로 끝낸다(3.41.4절).
    if (record->state == RecordState::Voiceless) {
        EndVoiceless(command.handle.slot, PlaybackEndReason::Stopped, batch);
        return;
    }

    RequestStop(*record, command.fadeFrames, PlaybackEndReason::Stopped);
}

void AudioDeviceContext::ApplySetParameter(const ControlCommand& command, uint64 now) noexcept {
    Record* record = FindRecord(command.handle);
    if (record == nullptr) {
        return;
    }

    // 지금 값에서 새 목표로 ramp한다(진행 중 ramp는 그 자리에서 이어 받는다). 게시는
    // PublishParams가 묶음마다 한다 — 믹서 gain이 하나뿐이라 시각 명령 ramp는 공간 갱신이 덮기
    // 때문이다(3.35.6절).
    ValueRamp& ramp =
        command.parameter == PlaybackParameter::Volume ? record->volume : record->pitch;
    ramp.StartFrom(ramp.At(now), command.value, now, command.rampFrames);
    record->paramsDirty = true;
}

void AudioDeviceContext::ApplySnapshots(const ControlBatch& batch) noexcept {
    for (const SceneUpdate& update : batch.scenes) {
        if (update.index < scenes_.Size()) {
            ControlScene& scene = scenes_[update.index];
            scene.epoch = update.epoch;
            scene.listenerPosition = update.position;
            scene.listenerRotation = update.rotation;
        }
    }

    for (const EmitterUpdate& update : batch.emitters) {
        if (update.emitter.slot < emitters_.Size()) {
            ControlEmitter& emitter = emitters_[update.emitter.slot];
            emitter.generation = update.emitter.generation;
            emitter.position = update.position;
        }
    }
}

void AudioDeviceContext::ApplyRemoveScene(const ControlCommand& command,
                                          ControlBatch& batch) noexcept {
    if (command.scene >= scenes_.Size()) {
        return;
    }

    // 같은 묶음에서 그 칸을 새 World가 받았으면(스냅숏이 새 epoch를 먼저 적용) 칸은 두고 옛 재생만
    // 끝낸다.
    if (scenes_[command.scene].epoch == command.sceneEpoch) {
        scenes_[command.scene] = ControlScene{};
    }

    for (uint32 slot = 0; slot < records_.Size(); ++slot) {
        Record& record = records_[slot];
        if (record.state == RecordState::Inactive || record.scene != command.scene ||
            record.sceneEpoch != command.sceneEpoch) {
            continue;
        }

        if (record.state == RecordState::Voiceless) {
            EndVoiceless(slot, PlaybackEndReason::SceneRemoved, batch);
        } else {
            RequestStop(record, SceneRemoveFadeFrames, PlaybackEndReason::SceneRemoved);
        }
    }
}

void AudioDeviceContext::ApplyRemoveEmitter(const ControlCommand& command,
                                            ControlBatch& batch) noexcept {
    const AudioEmitterId id = command.emitter;
    if (id.slot >= emitters_.Size()) {
        return;
    }

    if (emitters_[id.slot].generation == id.generation) {
        emitters_[id.slot] = ControlEmitter{};
    }

    for (uint32 slot = 0; slot < records_.Size(); ++slot) {
        Record& record = records_[slot];
        if (record.state == RecordState::Inactive || record.placement != PlayPlacement::Emitter ||
            record.emitter.slot != id.slot || record.emitter.generation != id.generation) {
            continue;
        }

        if (record.state == RecordState::Voiceless) {
            EndVoiceless(slot, PlaybackEndReason::EmitterRemoved, batch);
        } else {
            RequestStop(record, audio::DefaultStopFadeFrames, PlaybackEndReason::EmitterRemoved);
        }
    }
}

void AudioDeviceContext::UpdateSpatial(Record& record) noexcept {
    const SoundPlayable& playable = *record.sound.Get();
    const ControlScene& scene = scenes_[record.scene];
    Vec3d source = record.position;
    if (record.placement == PlayPlacement::Emitter) {
        const ControlEmitter& emitter = emitters_[record.emitter.slot];
        // emitter가 사라졌으면 마지막 위치를 그대로 쓴다(재생은 RemoveEmitter가 끝낸다).
        if (emitter.generation == record.emitter.generation) {
            record.position = emitter.position;
        }

        source = record.position;
    }

    record.spatial = ComputeSpatialParams(playable.attenuation.Value(), playable.ChannelCount(),
                                          scene.listenerPosition, scene.listenerRotation, source);
    record.paramsDirty = true;
}

void AudioDeviceContext::ApplyMasterGain(float gain) noexcept {
    if (gain == masterGain_ && masterGainSent_) {
        return;
    }

    auto made = mixer_.MakeSetMasterGain(audio::ImmediateMixFrame, gain,
                                         masterGainSent_ ? audio::DefaultParamRampFrames : 0);
    if (!made) {
        return;
    }

    // 링이 차면 다음 묶음에서 다시 보낸다(masterGain_을 바꾸지 않는다).
    if (device_.Submit(made.Value())) {
        masterGain_ = gain;
        masterGainSent_ = true;
        ++stats_.masterGainCommands;
    } else {
        ++stats_.masterGainRetries;
    }
}

void AudioDeviceContext::PublishParams(uint64 now) noexcept {
    for (Record& record : records_) {
        if (record.state != RecordState::Submitted || !record.paramsDirty) {
            continue;
        }

        (void)mixer_.PublishParams(record.voice, ParamsFor(record, now));
        // 공간 재생과 ramp 중 재생은 묶음마다 다시 낸다.
        record.paramsDirty = record.spatialized || record.volume.IsRamping(now) ||
                             record.pitch.IsRamping(now) || record.resume.IsRamping(now) ||
                             ConcurrencyRamping(record, now);
    }
}

void AudioDeviceContext::CollectStarted(ControlBatch& batch) noexcept {
    usize index = 0;
    while (index < awaitingStart_.Size()) {
        const uint32 slot = awaitingStart_[index];
        Record& record = records_[slot];
        auto status = mixer_.QueryVoice(record.voice);
        if (status && status.Value().startMixFrame != audio::UnsetMixFrame) {
            const uint64 startMixFrame = status.Value().startMixFrame;
            record.started = true;
            record.voiceStartMixFrame = startMixFrame;
            // 논리 커서는 실제 시작 frame에서 이 voice의 시작 위치로 다시 맞춘다(3.41.1절).
            record.cursor = static_cast<double>(record.voiceStartFrame);
            record.cursorMixFrame = startMixFrame;
            record.cursorRunning = true;
            // 복귀 fade-in은 실제 시작에서 시작한다(3.41.1절).
            if (record.resumePending) {
                record.resumePending = false;
                record.resume.StartFrom(0.0f, 1.0f, startMixFrame, ResumeFadeFrames);
                record.paramsDirty = true;
            }

            // voice를 잃었다 되찾는 일은 이벤트를 내지 않는다(Started는 재생마다 한 번).
            if (!record.everStarted) {
                record.everStarted = true;
                Emit(batch, PlaybackHandle{slot, record.generation}, PlaybackEventKind::Started,
                     PlaybackEndReason::None, startMixFrame);
            }

            (void)awaitingStart_.TryRemoveAt(index);
            continue;
        }

        ++index;
    }
}

void AudioDeviceContext::CollectTerminals(ControlBatch& batch) noexcept {
    for (;;) {
        const uint32 count = mixer_.CollectFinished(
            Span<audio::AudioVoiceTerminal>(terminalScratch_.Data(), terminalScratch_.Size()));
        for (uint32 index = 0; index < count; ++index) {
            const audio::AudioVoiceTerminal& terminal = terminalScratch_[index];
            const uint32 owner = voiceOwners_[terminal.voice.index];
            if (owner == 0) {
                // 재생에서 떼어 낸 voice는 이벤트 없이 놓기 대기로 간다(3.41.1절 분리 목록).
                for (usize at = 0; at < detached_.Size(); ++at) {
                    if (detached_[at].voice.index == terminal.voice.index &&
                        detached_[at].voice.generation == terminal.voice.generation) {
                        ROCKS_ASSERT(releasing_.Size() < releasing_.Capacity());
                        (void)releasing_.TryPushBack(
                            ReleasingVoice{detached_[at].voice, detached_[at].clip});
                        (void)detached_.TryRemoveAt(at);
                        break;
                    }
                }

                continue;
            }

            const uint32 slot = owner - 1;
            Record& record = records_[slot];
            if (record.voice.generation != terminal.voice.generation) {
                continue;
            }

            const PlaybackHandle handle{slot, record.generation};
            if (!record.started) {
                // 같은 묶음에서 시작과 끝이 함께 보이면 Started를 먼저 낸다(재생마다 한 번).
                auto status = mixer_.QueryVoice(record.voice);
                if (!record.everStarted && status &&
                    status.Value().startMixFrame != audio::UnsetMixFrame) {
                    Emit(batch, handle, PlaybackEventKind::Started, PlaybackEndReason::None,
                         status.Value().startMixFrame);
                }

                RemoveValue(awaitingStart_, slot);
            }

            PlaybackEndReason reason = PlaybackEndReason::Failed;
            switch (terminal.reason) {
            case audio::AudioVoiceEndReason::NaturalEnd:
                reason = PlaybackEndReason::NaturalEnd;
                break;
            case audio::AudioVoiceEndReason::Stopped:
                reason = record.stopReason;
                break;
            case audio::AudioVoiceEndReason::StoppedAll:
                reason = PlaybackEndReason::ShuttingDown;
                break;
            case audio::AudioVoiceEndReason::None:
            case audio::AudioVoiceEndReason::StartFailed:
            // 스트림 디코드·데이터 오류(Docs/Design/AudioSystem 3.40.3절).
            case audio::AudioVoiceEndReason::StreamFailed:
                reason = PlaybackEndReason::Failed;
                break;
            }

            Emit(batch, handle, PlaybackEventKind::Ended, reason, terminal.endMixFrame);
            ROCKS_ASSERT(releasing_.Size() < releasing_.Capacity());
            (void)releasing_.TryPushBack(ReleasingVoice{record.voice, record.clip});
            voiceOwners_[terminal.voice.index] = 0;
            LeaveConcurrencyGroups(slot, mixer_.CompletedMixFrame());
            record = Record{};
        }

        if (count < terminalScratch_.Size()) {
            return;
        }
    }
}

void AudioDeviceContext::ReclaimVoices() noexcept {
    usize index = 0;
    while (index < releasing_.Size()) {
        // Busy는 AudioRT ACK가 아직 마지막 사용 블록에 닿지 않았다는 뜻이다(다음 묶음에서 다시).
        if (mixer_.ReleaseVoice(releasing_[index].voice)) {
            ReleaseClipUser(releasing_[index].clip);
            (void)releasing_.TryRemoveAt(index);
            continue;
        }

        ++index;
    }
}

void AudioDeviceContext::ReclaimClips() noexcept {
    for (ClipEntry& entry : clips_) {
        if (entry.registered && entry.users == 0 && mixer_.UnregisterClip(entry.id)) {
            if (!entry.hasStreamSlot) {
                entry = ClipEntry{};
                continue;
            }

            // 스트림: 믹서가 더 읽지 않는다 → 작업 취소 → 서비스 반납을 기다린다(3.40.2절).
            entry.registered = false;
            entry.draining = true;
            streams_->CancelJob(entry.streamSlot);
        }

        if (entry.draining && streams_->IsReturned(entry.streamSlot)) {
            const bool released = streams_->ReleaseSlot(entry.streamSlot).IsOk();
            ROCKS_ASSERT(released);
            (void)released;
            ResetStream(entry);
            entry = ClipEntry{};
        }
    }
}

void AudioDeviceContext::ResetStream(ClipEntry& entry) noexcept {
    // 디코더가 공급자·데이터를 가리키므로 디코더부터 놓는다.
    entry.decoder.Reset();
    entry.chunks.Reset();
    entry.stream = SharedPtr<const SoundStreamData>{};
}

AudioDeviceContext::Record* AudioDeviceContext::FindRecord(PlaybackHandle handle) noexcept {
    if (handle.slot >= records_.Size()) {
        return nullptr;
    }

    Record& record = records_[handle.slot];
    if (record.state == RecordState::Inactive || record.generation != handle.generation) {
        return nullptr;
    }

    return &record;
}

audio::AudioVoiceParams AudioDeviceContext::ParamsFor(const Record& record,
                                                      uint64 frame) const noexcept {
    const SoundPlayable& playable = *record.sound.Get();
    audio::AudioVoiceParams params;
    // 곱은 믹서 범위로 자른다(UE처럼 — PreparePlay·PublishParams가 범위 밖을 거부한다).
    params.gain =
        ClampGain(record.volume.At(frame) * playable.volume * record.spatial.attenuationGain *
                  record.resume.At(frame) * record.concurrencyScale * record.stealFader.At(frame));
    params.pitch = ClampVoicePitch(record.pitch.At(frame) * playable.pitch);
    params.panMode = audio::AudioPanMode::Direct;
    if (record.spatial.azimuthPan) {
        params.panMode = audio::AudioPanMode::Azimuth;
        params.azimuthDegrees = record.spatial.azimuthDegrees;
        params.nonSpatializedAmount = record.spatial.nonSpatializedAmount;
    }

    return params;
}

Result<uint32> AudioDeviceContext::AcquireClip(const SoundPlayable& playable, uint32 startFrame,
                                               bool& slotRejected) noexcept {
    slotRejected = false;
    if (playable.stream) {
        return AcquireStreamClip(playable, startFrame, slotRejected);
    }

    const SoundPcm* pcm = playable.pcm.Get();
    usize freeEntry = clips_.Size();
    for (usize index = 0; index < clips_.Size(); ++index) {
        ClipEntry& entry = clips_[index];
        if (!entry.registered) {
            if (freeEntry == clips_.Size() && !entry.draining) {
                freeEntry = index;
            }

            continue;
        }

        // 스트림 clip은 공유하지 않는다(SPSC 링 — voice 하나).
        if (entry.hasStreamSlot) {
            continue;
        }

        // 해제 대기(사용 0) 항목도 다시 쓴다.
        if (entry.pcm.Get() == pcm && entry.loopBegin == playable.loopBegin &&
            entry.loopEnd == playable.loopEnd) {
            ++entry.users;
            return Result<uint32>::Success(static_cast<uint32>(index));
        }
    }

    if (freeEntry == clips_.Size()) {
        if (clips_.Size() >= clips_.Capacity()) {
            return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
        }

        (void)clips_.TryPushBack(ClipEntry{});
    }

    audio::AudioClipDesc desc;
    desc.channelCount = pcm->ChannelCount();
    for (uint32 channel = 0; channel < desc.channelCount; ++channel) {
        desc.channels[channel] = pcm->Channel(channel).Data();
    }

    desc.frameCount = pcm->FrameCount();
    desc.sampleRate = pcm->SampleRate();
    desc.loopBegin = playable.loopBegin;
    desc.loopEnd = playable.loopEnd;
    auto registered = mixer_.RegisterClip(desc);
    if (!registered) {
        return Result<uint32>::Failure(registered.Error());
    }

    ClipEntry& entry = clips_[freeEntry];
    entry.pcm = playable.pcm;
    entry.loopBegin = playable.loopBegin;
    entry.loopEnd = playable.loopEnd;
    entry.id = registered.Value();
    entry.users = 1;
    entry.registered = true;
    return Result<uint32>::Success(static_cast<uint32>(freeEntry));
}

Result<uint32> AudioDeviceContext::AcquireStreamClip(const SoundPlayable& playable,
                                                     uint32 startFrame,
                                                     bool& slotRejected) noexcept {
    using Acquired = Result<uint32>;
    if (streams_ == nullptr || decoders_ == nullptr) {
        return Acquired::Failure(ErrorCode::NotReady);
    }

    usize freeEntry = clips_.Size();
    for (usize index = 0; index < clips_.Size(); ++index) {
        if (!clips_[index].registered && !clips_[index].draining) {
            freeEntry = index;
            break;
        }
    }

    if (freeEntry == clips_.Size() && clips_.Size() >= clips_.Capacity()) {
        return Acquired::Failure(ErrorCode::CapacityExceeded);
    }

    const SoundStreamData& data = *playable.stream;
    auto slot = streams_->AcquireSlot(data.ChannelCount());
    if (!slot) {
        slotRejected = slot.Error() == ErrorCode::CapacityExceeded;
        return Acquired::Failure(slot.Error());
    }

    ClipEntry entry;
    entry.stream = playable.stream;
    entry.streamSlot = slot.Value();
    entry.hasStreamSlot = true;
    const auto fail = [&](ErrorCode error) noexcept {
        ResetStream(entry);
        (void)streams_->ReleaseSlot(slot.Value());
        return Acquired::Failure(error);
    };

    auto chunks = UniquePtr<audio::ResidentAudioChunkSource>::TryMake(allocator_, data.View());
    if (!chunks) {
        return fail(chunks.Error());
    }

    entry.chunks = Move(chunks.Value());
    auto codec = decoders_->Create(data.View().Header().codec, allocator_);
    if (!codec) {
        return fail(codec.Error());
    }

    audio::AudioStreamDecoderDesc desc;
    desc.view = &entry.stream->View();
    desc.chunks = entry.chunks.Get();
    desc.startFrame = startFrame;
    desc.loop = playable.looping;
    desc.loopBegin = playable.loopBegin;
    desc.loopEnd = playable.loopEnd;
    auto decoder = audio::AudioStreamDecoder::TryCreate(allocator_, Move(codec.Value()), desc);
    if (!decoder) {
        return fail(decoder.Error());
    }

    entry.decoder = Move(decoder.Value());
    audio::AudioStreamClipDesc clip;
    clip.ring = &streams_->Ring(slot.Value());
    clip.channelCount = data.ChannelCount();
    clip.frameCount = static_cast<uint32>(data.FrameCount());
    clip.sampleRate = data.SampleRate();
    clip.loopBegin = playable.loopBegin;
    clip.loopEnd = playable.loopEnd;
    clip.startFrame = startFrame;
    clip.loop = playable.looping;
    auto registered = mixer_.RegisterStreamClip(clip);
    if (!registered) {
        return fail(registered.Error());
    }

    // 등록 뒤에 작업을 시작한다(시작 대기 중인 voice는 링이 찰 때까지 소리를 내지 않는다).
    if (auto started = streams_->StartJob(slot.Value(), entry.decoder.Get()); !started) {
        (void)mixer_.UnregisterClip(registered.Value());
        return fail(started.Error());
    }

    entry.id = registered.Value();
    entry.loopBegin = playable.loopBegin;
    entry.loopEnd = playable.loopEnd;
    entry.users = 1;
    entry.registered = true;
    if (freeEntry == clips_.Size()) {
        (void)clips_.TryPushBack(ClipEntry{});
    }

    clips_[freeEntry] = Move(entry);
    return Acquired::Success(static_cast<uint32>(freeEntry));
}

void AudioDeviceContext::ReleaseClipUser(uint32 clip) noexcept {
    ROCKS_ASSERT(clip < clips_.Size() && clips_[clip].users != 0);
    --clips_[clip].users;
}

void AudioDeviceContext::RequestStop(Record& record, uint32 fadeFrames,
                                     PlaybackEndReason reason) noexcept {
    // 처음 낸 정지의 사유를 둔다(사용자 Stop 뒤 World 제거면 Stopped).
    if (!record.stopRequested) {
        record.stopRequested = true;
        record.stopReason = reason;
    }

    record.stopFade = fadeFrames < record.stopFade ? fadeFrames : record.stopFade;

    if (record.state == RecordState::Deferred) {
        record.pendingStopFade =
            fadeFrames < record.pendingStopFade ? fadeFrames : record.pendingStopFade;
        return;
    }

    (void)mixer_.RequestStop(record.voice, fadeFrames);
}

bool AudioDeviceContext::TrySubmitPlay(uint32 slot) noexcept {
    Record& record = records_[slot];
    ROCKS_ASSERT(record.state == RecordState::Deferred);
    if (!device_.Submit(record.play)) {
        return false;
    }

    record.state = RecordState::Submitted;
    ROCKS_ASSERT(awaitingStart_.Size() < awaitingStart_.Capacity());
    (void)awaitingStart_.TryPushBack(slot);
    if (record.pendingStopFade != NoPendingStop) {
        // 보내기 전에 받은 Stop이다. Play와 같은 블록에서 mailbox가 먼저 처리돼도 믹서가 새 수명을
        // 받아들인 뒤 적용한다(3.23.3절).
        (void)mixer_.RequestStop(record.voice, record.pendingStopFade);
        record.pendingStopFade = NoPendingStop;
    }

    return true;
}

void AudioDeviceContext::Emit(ControlBatch& batch, PlaybackHandle handle, PlaybackEventKind kind,
                              PlaybackEndReason reason, uint64 mixFrame) noexcept {
    // 묶음마다 2 × MaxPlaybacks를 미리 잡는다(재생 하나는 한 묶음에 결과 두 개 이하).
    ROCKS_ASSERT(batch.events.Size() < batch.events.Capacity());
    PlaybackEvent event;
    event.handle = handle;
    event.kind = kind;
    event.reason = reason;
    event.mixFrame = mixFrame;
    (void)batch.events.TryPushBack(event);
}

} // namespace rocks
