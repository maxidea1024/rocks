#include <Audio/AudioMixer.h>

#include <Audio/Panner.h>
#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>
#include <Core/Move.h>
#include <Platform/FloatingPointEnvironment.h>

namespace rocks::audio {

namespace {

// OutputFramesBefore는 MaxBlockFrames에서 멈추므로(그 이상이라는 뜻) 블록 크기가 그보다 작아야 끝
// 판정이 맞다.
static_assert(MaxMixBlockFrames < MaxBlockFrames,
              "Mix blocks must be shorter than the resampler count limit");

// Direct 패닝의 mono → stereo 이득(UE EMonoChannelUpmixMethod::Linear 기본값)과 stereo → mono 이득
// (UE ToMonoMatrix의 FL·FR)이다.
constexpr float MonoUpmixLinearGain = 0.5f;
constexpr float StereoToMonoGain = 0.707f;

bool IsValidBlockFrames(uint32 frames) noexcept {
    return frames >= MinMixBlockFrames && frames <= MaxMixBlockFrames &&
           (frames & (frames - 1)) == 0;
}

bool IsValidParams(const AudioVoiceParams& params) noexcept {
    return math::IsFinite(params.gain) && params.gain >= 0.0f && params.gain <= MaxVoiceGain &&
           math::IsFinite(params.pitch) && params.pitch >= MinPitch && params.pitch <= MaxPitch &&
           static_cast<uint32>(params.panMode) <= static_cast<uint32>(AudioPanMode::Azimuth) &&
           math::IsFinite(params.azimuthDegrees) && math::IsFinite(params.nonSpatializedAmount) &&
           params.nonSpatializedAmount >= 0.0f && params.nonSpatializedAmount <= 1.0f &&
           math::IsFinite(params.lowPassCutoffHz) && params.lowPassCutoffHz >= 0.0f &&
           math::IsFinite(params.highPassCutoffHz) && params.highPassCutoffHz >= 0.0f;
}

// 범위 밖 값을 자르거나 기본값으로 바꾸고 바꾼 수를 센다(실시간 경로는 실패를 돌려줄 곳이 없다).
float ClampCounted(float value, float minimum, float maximum, float fallback,
                   uint64& counter) noexcept {
    if (!math::IsFinite(value)) {
        ++counter;
        return fallback;
    }

    if (value < minimum || value > maximum) {
        ++counter;
        return math::Clamp(value, minimum, maximum);
    }

    return value;
}

uint32 NextGeneration(uint32 generation) noexcept {
    // 0은 무효 handle이라 건너뛴다.
    return generation + 1 == 0 ? 1 : generation + 1;
}

// loop 구간의 [시작, 끝)이다. 0/0이면 clip 전체다.
void LoopRange(const AudioClipDesc& desc, uint32& begin, uint32& end) noexcept {
    if (desc.loopBegin == 0 && desc.loopEnd == 0) {
        begin = 0;
        end = desc.frameCount;
        return;
    }

    begin = desc.loopBegin;
    end = desc.loopEnd;
}

PlanarView WithChannels(PlanarView view, uint32 channels) noexcept {
    view.channelCount = channels;
    return view;
}

} // namespace

AudioMixer::AudioMixer(const AudioMixerConfig& config, Allocator& allocator) noexcept
    : config_(config), voices_(allocator), clips_(allocator) {
}

Result<AudioMixer> AudioMixer::TryCreate(const AudioMixerConfig& config,
                                         Allocator& allocator) noexcept {
    if (!IsValidMixSampleRate(config.sampleRate) ||
        (config.layout != ChannelLayout::Mono && config.layout != ChannelLayout::Stereo) ||
        !IsValidBlockFrames(config.blockFrames) || config.maxVoices == 0 ||
        config.maxVoices > MaxMixerVoices || config.maxClips == 0 ||
        config.maxClips > MaxMixerClips) {
        return Result<AudioMixer>::Failure(ErrorCode::InvalidArgument);
    }

    const uint32 outputChannels = ChannelCount(config.layout);
    AudioMixer mixer(config, allocator);
    auto limiter = DynamicsProcessor::TryCreate(config.sampleRate, outputChannels,
                                                config.masterLimiter, allocator);
    if (!limiter) {
        return Result<AudioMixer>::Failure(limiter.Error());
    }

    mixer.limiter_ = Move(limiter.Value());
    if (auto resized = mixer.voices_.TryResize(config.maxVoices); !resized) {
        return Result<AudioMixer>::Failure(resized.Error());
    }

    if (auto resized = mixer.clips_.TryResize(config.maxClips); !resized) {
        return Result<AudioMixer>::Failure(resized.Error());
    }

    auto input = PlanarBuffer::TryCreate(MaxClipChannels, MaxBlockFrames, allocator);
    if (!input) {
        return Result<AudioMixer>::Failure(input.Error());
    }

    auto voice = PlanarBuffer::TryCreate(MaxClipChannels, config.blockFrames, allocator);
    if (!voice) {
        return Result<AudioMixer>::Failure(voice.Error());
    }

    auto master = PlanarBuffer::TryCreate(outputChannels, config.blockFrames, allocator);
    if (!master) {
        return Result<AudioMixer>::Failure(master.Error());
    }

    mixer.inputScratch_ = Move(input.Value());
    mixer.voiceScratch_ = Move(voice.Value());
    mixer.master_ = Move(master.Value());
    return Result<AudioMixer>::Success(Move(mixer));
}

// --- clip 표 ---

Result<AudioClipId> AudioMixer::RegisterClip(const AudioClipDesc& desc) noexcept {
    if (desc.channelCount == 0 || desc.channelCount > MaxClipChannels || desc.frameCount == 0 ||
        desc.sampleRate < MinClipSampleRate || desc.sampleRate > MaxClipSampleRate) {
        return Result<AudioClipId>::Failure(ErrorCode::InvalidArgument);
    }

    const bool wholeLoop = desc.loopBegin == 0 && desc.loopEnd == 0;
    if (!wholeLoop && !(desc.loopBegin < desc.loopEnd && desc.loopEnd <= desc.frameCount)) {
        return Result<AudioClipId>::Failure(ErrorCode::InvalidArgument);
    }

    // NaN 하나가 필터 상태를 영구 NaN으로 만들고 마스터 합 전체를 지우므로 등록 때 거른다.
    for (uint32 channel = 0; channel < desc.channelCount; ++channel) {
        const float* samples = desc.channels[channel];
        if (samples == nullptr) {
            return Result<AudioClipId>::Failure(ErrorCode::InvalidArgument);
        }

        for (uint32 frame = 0; frame < desc.frameCount; ++frame) {
            if (!math::IsFinite(samples[frame])) {
                return Result<AudioClipId>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    for (uint32 index = 0; index < clips_.Size(); ++index) {
        ClipSlot& slot = clips_[index];
        if (!slot.registered) {
            slot.desc = desc;
            slot.registered = true;
            slot.users = 0;
            return Result<AudioClipId>::Success(AudioClipId{index, slot.generation});
        }
    }

    return Result<AudioClipId>::Failure(ErrorCode::CapacityExceeded);
}

Result<void> AudioMixer::UnregisterClip(AudioClipId clip) noexcept {
    if (clip.index >= clips_.Size() || !clips_[clip.index].registered ||
        clips_[clip.index].generation != clip.generation) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    ClipSlot& slot = clips_[clip.index];
    if (slot.users > 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    slot.registered = false;
    slot.desc = AudioClipDesc{};
    slot.generation = NextGeneration(slot.generation);
    return Result<void>::Success();
}

// --- voice ---

AudioMixer::VoiceSlot* AudioMixer::FindVoice(AudioVoiceHandle voice) noexcept {
    if (voice.index >= voices_.Size()) {
        return nullptr;
    }

    VoiceSlot& slot = voices_[voice.index];
    return slot.state != AudioVoiceState::Idle && slot.generation == voice.generation ? &slot
                                                                                      : nullptr;
}

const AudioMixer::VoiceSlot* AudioMixer::FindVoice(AudioVoiceHandle voice) const noexcept {
    if (voice.index >= voices_.Size()) {
        return nullptr;
    }

    const VoiceSlot& slot = voices_[voice.index];
    return slot.state != AudioVoiceState::Idle && slot.generation == voice.generation ? &slot
                                                                                      : nullptr;
}

Result<AudioVoiceHandle> AudioMixer::AcquireVoice() noexcept {
    const uint32 count = static_cast<uint32>(voices_.Size());
    for (uint32 step = 0; step < count; ++step) {
        const uint32 index = (nextVoiceSearch_ + step) % count;
        VoiceSlot& slot = voices_[index];
        if (slot.state == AudioVoiceState::Idle) {
            slot.state = AudioVoiceState::Reserved;
            slot.collected = false;
            slot.hasPendingParams = false;
            slot.startMixFrame = UnsetMixFrame;
            slot.endMixFrame = UnsetMixFrame;
            slot.endReason = AudioVoiceEndReason::None;
            nextVoiceSearch_ = (index + 1) % count;
            return Result<AudioVoiceHandle>::Success(AudioVoiceHandle{index, slot.generation});
        }
    }

    return Result<AudioVoiceHandle>::Failure(ErrorCode::CapacityExceeded);
}

Result<AudioRtCommand> AudioMixer::PreparePlay(AudioVoiceHandle voice, AudioClipId clip,
                                               uint32 startFrame, bool loop,
                                               const AudioVoiceParams& params) noexcept {
    VoiceSlot* slot = FindVoice(voice);
    if (slot == nullptr || clip.index >= clips_.Size() || !clips_[clip.index].registered ||
        clips_[clip.index].generation != clip.generation) {
        return Result<AudioRtCommand>::Failure(ErrorCode::StaleHandle);
    }

    const AudioClipDesc& desc = clips_[clip.index].desc;
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
    LoopRange(desc, loopBegin, loopEnd);
    // 방위각 패닝은 mono clip만이다(stereo spread는 AU3).
    const bool azimuthOnStereo = params.panMode == AudioPanMode::Azimuth && desc.channelCount != 1;
    if (slot->state != AudioVoiceState::Reserved || startFrame >= desc.frameCount ||
        (loop && startFrame >= loopEnd) || !IsValidParams(params) || azimuthOnStereo) {
        return Result<AudioRtCommand>::Failure(ErrorCode::InvalidArgument);
    }

    // clip을 voice에 묶고 사용 수를 올린다. ReleaseVoice가 내린다(해제된 PCM을 읽지 않게).
    ++clips_[clip.index].users;
    slot->clipIndex = clip.index;
    slot->startFrame = startFrame;
    slot->loop = loop;
    slot->state = AudioVoiceState::Prepared;

    AudioRtCommand command;
    command.type = AudioRtCommandType::Play;
    command.voice = voice;
    command.params = params;
    return Result<AudioRtCommand>::Success(command);
}

Result<void> AudioMixer::ReleaseVoice(AudioVoiceHandle voice) noexcept {
    VoiceSlot* slot = FindVoice(voice);
    if (slot == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const bool reserved = slot->state == AudioVoiceState::Reserved;
    const bool collectedFinish = slot->state == AudioVoiceState::Finished && slot->collected;
    if (!reserved && !collectedFinish) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // Finished는 PreparePlay를 거쳤으므로 clip이 묶여 있다.
    if (collectedFinish) {
        ROCKS_ASSERT(clips_[slot->clipIndex].users > 0);
        --clips_[slot->clipIndex].users;
    }

    slot->state = AudioVoiceState::Idle;
    slot->generation = NextGeneration(slot->generation);
    return Result<void>::Success();
}

Result<AudioVoiceStatus> AudioMixer::QueryVoice(AudioVoiceHandle voice) const noexcept {
    const VoiceSlot* slot = FindVoice(voice);
    if (slot == nullptr) {
        return Result<AudioVoiceStatus>::Failure(ErrorCode::StaleHandle);
    }

    AudioVoiceStatus status;
    status.state = slot->state;
    status.startMixFrame = slot->startMixFrame;
    status.endMixFrame = slot->endMixFrame;
    status.endReason = slot->endReason;
    return Result<AudioVoiceStatus>::Success(status);
}

uint32 AudioMixer::CollectFinished(Span<AudioVoiceTerminal> out) noexcept {
    uint32 written = 0;
    for (uint32 index = 0; index < voices_.Size() && written < out.Size(); ++index) {
        VoiceSlot& slot = voices_[index];
        if (slot.state == AudioVoiceState::Finished && !slot.collected) {
            out[written].voice = AudioVoiceHandle{index, slot.generation};
            out[written].reason = slot.endReason;
            out[written].endMixFrame = slot.endMixFrame;
            slot.collected = true;
            ++written;
        }
    }

    return written;
}

// --- 명령 적용 ---

void AudioMixer::Finish(VoiceSlot& slot, AudioVoiceEndReason reason, uint64 frame) noexcept {
    slot.state = AudioVoiceState::Finished;
    slot.endReason = reason;
    slot.endMixFrame = frame;
    slot.collected = false;
}

void AudioMixer::ApplyParams(VoiceSlot& slot, const AudioVoiceParams& requested, uint32 rampFrames,
                             bool immediate) noexcept {
    const AudioClipDesc& desc = clips_[slot.clipIndex].desc;
    uint64& clamped = counters_.clampedParams;
    AudioVoiceParams params = requested;
    const AudioVoiceParams defaults;
    params.gain = ClampCounted(params.gain, 0.0f, MaxVoiceGain, defaults.gain, clamped);
    params.pitch = ClampCounted(params.pitch, MinPitch, MaxPitch, defaults.pitch, clamped);
    params.azimuthDegrees =
        ClampCounted(params.azimuthDegrees, -NumericLimits<float>::Max(),
                     NumericLimits<float>::Max(), defaults.azimuthDegrees, clamped);
    params.nonSpatializedAmount = ClampCounted(params.nonSpatializedAmount, 0.0f, 1.0f,
                                               defaults.nonSpatializedAmount, clamped);
    params.lowPassCutoffHz = ClampCounted(params.lowPassCutoffHz, 0.0f, NumericLimits<float>::Max(),
                                          defaults.lowPassCutoffHz, clamped);
    params.highPassCutoffHz =
        ClampCounted(params.highPassCutoffHz, 0.0f, NumericLimits<float>::Max(),
                     defaults.highPassCutoffHz, clamped);
    if (static_cast<uint32>(params.panMode) > static_cast<uint32>(AudioPanMode::Azimuth) ||
        (params.panMode == AudioPanMode::Azimuth && desc.channelCount != 1)) {
        // 방위각 패닝은 mono clip만이다(stereo spread는 AU3).
        ++clamped;
        params.panMode = AudioPanMode::Direct;
    }

    const uint32 ramp =
        immediate ? 0u : (rampFrames < MaxRatioInterpFrames ? rampFrames : MaxRatioInterpFrames);
    if (immediate) {
        slot.gain.Reset(params.gain);
    } else {
        slot.gain.SetTarget(params.gain, ramp);
    }

    double ratio = static_cast<double>(params.pitch) * static_cast<double>(desc.sampleRate) /
                   static_cast<double>(config_.sampleRate);
    if (ratio < MinFrameRatio || ratio > MaxFrameRatio) {
        ++clamped;
        ratio = math::Clamp(ratio, MinFrameRatio, MaxFrameRatio);
    }

    // 값은 위에서 범위 안으로 맞췄으므로 실패하지 않는다.
    const bool ratioSet = static_cast<bool>(slot.resampler.SetFrameRatio(ratio, ramp));
    const bool lowSet = static_cast<bool>(slot.lowPass.SetCutoff(params.lowPassCutoffHz, ramp));
    const bool highSet = static_cast<bool>(slot.highPass.SetCutoff(params.highPassCutoffHz, ramp));
    ROCKS_ASSERT(ratioSet && lowSet && highSet);
    (void)ratioSet;
    (void)lowSet;
    (void)highSet;

    // [원본 채널][출력 채널] 목표 이득이다.
    float targets[MaxClipChannels][MaxClipChannels] = {};
    const uint32 outputs = OutputChannelCount();
    if (params.panMode == AudioPanMode::Azimuth) {
        const Result<void> computed =
            ComputeChannelGains(config_.layout, params.azimuthDegrees, params.nonSpatializedAmount,
                                0.0f, Span<float>(targets[0], MaxClipChannels));
        ROCKS_ASSERT(computed);
        (void)computed;
    } else if (desc.channelCount == 1) {
        targets[0][0] = outputs == 1 ? 1.0f : MonoUpmixLinearGain;
        targets[0][1] = outputs == 1 ? 0.0f : MonoUpmixLinearGain;
    } else if (outputs == 1) {
        targets[0][0] = StereoToMonoGain;
        targets[1][0] = StereoToMonoGain;
    } else {
        targets[0][0] = 1.0f;
        targets[1][1] = 1.0f;
    }

    for (uint32 source = 0; source < MaxClipChannels; ++source) {
        for (uint32 output = 0; output < MaxClipChannels; ++output) {
            if (immediate) {
                slot.channelGains[source][output].Reset(targets[source][output]);
            } else {
                slot.channelGains[source][output].SetTarget(targets[source][output], ramp);
            }
        }
    }

    slot.params = params;
}

void AudioMixer::StartVoice(VoiceSlot& slot, const AudioRtCommand& command, uint64 frame) noexcept {
    const AudioClipDesc& desc = clips_[slot.clipIndex].desc;
    // 같은 voice에 Play보다 뒤 sequence로 저장된 SetParams가 있으면 그것이 이긴다.
    const AudioVoiceParams& params =
        slot.hasPendingParams && slot.pendingParamsSequence > command.sequence ? slot.params
                                                                               : command.params;
    const AudioVoiceParams initial = params;

    // DSP 상태는 clip 채널 수로 다시 만든다(할당 없음). 인자는 늘 유효하다.
    auto resampler = LinearResampler::TryCreate(desc.channelCount, 1.0);
    auto lowPass = OnePoleLowPass::TryCreate(config_.sampleRate, desc.channelCount);
    auto highPass = OnePoleHighPass::TryCreate(config_.sampleRate, desc.channelCount);
    ROCKS_ASSERT(resampler && lowPass && highPass);
    slot.resampler = resampler.Value();
    slot.lowPass = lowPass.Value();
    slot.highPass = highPass.Value();
    slot.fade.Reset(1.0f);
    slot.sourceCursor = slot.startFrame;
    ApplyParams(slot, initial, 0, true);
    slot.hasPendingParams = false;
    slot.state = AudioVoiceState::Playing;
    slot.startMixFrame = frame;
}

void AudioMixer::ApplyCommand(const AudioRtCommand& command, uint64 frame) noexcept {
    if (command.type == AudioRtCommandType::SetMasterGain) {
        const float gain = ClampCounted(command.masterGain, 0.0f, MaxMasterGain, DefaultMasterGain,
                                        counters_.clampedParams);
        masterGain_.SetTarget(gain, command.rampFrames);
        return;
    }

    VoiceSlot* slot = FindVoice(command.voice);
    if (slot == nullptr) {
        ++counters_.staleCommands;
        return;
    }

    switch (command.type) {
    case AudioRtCommandType::Play:
        if (slot->state == AudioVoiceState::Prepared) {
            StartVoice(*slot, command, frame);
            return;
        }

        break;
    case AudioRtCommandType::Stop:
        if (slot->state == AudioVoiceState::Prepared) {
            // 시작 전 Stop은 fade 없이 바로 끝낸다(UE StopInternal). 뒤에 오는 Play는 무시된다.
            Finish(*slot, AudioVoiceEndReason::Stopped, frame);
            return;
        }

        if (slot->state == AudioVoiceState::Playing) {
            if (command.fadeFrames == 0) {
                Finish(*slot, AudioVoiceEndReason::Stopped, frame);
            } else {
                slot->state = AudioVoiceState::Stopping;
                slot->fade.SetTarget(0.0f, command.fadeFrames);
            }

            return;
        }

        break;
    case AudioRtCommandType::SetParams:
        if (slot->state == AudioVoiceState::Prepared) {
            if (!slot->hasPendingParams || command.sequence > slot->pendingParamsSequence) {
                slot->params = command.params;
                slot->pendingParamsSequence = command.sequence;
                slot->hasPendingParams = true;
                return;
            }

            break;
        }

        if (slot->state == AudioVoiceState::Playing || slot->state == AudioVoiceState::Stopping) {
            ApplyParams(*slot, command.params, command.rampFrames, false);
            return;
        }

        break;
    case AudioRtCommandType::SetMasterGain:
        break;
    }

    ++counters_.ignoredCommands[static_cast<uint32>(command.type)];
}

// --- 렌더 ---

RenderBlockResult AudioMixer::RenderBlock(Span<const AudioRtCommand> commands,
                                          Span<float> interleavedOut) noexcept {
    ROCKS_ASSERT(platform::IsRealtimeAudioFloatingPointEnvironment());
    const uint32 blockFrames = config_.blockFrames;
    ROCKS_ASSERT(interleavedOut.Size() == static_cast<usize>(blockFrames) * OutputChannelCount());
    master_.Clear();

    const uint64 blockStart = mixFrame_;
    const uint64 blockEnd = blockStart + blockFrames;
    uint32 segmentStart = 0;
    uint32 segmentCount = 1;
    usize index = 0;
    for (; index < commands.Size(); ++index) {
        const AudioRtCommand& command = commands[index];
        ROCKS_ASSERT(command.mixFrame != ImmediateMixFrame);
        ROCKS_ASSERT(index == 0 || commands[index - 1].mixFrame < command.mixFrame ||
                     (commands[index - 1].mixFrame == command.mixFrame &&
                      commands[index - 1].sequence < command.sequence));
        if (command.mixFrame >= blockEnd) {
            break;
        }

        uint32 offset = 0;
        if (command.mixFrame < blockStart) {
            ++counters_.lateCommands;
            counters_.lateFrames += blockStart - command.mixFrame;
        } else {
            offset = static_cast<uint32>(command.mixFrame - blockStart);
        }

        if (offset != segmentStart) {
            if (segmentCount == MaxBlockSegments) {
                // 구간 상한: 이 명령부터 블록 안의 나머지는 다음 블록으로 미룬다.
                for (usize rest = index;
                     rest < commands.Size() && commands[rest].mixFrame < blockEnd; ++rest) {
                    ++counters_.deferredCommands;
                }

                break;
            }

            RenderSegment(segmentStart, offset - segmentStart);
            segmentStart = offset;
            ++segmentCount;
        }

        ApplyCommand(command, blockStart + offset);
    }

    RenderSegment(segmentStart, blockFrames - segmentStart);
    FinishMaster(interleavedOut);
    mixFrame_ = blockEnd;
    RenderBlockResult result;
    result.consumedCommands = static_cast<uint32>(index);
    return result;
}

void AudioMixer::RenderSegment(uint32 offset, uint32 frames) noexcept {
    if (frames == 0) {
        return;
    }

    for (uint32 index = 0; index < voices_.Size(); ++index) {
        VoiceSlot& slot = voices_[index];
        if (slot.state == AudioVoiceState::Playing || slot.state == AudioVoiceState::Stopping) {
            RenderVoice(slot, offset, frames);
        }
    }

    // 마스터 gain은 구간마다 건다. 블록 중간의 SetMasterGain이 앞 구간을 바꾸지 않게 한다.
    masterGain_.Apply(master_.View().Slice(offset, frames));
}

uint32 AudioMixer::FillInput(VoiceSlot& slot, uint32 frames) noexcept {
    const AudioClipDesc& desc = clips_[slot.clipIndex].desc;
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
    LoopRange(desc, loopBegin, loopEnd);
    for (uint32 channel = 0; channel < desc.channelCount; ++channel) {
        const float* source = desc.channels[channel];
        float* target = inputScratch_.Channel(channel);
        uint64 cursor = slot.sourceCursor;
        for (uint32 frame = 0; frame < frames; ++frame) {
            if (slot.loop) {
                target[frame] = source[cursor];
                ++cursor;
                if (cursor >= loopEnd) {
                    cursor = loopBegin;
                }
            } else {
                // 끝 뒤는 0 꼬리다(3.22.5절).
                target[frame] = cursor < desc.frameCount ? source[cursor] : 0.0f;
                ++cursor;
            }
        }
    }

    return frames;
}

void AudioMixer::AdvanceCursor(VoiceSlot& slot, uint32 consumed) noexcept {
    slot.sourceCursor += consumed;
    if (!slot.loop) {
        return;
    }

    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
    LoopRange(clips_[slot.clipIndex].desc, loopBegin, loopEnd);
    if (slot.sourceCursor >= loopEnd) {
        slot.sourceCursor = loopBegin + (slot.sourceCursor - loopEnd) % (loopEnd - loopBegin);
    }
}

void AudioMixer::RenderVoice(VoiceSlot& slot, uint32 offset, uint32 frames) noexcept {
    const AudioClipDesc& desc = clips_[slot.clipIndex].desc;
    const uint32 sourceChannels = desc.channelCount;
    const uint32 outputs = OutputChannelCount();
    const uint64 segmentStartFrame = mixFrame_ + offset;
    uint32 produced = 0;
    while (produced < frames) {
        uint32 chunk = frames - produced;
        // 요구 입력이 scratch를 넘으면 출력을 줄인다(pitch·rate 비율이 크면).
        uint32 needed = slot.resampler.InputFramesNeeded(chunk).Value();
        while (needed > MaxBlockFrames && chunk > 1) {
            chunk /= 2;
            needed = slot.resampler.InputFramesNeeded(chunk).Value();
        }

        // 끝 판정(3.22.5절): 자연 끝은 다음 출력 위치가 원본 끝 이상이 되는 frame, fade 끝은 fade가
        // 0에 닿는 frame이다. 같은 frame이면 자연 끝이 이긴다.
        AudioVoiceEndReason ending = AudioVoiceEndReason::None;
        if (!slot.loop) {
            const int64 remaining =
                static_cast<int64>(desc.frameCount) - static_cast<int64>(slot.sourceCursor);
            const uint32 before = slot.resampler.OutputFramesBefore(remaining);
            if (before <= chunk) {
                chunk = before;
                ending = AudioVoiceEndReason::NaturalEnd;
            }
        }

        if (slot.state == AudioVoiceState::Stopping) {
            const uint32 fadeRemaining = slot.fade.RemainingFrames();
            if (fadeRemaining < chunk ||
                (fadeRemaining == chunk && ending == AudioVoiceEndReason::None)) {
                chunk = fadeRemaining;
                ending = AudioVoiceEndReason::Stopped;
            }
        }

        if (chunk > 0) {
            needed = slot.resampler.InputFramesNeeded(chunk).Value();
            FillInput(slot, needed);
            const PlanarView input = WithChannels(inputScratch_.View(needed), sourceChannels);
            const PlanarView voice = WithChannels(voiceScratch_.View(chunk), sourceChannels);
            const uint32 consumed = slot.resampler.Process(input, voice).Value();
            AdvanceCursor(slot, consumed);
            slot.lowPass.Process(voice);
            slot.highPass.Process(voice);
            slot.gain.Apply(voice);
            slot.fade.Apply(voice);
            for (uint32 source = 0; source < sourceChannels; ++source) {
                for (uint32 output = 0; output < outputs; ++output) {
                    slot.channelGains[source][output].MixAdd(
                        voice.channels[source], master_.Channel(output) + offset + produced, chunk);
                }
            }

            produced += chunk;
        }

        if (ending != AudioVoiceEndReason::None) {
            Finish(slot, ending, segmentStartFrame + produced);
            return;
        }
    }
}

void AudioMixer::FinishMaster(Span<float> interleavedOut) noexcept {
    const PlanarView view = master_.View();
    const uint32 channels = view.channelCount;
    // 비유한 표본은 limiter 앞에서 0으로 바꾸고 센다(limiter 상태를 오염시키지 않게).
    for (uint32 channel = 0; channel < channels; ++channel) {
        float* samples = view.channels[channel];
        for (uint32 frame = 0; frame < view.frameCount; ++frame) {
            if (!math::IsFinite(samples[frame])) {
                ++counters_.nonFiniteSamples;
                samples[frame] = 0.0f;
            }
        }
    }

    if (config_.masterLimiterEnabled) {
        limiter_.Process(view);
    }

    // [−1, 1] 밖은 잘라 센다(UE도 출력에서 자른다 — AU2 int16 변환이 넘치지 않게).
    for (uint32 channel = 0; channel < channels; ++channel) {
        const float* samples = view.channels[channel];
        for (uint32 frame = 0; frame < view.frameCount; ++frame) {
            float value = samples[frame];
            if (value > 1.0f || value < -1.0f) {
                ++counters_.clippedSamples;
                value = value > 1.0f ? 1.0f : -1.0f;
            }

            interleavedOut[static_cast<usize>(frame) * channels + channel] = value;
        }
    }
}

} // namespace rocks::audio
