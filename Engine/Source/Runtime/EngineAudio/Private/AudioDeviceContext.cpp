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
      candidates_(allocator), groups_(allocator), groupScratch_(allocator), mixes_(allocator),
      overrides_(allocator), adjustments_(allocator), categoryScales_(allocator),
      categoryCache_(allocator), passive_(allocator) {
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
        !emitters_.TryResize(MaxAudioEmitters) || !mixes_.TryReserve(MaxActiveMixes) ||
        !overrides_.TryReserve(MaxMixOverrides) ||
        !adjustments_.TryReserve(MaxActiveMixes * MaxMixAdjusters + MaxMixOverrides) ||
        !categoryScales_.TryReserve(MaxCategoryDistanceScales) ||
        !categoryCache_.TryReserve(MaxPlaybacks) || !passive_.TryReserve(MaxActiveMixes)) {
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
    virtual_ = batch.virtualization;
    focus_ = batch.focus;
    focusInitializeOnFirstUpdate_ = batch.focusInitializeOnFirstUpdate;
    occlusion_ = batch.occlusion;
    userVolume_ = batch.userVolume;
    ApplySnapshots(batch);
    // 일시정지 전환을 미룬 Play 재시도 앞에 반영한다(일시정지되는 Deferred는 보내지 않고 놓는다
    // — 3.44.4절).
    UpdatePause(batch, now);
    RetryDeferred();
    // 수거를 먼저 한다(3.41.4절 — 이번 묶음에 끝난 voice가 배정의 상한 수에 들어가지 않게).
    CollectStarted(batch);
    CollectTerminals(batch);
    ReclaimVoices();
    ReclaimClips();
    // 믹스 명령을 먼저 적용하고 상태·조정을 갱신한다(UE UpdateSoundClassProperties가 parse 앞 —
    // 이번 묶음 수락 판정이 이번 묶음 분류 값을 본다, 3.43.8절).
    for (const ControlCommand& command : batch.commands) {
        if (IsMixCommand(command.type)) {
            ApplyMixCommand(command, now);
        }
    }

    UpdateMixes(now);
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
        case ControlCommandType::SetOcclusion:
            ApplySetOcclusion(command, now);
            break;
        case ControlCommandType::PushMix:
        case ControlCommandType::PopMix:
        case ControlCommandType::ClearMix:
        case ControlCommandType::ClearAllMixes:
        case ControlCommandType::SetBaseMix:
        case ControlCommandType::SetMixCategoryOverride:
        case ControlCommandType::ClearMixCategoryOverride:
        case ControlCommandType::SetCategoryDistanceScale:
            // 위에서 이미 적용했다.
            break;
        }
    }

    UpdateCategories(now);
    // 가상 재생은 검사 때만 갱신한다(UE FAudioVirtualLoop::UpdateFocusData — AVL 275–295).
    for (Record& record : records_) {
        if (record.state != RecordState::Inactive && record.state != RecordState::Virtual &&
            record.spatialized) {
            UpdateSpatial(record, now, false);
        }
    }

    if (!shuttingDown_) {
        UpdateOcclusion(batch, now);
        UpdateEstimates(batch, now);
        UpdateVirtualization(batch, now);
        UpdateConcurrency(batch, now);
        UpdateWeights(now);
        AssignVoices(batch, now);
        // passive 믹스는 배정 뒤에 본다(UE UpdatePassiveSoundMixModifiers — 다음 묶음
        // 반영, 3.43.7절).
        UpdatePassiveMixes(now);
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
        if (records_[slot].state == RecordState::Voiceless ||
            records_[slot].state == RecordState::Virtual) {
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
    stats.virtualPlaybacks = 0;
    for (const Record& record : records_) {
        if (record.state == RecordState::Voiceless) {
            ++stats.voicelessPlaybacks;
        } else if (record.state == RecordState::Virtual) {
            ++stats.virtualPlaybacks;
        } else if (record.state != RecordState::Inactive) {
            if (record.stopRequested) {
                ++stats.stoppingVoices;
            } else {
                ++stats.activeVoices;
            }
        }

        if (record.state != RecordState::Inactive && record.paused) {
            ++stats.pausedPlaybacks;
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
    record.rotation = command.rotation;
    record.emitter = command.emitter;
    record.priority = command.priority;
    // 감쇠 경로가 없는 재생의 실효 우선순위는 사운드 값 그대로다(3.42.4절).
    record.voicePriority = command.priority;
    record.concurrencyPriority = command.priority;
    record.focusFactor = 1.0f;
    record.focusInitialized = false;
    record.focusMixFrame = now;
    record.focus = FocusScales{};
    // 분류(3.43.2절): 수락 판정(짧은 소리·가상·동시 발음)도 이번 묶음 분류 값을 본다.
    record.category = Move(command.category);
    record.categoryValues = ComputeCategory(record.category.Get(), now);
    record.optionAlwaysPlay = command.alwaysPlay;
    record.optionUiSound = command.isUiSound;
    record.alwaysPlay = command.alwaysPlay || record.categoryValues.alwaysPlay;
    record.isUiSound = command.isUiSound || record.categoryValues.isUiSound;
    // 일시정지된 scene의 UI 아닌 재생은 시계가 멈춘 채 연다(3.44.2절).
    record.paused = scenes_[command.scene].paused && !record.isUiSound;
    record.pauseStartMixFrame = now;
    record.voicePaused = false;
    record.cursorUnread = 0;
    record.playbackLowPass = command.lowPassFrequency;
    record.playbackHighPass = command.highPassFrequency;
    record.hasOcclusionInterval = command.hasOcclusionInterval;
    record.occlusionInterval = command.occlusionInterval;
    record.everAssigned = false;
    record.occlusionPending = false;
    record.occlusionChecked = false;
    record.occlusionSequence = 0;
    record.occlusionCheckFrame = now;
    record.occlusionVolume.Set(1.0f);
    record.occlusionFilter.Set(MaxAttenuationFilterFrequency);
    // Play2D(NonSpatial)는 감쇠가 있어도 2D다(UE PlaySound2D). 위치 재생에 감쇠가 없으면 2D다.
    record.spatialized =
        command.placement != PlayPlacement::NonSpatial && playable.attenuation.HasValue();
    record.spatial = SpatialParams{};
    if (record.spatialized) {
        UpdateSpatial(record, now, true);
        // 게임 쪽 첫 검사 결과는 보간 없이 바로 쓴다(UE 첫 검사 — ActiveSound.cpp:1600–1615).
        if (command.occlusionChecked && playable.attenuation.Value().enableOcclusion &&
            !occlusion_.disabled) {
            ApplyOcclusionResult(record, command.occluded, now, true);
        }
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
    if (!playable.looping && playable.FrameCount() <= shortFrames && !IsAudible(record, true)) {
        ++stats_.inaudibleEnds;
        EndVoiceless(slot, PlaybackEndReason::Inaudible, batch);
        return;
    }

    record.owner = command.owner;
    record.concurrencyCount = command.concurrencyCount;
    for (uint32 index = 0; index < command.concurrencyCount; ++index) {
        record.concurrency[index] = command.concurrency[index];
    }

    // 가청 거리 밖의 가상화 가능한 loop는 voice 없이 가상으로 시작한다(UE AD 5387–5400 — 동시
    // 발음은 실현 때 본다, 3.41.3절).
    if (IsVirtualizable(record) && !IsAudible(record)) {
        EnterVirtual(slot, now);
        return;
    }

    // 동시 발음 그룹(3.41.2절): 하나라도 거부하면 시작하지 못한다(Ended(Concurrency)). 가상화
    // 가능한 loop는 거부 대신 가상으로 둔다(UE AD 5412–5432).
    if (command.concurrencyCount != 0) {
        const ConcurrencyOutcome outcome = EvaluateConcurrency(slot, batch, now, false);
        if (outcome == ConcurrencyOutcome::Rejected && IsVirtualizable(record)) {
            ++stats_.concurrencyRejects;
            EnterVirtual(slot, now);
        } else if (outcome == ConcurrencyOutcome::Rejected) {
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
    if (record->state == RecordState::Voiceless || record->state == RecordState::Virtual) {
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

    // 재생 LPF·HPF는 바로 바뀐다(UE SetLowPassFilterFrequency — 3.43.1절).
    if (command.parameter == PlaybackParameter::LowPassFrequency) {
        record->playbackLowPass = command.value;
        record->paramsDirty = true;
        return;
    }

    if (command.parameter == PlaybackParameter::HighPassFrequency) {
        record->playbackHighPass = command.value;
        record->paramsDirty = true;
        return;
    }

    // 지금 값에서 새 목표로 ramp한다(진행 중 ramp는 그 자리에서 이어 받는다). 게시는
    // PublishParams가 묶음마다 한다 — 믹서 gain이 하나뿐이라 시각 명령 ramp는 공간 갱신이 덮기
    // 때문이다(3.35.6절).
    ValueRamp& ramp =
        command.parameter == PlaybackParameter::Volume ? record->volume : record->pitch;
    // 일시정지 중이면 멈춘 시각에서 걸고, 풀린 뒤 진행한다(3.44.3절).
    const uint64 at = RecordNow(*record, now);
    ramp.StartFrom(ramp.At(at), command.value, at, command.rampFrames);
    record->paramsDirty = true;
}

void AudioDeviceContext::ApplySnapshots(const ControlBatch& batch) noexcept {
    for (ControlScene& scene : scenes_) {
        scene.listenerJumped = false;
        scene.pauseChanged = false;
    }

    const double force = static_cast<double>(virtual_.forceUpdateDistance);
    for (const SceneUpdate& update : batch.scenes) {
        if (update.index < scenes_.Size()) {
            ControlScene& scene = scenes_[update.index];
            // 리스너가 forceUpdateDistance보다 많이 움직이면 가상 재생을 바로 검사한다(UE AVL
            // 451–456).
            scene.listenerJumped =
                (update.position - scene.listenerPosition).LengthSquared() > force * force;
            // 새 World가 받은 칸은 일시정지 아님에서 연다(3.44.1절).
            const bool wasPaused = scene.epoch == update.epoch && scene.paused;
            scene.pauseChanged = scene.pauseChanged || wasPaused != update.paused;
            scene.paused = update.paused;
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
            emitter.rotation = update.rotation;
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

        if (record.state == RecordState::Voiceless || record.state == RecordState::Virtual) {
            EndVoiceless(slot, PlaybackEndReason::SceneRemoved, batch);
        } else {
            RequestStop(record, SceneRemoveFadeFrames, PlaybackEndReason::SceneRemoved);
        }
    }

    // 가상이 된 재생에서 떼어 낸 voice(steal fade 중 포함)도 그 World와 함께 짧게 끊는다.
    for (DetachedVoice& detached : detached_) {
        if (detached.scene != command.scene || detached.sceneEpoch != command.sceneEpoch) {
            continue;
        }

        detached.fading = false;
        if (detached.stopFade > SceneRemoveFadeFrames) {
            detached.stopFade = SceneRemoveFadeFrames;
            (void)mixer_.RequestStop(detached.voice, SceneRemoveFadeFrames);
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

        if (record.state == RecordState::Voiceless || record.state == RecordState::Virtual) {
            EndVoiceless(slot, PlaybackEndReason::EmitterRemoved, batch);
        } else {
            RequestStop(record, audio::DefaultStopFadeFrames, PlaybackEndReason::EmitterRemoved);
        }
    }
}

void AudioDeviceContext::UpdateSpatial(Record& record, uint64 now, bool accepting) noexcept {
    const SoundPlayable& playable = *record.sound.Get();
    const SoundAttenuation& attenuation = playable.attenuation.Value();
    const ControlScene& scene = scenes_[record.scene];
    if (record.placement == PlayPlacement::Emitter) {
        const ControlEmitter& emitter = emitters_[record.emitter.slot];
        // emitter가 사라졌으면 마지막 위치를 그대로 쓴다(재생은 RemoveEmitter가 끝낸다).
        if (emitter.generation == record.emitter.generation) {
            record.position = emitter.position;
            record.rotation = emitter.rotation;
        }
    }

    // 1) 우선순위 거리 감쇠(UE AS 2080–2103 — 모양 반경을 빼지 않은 거리).
    const float listenerDistance =
        static_cast<float>((record.position - scene.listenerPosition).Length());
    const float attenuated =
        record.priority * PriorityAttenuationScale(attenuation, listenerDistance);
    const auto factorNow = [&]() noexcept {
        return FocusFactor(
            attenuation, focus_,
            FocusAzimuthDegrees(scene.listenerPosition, scene.listenerRotation, record.position));
    };

    // 2) focus(UE AS 2105–2130). 수락 때는 계수를 spatialize·focus일 때만 계산하고
    // Priority·Distance 배수는 그 계수로 늘 정한다(UE AD 6279·6293–6309·6745–6750 — Volume은 아직
    // 적용하지 않는다). 갱신 블록이 돌지 않는 감쇠(spatialize·focus 둘 다 꺼짐)는 수락 때 배수를
    // 그대로 두고 H = 1이다.
    if (accepting) {
        const float factor =
            attenuation.spatialize && attenuation.enableListenerFocus ? factorNow() : 1.0f;
        record.focus = ComputeFocusScales(attenuation, focus_, factor);
        record.focus.volume = 1.0f;
        record.voicePriority = math::Clamp(attenuated, 0.0f, MaxSoundPriority);
        record.concurrencyPriority =
            math::Clamp(record.focus.priority, 0.0f, MaxSoundPriority) * record.priority;
    } else if (attenuation.spatialize || attenuation.enableListenerFocus) {
        const float target = factorNow();
        // focus 보간은 record 시계다(일시정지 중 멈춤 — 3.44.3절).
        const uint64 at = RecordNow(record, now);
        if (attenuation.enableFocusInterpolation &&
            (record.focusInitialized || !focusInitializeOnFirstUpdate_)) {
            const double seconds = static_cast<double>(at - record.focusMixFrame) /
                                   static_cast<double>(mixer_.Config().sampleRate);
            record.focusFactor = InterpolateFocusFactor(
                record.focusFactor, target, static_cast<float>(seconds),
                attenuation.focusAttackInterpSpeed, attenuation.focusReleaseInterpSpeed);
        } else {
            record.focusFactor = target;
        }

        record.focusInitialized = true;
        record.focusMixFrame = at;
        record.focus = attenuation.enableListenerFocus
                           ? ComputeFocusScales(attenuation, focus_, record.focusFactor)
                           : FocusScales{};
        // UE PriorityHighest(AS 1073·2117–2122): 1에서 시작해 focus 배수 전의 P × pa로 올라간다.
        const float highest = math::Max(1.0f, attenuated);
        record.voicePriority =
            math::Clamp(attenuated * record.focus.priority, 0.0f, MaxSoundPriority);
        record.concurrencyPriority =
            math::Clamp(highest * record.focus.priority, 0.0f, MaxSoundPriority) * record.priority;
    } else {
        // 블록이 돌지 않으면 배수는 기본값 1이고 PriorityHighest는 1이다(UE FocusDataToApply —
        // ActiveSound.cpp:2052–2053·2318).
        record.focus = FocusScales{};
        record.voicePriority = math::Clamp(attenuated, 0.0f, MaxSoundPriority);
        record.concurrencyPriority = record.priority;
    }

    // 3) 거리 감쇠·공기 흡수·패닝(UE AS 2135–2156·2257–2276). 거리 배수에 분류 감쇠 배수가 곱해진다
    // (가청 판정에는 들어가지 않는다 — 3.43.6절).
    record.spatial =
        ComputeSpatialParams(attenuation, playable.ChannelCount(), scene.listenerPosition,
                             scene.listenerRotation, record.position, record.rotation,
                             record.focus.distance * record.categoryValues.distanceScale);
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
        // 공간 재생과 ramp 중 재생은 묶음마다 다시 낸다(사용자·resume ramp는 record 시계).
        const uint64 at = RecordNow(record, now);
        record.paramsDirty = record.spatialized || record.volume.IsRamping(at) ||
                             record.pitch.IsRamping(at) || record.resume.IsRamping(at) ||
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
            record.cursorUnread = 0;
            record.cursorRunning = true;
            // 복귀 fade-in은 실제 시작에서 시작한다(3.41.1절). 일시정지 중이면 멈춘 시각에서 걸어
            // 풀린 뒤 진행한다(3.44.3절).
            if (record.resumePending) {
                record.resumePending = false;
                const uint64 at = RecordNow(record, mixer_.CompletedMixFrame());
                record.resume.StartFrom(0.0f, 1.0f, startMixFrame < at ? startMixFrame : at,
                                        ResumeFadeFrames);
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
    // 사용자·차폐·resume ramp와 pitch는 record 시계, steal fader는 mix 시계다(3.44.3절).
    const uint64 at = RecordNow(record, frame);
    // 곱은 믹서 범위로 자른다(UE처럼 — PreparePlay·PublishParams가 범위 밖을 거부한다).
    const float stereo =
        playable.normalizeStereo && playable.ChannelCount() == 2 ? StereoNormalizationGain : 1.0f;
    params.gain = ClampGain(
        record.volume.At(at) * playable.volume * stereo * record.categoryValues.volume *
        record.spatial.attenuationGain * record.focus.volume * record.occlusionVolume.At(at) *
        record.resume.At(at) * record.concurrencyScale * record.stealFader.At(frame));
    // LPF는 공기 흡수·차폐·분류·재생 값의 최솟값, HPF는 최댓값이다(UE
    // FSoundSource::SetFilterFrequency — Audio.cpp:555–623).
    params.lowPassCutoffHz =
        math::Min(math::Min(record.spatial.lowPassHz, OcclusionFilterAt(record, at)),
                  math::Min(record.categoryValues.lowPassFrequency, record.playbackLowPass));
    params.highPassCutoffHz = math::Max(record.spatial.highPassHz, record.playbackHighPass);
    params.pitch = EffectivePitch(record, at);
    params.paused = record.paused;
    params.panMode = audio::AudioPanMode::Direct;
    if (record.spatial.azimuthPan) {
        params.panMode = audio::AudioPanMode::Azimuth;
        params.azimuthDegrees = record.spatial.azimuthDegrees;
        params.rightAzimuthDegrees = record.spatial.rightAzimuthDegrees;
        params.nonSpatializedAmount = record.spatial.nonSpatializedAmount;
        params.omniMode = record.spatial.omniMode;
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
