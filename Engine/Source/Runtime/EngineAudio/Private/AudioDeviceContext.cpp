#include "AudioDeviceContext.h"

#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>

namespace rocks {

namespace {

// 보내기 전 Stop이 없다는 표지다.
constexpr uint32 NoPendingStop = NumericLimits<uint32>::Max();

// CollectFinished 한 번에 받는 terminal 수다(다 받을 때까지 반복한다).
constexpr usize TerminalBatch = 64;

float ClampGain(float value) noexcept {
    return math::Clamp(value, 0.0f, audio::MaxVoiceGain);
}

float ClampPitch(float value) noexcept {
    return math::Clamp(value, audio::MinPitch, audio::MaxPitch);
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

void AudioDeviceContext::ValueRamp::StartFrom(float current, float value, uint64 frame,
                                              uint32 length) noexcept {
    from = current;
    target = value;
    startFrame = frame;
    frames = length;
}

AudioDeviceContext::AudioDeviceContext(Allocator& allocator,
                                       audio::AudioOutputDevice& device) noexcept
    : device_(device), mixer_(device.Mixer()), records_(allocator), voiceOwners_(allocator),
      clips_(allocator), releasing_(allocator), deferred_(allocator), awaitingStart_(allocator),
      terminalScratch_(allocator) {
}

AudioDeviceContext::~AudioDeviceContext() noexcept = default;

Result<void> AudioDeviceContext::Initialize(uint32 maxActiveVoices) noexcept {
    const uint32 mixerVoices = mixer_.Config().maxVoices;
    const uint32 mixerClips = mixer_.Config().maxClips;
    if (!records_.TryResize(MaxPlaybacks) || !voiceOwners_.TryResize(mixerVoices) ||
        !clips_.TryReserve(mixerClips) || !releasing_.TryReserve(mixerVoices) ||
        !deferred_.TryReserve(MaxPlaybacks) || !awaitingStart_.TryReserve(MaxPlaybacks) ||
        !terminalScratch_.TryResize(TerminalBatch)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (uint32& owner : voiceOwners_) {
        owner = 0;
    }

    maxActiveVoices_ = maxActiveVoices;
    return Result<void>::Success();
}

void AudioDeviceContext::Process(ControlBatch& batch) noexcept {
    const uint64 now = mixer_.CompletedMixFrame();
    RetryDeferred();
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
        }
    }

    ApplyMasterGain(batch.masterGain);
    PublishParams(now);
    CollectStarted(batch);
    CollectTerminals(batch);
    ReclaimVoices();
    ReclaimClips();
}

void AudioDeviceContext::StopAll(uint32 fadeFrames) noexcept {
    mixer_.StopAll(fadeFrames);
    RetryDeferred();
}

bool AudioDeviceContext::IsIdle() const noexcept {
    if (stats_.activeVoices != 0 || !releasing_.IsEmpty()) {
        return false;
    }

    for (const ClipEntry& entry : clips_) {
        if (entry.registered) {
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
        entry = ClipEntry{};
    }

    for (uint32& owner : voiceOwners_) {
        owner = 0;
    }

    releasing_.Clear();
    deferred_.Clear();
    awaitingStart_.Clear();
    stats_.activeVoices = 0;
}

AudioDeviceContextStats AudioDeviceContext::Stats() const noexcept {
    AudioDeviceContextStats stats = stats_;
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
    if (stats_.activeVoices >= maxActiveVoices_) {
        ++stats_.voiceLimitRejects;
        Emit(batch, command.handle, PlaybackEventKind::Ended, PlaybackEndReason::VoiceLimit,
             audio::UnsetMixFrame);
        return;
    }

    auto clip = AcquireClip(playable);
    if (!clip) {
        ++stats_.failedPlays;
        Emit(batch, command.handle, PlaybackEventKind::Ended, PlaybackEndReason::Failed,
             audio::UnsetMixFrame);
        return;
    }

    auto voice = mixer_.AcquireVoice();
    if (!voice) {
        // 믹서 표는 상한의 MixerVoiceHeadroomFactor배라 놓기 대기가 그 여유를 다 쓴 경우다.
        ReleaseClipUser(clip.Value());
        ++stats_.voiceLimitRejects;
        Emit(batch, command.handle, PlaybackEventKind::Ended, PlaybackEndReason::VoiceLimit,
             audio::UnsetMixFrame);
        return;
    }

    record.generation = command.handle.generation;
    record.sound = Move(command.sound);
    record.clip = clip.Value();
    record.voice = voice.Value();
    // fade-in은 0에서 옵션 volume까지의 사용자 ramp다(3.35.6절).
    const uint64 now = mixer_.CompletedMixFrame();
    if (command.fadeInFrames != 0) {
        record.volume.StartFrom(0.0f, command.volume, now, command.fadeInFrames);
    } else {
        record.volume.Set(command.volume);
    }

    record.pitch.Set(command.pitch);
    record.scene = command.scene;
    record.placement = command.placement;
    record.started = false;
    // ramp 중 재생(공간 재생은 AU3a2)은 묶음마다 다시 게시한다(PublishParams).
    record.paramsDirty = command.fadeInFrames != 0;
    record.pendingStopFade = NoPendingStop;
    record.stopRequested = false;
    record.stopReason = PlaybackEndReason::Stopped;
    auto play = mixer_.PreparePlay(record.voice, clips_[record.clip].id, command.startFrame,
                                   playable.looping, ParamsFor(record, now));
    if (!play) {
        // Reserved voice는 바로 놓을 수 있다.
        (void)mixer_.ReleaseVoice(record.voice);
        ReleaseClipUser(record.clip);
        record = Record{};
        ++stats_.failedPlays;
        Emit(batch, command.handle, PlaybackEventKind::Ended, PlaybackEndReason::Failed,
             audio::UnsetMixFrame);
        return;
    }

    record.play = play.Value();
    record.state = RecordState::Deferred;
    voiceOwners_[record.voice.index] = slot + 1;
    ++stats_.activeVoices;
    // 앞에 미룬 Play가 있으면 순서를 지키려고 뒤에 붙인다.
    if (!deferred_.IsEmpty() || !TrySubmitPlay(slot)) {
        ROCKS_ASSERT(deferred_.Size() < deferred_.Capacity());
        (void)deferred_.TryPushBack(slot);
        ++stats_.deferredPlays;
    }
}

void AudioDeviceContext::ApplyStop(const ControlCommand& command, ControlBatch& batch) noexcept {
    (void)batch;
    if (Record* record = FindRecord(command.handle)) {
        RequestStop(*record, command.fadeFrames, PlaybackEndReason::Stopped);
    }
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
    }
}

void AudioDeviceContext::PublishParams(uint64 now) noexcept {
    for (Record& record : records_) {
        if (record.state != RecordState::Submitted || !record.paramsDirty) {
            continue;
        }

        (void)mixer_.PublishParams(record.voice, ParamsFor(record, now));
        // ramp가 끝날 때까지 묶음마다 중간값을 다시 낸다.
        record.paramsDirty = record.volume.IsRamping(now) || record.pitch.IsRamping(now);
    }
}

void AudioDeviceContext::CollectStarted(ControlBatch& batch) noexcept {
    usize index = 0;
    while (index < awaitingStart_.Size()) {
        const uint32 slot = awaitingStart_[index];
        Record& record = records_[slot];
        auto status = mixer_.QueryVoice(record.voice);
        if (status && status.Value().startMixFrame != audio::UnsetMixFrame) {
            record.started = true;
            Emit(batch, PlaybackHandle{slot, record.generation}, PlaybackEventKind::Started,
                 PlaybackEndReason::None, status.Value().startMixFrame);
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
                continue;
            }

            const uint32 slot = owner - 1;
            Record& record = records_[slot];
            if (record.voice.generation != terminal.voice.generation) {
                continue;
            }

            const PlaybackHandle handle{slot, record.generation};
            if (!record.started) {
                // 같은 묶음에서 시작과 끝이 함께 보이면 Started를 먼저 낸다.
                auto status = mixer_.QueryVoice(record.voice);
                if (status && status.Value().startMixFrame != audio::UnsetMixFrame) {
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
                reason = PlaybackEndReason::Failed;
                break;
            }

            Emit(batch, handle, PlaybackEventKind::Ended, reason, terminal.endMixFrame);
            ROCKS_ASSERT(releasing_.Size() < releasing_.Capacity());
            (void)releasing_.TryPushBack(ReleasingVoice{record.voice, record.clip});
            voiceOwners_[terminal.voice.index] = 0;
            --stats_.activeVoices;
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
            entry = ClipEntry{};
        }
    }
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
    params.gain = ClampGain(record.volume.At(frame) * playable.volume);
    params.pitch = ClampPitch(record.pitch.At(frame) * playable.pitch);
    params.panMode = audio::AudioPanMode::Direct;
    return params;
}

Result<uint32> AudioDeviceContext::AcquireClip(const SoundPlayable& playable) noexcept {
    const SoundPcm* pcm = playable.pcm.Get();
    usize freeEntry = clips_.Size();
    for (usize index = 0; index < clips_.Size(); ++index) {
        ClipEntry& entry = clips_[index];
        if (!entry.registered) {
            if (freeEntry == clips_.Size()) {
                freeEntry = index;
            }

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
