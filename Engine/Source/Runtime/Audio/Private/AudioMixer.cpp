#include <Audio/AudioMixer.h>

#include <Audio/AudioStreamRing.h>
#include <Audio/Panner.h>
#include <Core/Assert.h>
#include <Core/Math/MathUtility.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include <Platform/Sync/RealtimeWakeSignal.h>

namespace rocks::audio {

using platform::AtomicLoad;
using platform::AtomicStore;
using platform::AtomicThreadFence;
using platform::MemoryOrder;

namespace {

// OutputFramesBefore는 MaxBlockFrames에서 멈추므로(그 이상이라는 뜻) 블록 크기가 그보다 작아야 끝
// 판정이 맞다.
static_assert(MaxMixBlockFrames < MaxBlockFrames,
              "Mix blocks must be shorter than the resampler count limit");

// Direct 패닝의 mono → stereo 이득(UE EMonoChannelUpmixMethod::Linear 기본값)과 stereo → mono 이득
// (UE ToMonoMatrix의 FL·FR)이다.
constexpr float MonoUpmixLinearGain = 0.5f;
constexpr float StereoToMonoGain = 0.707f;

// 스트림 refill 수위 = 링 용량 / 이 값이다(3.40.1절 — 절반).
constexpr uint32 StreamRefillDivisor = 2;

// 64비트 원자 word의 상위 32비트 자리와 하위 mask다(mailbox·stop-all·terminal 묶음).
constexpr uint32 HighWordShift = 32;
constexpr uint64 LowWordMask = NumericLimits<uint32>::Max();
constexpr uint64 ReasonMask = NumericLimits<uint8>::Max();

// seqlock 내용 word 색인이다.
enum SeqlockWord : uint32 {
    SeqlockGeneration,
    SeqlockSequenceLow,
    SeqlockSequenceHigh,
    SeqlockGain,
    SeqlockPitch,
    SeqlockPanMode,
    SeqlockAzimuth,
    SeqlockNonSpatialized,
    SeqlockLowPass,
    SeqlockHighPass,
    SeqlockWordCount
};

// seqlock version의 홀수 bit(쓰는 중)다.
constexpr uint32 SeqlockWritingBit = 1;

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

// generation 차이(부호 있음)다. 양수면 a가 b보다 새롭다(3.23.1절 — 2^31 한계).
int32 GenerationDelta(uint32 a, uint32 b) noexcept {
    return static_cast<int32>(a - b);
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

uint32 FloatBits(float value) noexcept {
    return platform::BitCast<uint32>(value);
}

float BitsFloat(uint32 bits) noexcept {
    return platform::BitCast<float>(bits);
}

} // namespace

// SharedVoice의 seqlock 내용 칸과 계수 스냅샷 칸이 정의와 어긋나지 않게 묶는다.
static_assert(sizeof(AudioMixerCounters) % sizeof(uint64) == 0,
              "AudioMixerCounters must hold only uint64 fields");

AudioMixer::AudioMixer(const AudioMixerConfig& config, Allocator& allocator) noexcept
    : config_(config), controlVoices_(allocator), clips_(allocator), sharedVoices_(allocator),
      voices_(allocator) {
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

    if (auto resized = mixer.controlVoices_.TryResize(config.maxVoices); !resized) {
        return Result<AudioMixer>::Failure(resized.Error());
    }

    if (auto resized = mixer.sharedVoices_.TryResize(config.maxVoices); !resized) {
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

// --- 제어 쪽: clip 표 ---

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

Result<AudioClipId> AudioMixer::RegisterStreamClip(const AudioStreamClipDesc& desc) noexcept {
    if (desc.ring == nullptr || desc.channelCount == 0 || desc.channelCount > MaxClipChannels ||
        desc.ring->ChannelCount() != desc.channelCount || desc.frameCount == 0 ||
        desc.sampleRate < MinClipSampleRate || desc.sampleRate > MaxClipSampleRate) {
        return Result<AudioClipId>::Failure(ErrorCode::InvalidArgument);
    }

    AudioClipDesc clip;
    clip.channelCount = desc.channelCount;
    clip.frameCount = desc.frameCount;
    clip.sampleRate = desc.sampleRate;
    clip.loopBegin = desc.loopBegin;
    clip.loopEnd = desc.loopEnd;
    const bool wholeLoop = clip.loopBegin == 0 && clip.loopEnd == 0;
    if (!wholeLoop && !(clip.loopBegin < clip.loopEnd && clip.loopEnd <= clip.frameCount)) {
        return Result<AudioClipId>::Failure(ErrorCode::InvalidArgument);
    }

    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
    LoopRange(clip, loopBegin, loopEnd);
    if (desc.startFrame >= clip.frameCount || (desc.loop && desc.startFrame >= loopEnd)) {
        return Result<AudioClipId>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < clips_.Size(); ++index) {
        ClipSlot& slot = clips_[index];
        if (!slot.registered) {
            slot.desc = clip;
            slot.registered = true;
            slot.users = 0;
            slot.stream = desc.ring;
            slot.streamStartFrame = desc.startFrame;
            slot.streamLoop = desc.loop;
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
    slot.stream = nullptr;
    slot.streamStartFrame = 0;
    slot.streamLoop = false;
    slot.generation = NextGeneration(slot.generation);
    return Result<void>::Success();
}

// --- 제어 쪽: voice ---

AudioMixer::ControlVoice* AudioMixer::FindControl(AudioVoiceHandle voice) noexcept {
    if (voice.index >= controlVoices_.Size()) {
        return nullptr;
    }

    ControlVoice& slot = controlVoices_[voice.index];
    return slot.state != ControlState::Idle && slot.generation == voice.generation ? &slot
                                                                                   : nullptr;
}

const AudioMixer::ControlVoice* AudioMixer::FindControl(AudioVoiceHandle voice) const noexcept {
    if (voice.index >= controlVoices_.Size()) {
        return nullptr;
    }

    const ControlVoice& slot = controlVoices_[voice.index];
    return slot.state != ControlState::Idle && slot.generation == voice.generation ? &slot
                                                                                   : nullptr;
}

Result<void> AudioMixer::CheckCommandTarget(AudioVoiceHandle voice) const noexcept {
    const ControlVoice* slot = FindControl(voice);
    if (slot == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    // Play를 만들지 않은 voice에는 명령을 낼 수 없다(3.23.1절 — RT가 첫 명령으로 수명을 안다).
    if (slot->state == ControlState::Reserved) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<AudioVoiceHandle> AudioMixer::AcquireVoice() noexcept {
    const uint32 count = static_cast<uint32>(controlVoices_.Size());
    for (uint32 step = 0; step < count; ++step) {
        const uint32 index = (nextVoiceSearch_ + step) % count;
        ControlVoice& slot = controlVoices_[index];
        if (slot.state == ControlState::Idle) {
            slot.state = ControlState::Reserved;
            slot.releaseMixFrame = UnsetMixFrame;
            slot.requestedStopFade = NumericLimits<uint32>::Max();
            nextVoiceSearch_ = (index + 1) % count;
            return Result<AudioVoiceHandle>::Success(AudioVoiceHandle{index, slot.generation});
        }
    }

    return Result<AudioVoiceHandle>::Failure(ErrorCode::CapacityExceeded);
}

Result<AudioRtCommand> AudioMixer::PreparePlay(AudioVoiceHandle voice, AudioClipId clip,
                                               uint32 startFrame, bool loop,
                                               const AudioVoiceParams& params) noexcept {
    ControlVoice* slot = FindControl(voice);
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
    if (slot->state != ControlState::Reserved || startFrame >= desc.frameCount ||
        (loop && startFrame >= loopEnd) || !IsValidParams(params) || azimuthOnStereo) {
        return Result<AudioRtCommand>::Failure(ErrorCode::InvalidArgument);
    }

    // 스트림 clip은 등록 값과 같은 시작·loop이고 voice 하나만 묶는다(SPSC 링 — 3.40.1절).
    const ClipSlot& clipSlot = clips_[clip.index];
    if (clipSlot.stream != nullptr && (startFrame != clipSlot.streamStartFrame ||
                                       loop != clipSlot.streamLoop || clipSlot.users != 0)) {
        return Result<AudioRtCommand>::Failure(ErrorCode::InvalidArgument);
    }

    // clip을 voice에 묶고 사용 수를 올린다. ReleaseVoice가 내린다(해제된 PCM을 읽지 않게).
    ++clips_[clip.index].users;
    slot->clipIndex = clip.index;
    slot->state = ControlState::Submitted;

    AudioRtCommand command;
    command.type = AudioRtCommandType::Play;
    command.voice = voice;
    command.params = params;
    command.clipIndex = clip.index;
    command.startFrame = startFrame;
    command.loop = loop;
    command.stopAllEpoch = controlStopAllEpoch_;
    command.sequence = ++nextSequence_;
    return Result<AudioRtCommand>::Success(command);
}

Result<void> AudioMixer::AbandonPreparedPlay(AudioVoiceHandle voice) noexcept {
    ControlVoice* slot = FindControl(voice);
    if (slot == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (slot->state != ControlState::Submitted ||
        slot->requestedStopFade != NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ROCKS_ASSERT(clips_[slot->clipIndex].users > 0);
    --clips_[slot->clipIndex].users;
    slot->state = ControlState::Reserved;
    return Result<void>::Success();
}

Result<AudioRtCommand> AudioMixer::MakeStop(AudioVoiceHandle voice, uint64 mixFrame,
                                            uint32 fadeFrames) noexcept {
    if (auto checked = CheckCommandTarget(voice); !checked) {
        return Result<AudioRtCommand>::Failure(checked.Error());
    }

    AudioRtCommand command;
    command.type = AudioRtCommandType::Stop;
    command.voice = voice;
    command.mixFrame = mixFrame;
    command.fadeFrames = fadeFrames;
    command.sequence = ++nextSequence_;
    return Result<AudioRtCommand>::Success(command);
}

Result<AudioRtCommand> AudioMixer::MakeSetParams(AudioVoiceHandle voice, uint64 mixFrame,
                                                 const AudioVoiceParams& params,
                                                 uint32 rampFrames) noexcept {
    if (auto checked = CheckCommandTarget(voice); !checked) {
        return Result<AudioRtCommand>::Failure(checked.Error());
    }

    const AudioClipDesc& desc = clips_[controlVoices_[voice.index].clipIndex].desc;
    if (!IsValidParams(params) ||
        (params.panMode == AudioPanMode::Azimuth && desc.channelCount != 1)) {
        return Result<AudioRtCommand>::Failure(ErrorCode::InvalidArgument);
    }

    AudioRtCommand command;
    command.type = AudioRtCommandType::SetParams;
    command.voice = voice;
    command.mixFrame = mixFrame;
    command.params = params;
    command.rampFrames = rampFrames;
    command.sequence = ++nextSequence_;
    return Result<AudioRtCommand>::Success(command);
}

Result<AudioRtCommand> AudioMixer::MakeSetMasterGain(uint64 mixFrame, float gain,
                                                     uint32 rampFrames) noexcept {
    if (!math::IsFinite(gain) || gain < 0.0f || gain > MaxMasterGain) {
        return Result<AudioRtCommand>::Failure(ErrorCode::InvalidArgument);
    }

    AudioRtCommand command;
    command.type = AudioRtCommandType::SetMasterGain;
    command.mixFrame = mixFrame;
    command.masterGain = gain;
    command.rampFrames = rampFrames;
    command.sequence = ++nextSequence_;
    return Result<AudioRtCommand>::Success(command);
}

Result<void> AudioMixer::PublishParams(AudioVoiceHandle voice,
                                       const AudioVoiceParams& params) noexcept {
    if (auto checked = CheckCommandTarget(voice); !checked) {
        return checked;
    }

    const AudioClipDesc& desc = clips_[controlVoices_[voice.index].clipIndex].desc;
    if (!IsValidParams(params) ||
        (params.panMode == AudioPanMode::Azimuth && desc.channelCount != 1)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // seqlock 쓰기(3.23.2절): version을 홀수로 → release fence → 내용 → version을 짝수로(release).
    SharedVoice& shared = sharedVoices_[voice.index];
    const uint64 sequence = ++nextSequence_;
    const uint32 version = AtomicLoad<MemoryOrder::Relaxed>(&shared.version);
    AtomicStore<MemoryOrder::Relaxed>(&shared.version, version + 1);
    AtomicThreadFence<MemoryOrder::Release>();
    const uint32 words[SeqlockWords] = {voice.generation,
                                        static_cast<uint32>(sequence & LowWordMask),
                                        static_cast<uint32>(sequence >> HighWordShift),
                                        FloatBits(params.gain),
                                        FloatBits(params.pitch),
                                        static_cast<uint32>(params.panMode),
                                        FloatBits(params.azimuthDegrees),
                                        FloatBits(params.nonSpatializedAmount),
                                        FloatBits(params.lowPassCutoffHz),
                                        FloatBits(params.highPassCutoffHz)};
    for (uint32 word = 0; word < SeqlockWords; ++word) {
        AtomicStore<MemoryOrder::Relaxed>(&shared.contents[word], words[word]);
    }

    AtomicStore<MemoryOrder::Release>(&shared.version, version + 2);
    return Result<void>::Success();
}

Result<void> AudioMixer::RequestStop(AudioVoiceHandle voice, uint32 fadeFrames) noexcept {
    if (auto checked = CheckCommandTarget(voice); !checked) {
        return checked;
    }

    // mailbox에는 이 수명에서 낸 fade의 최소값만 쓴다. 값이 줄기만 하므로 RT가 언제 읽어도 가장
    // 이른 끝을 보고, 같은 값이 되돌아오는 ABA도 없다(3.23.3절).
    ControlVoice& control = controlVoices_[voice.index];
    if (fadeFrames >= control.requestedStopFade) {
        return Result<void>::Success();
    }

    control.requestedStopFade = fadeFrames;
    const uint64 value = (static_cast<uint64>(voice.generation) << HighWordShift) | fadeFrames;
    AtomicStore<MemoryOrder::Release>(&sharedVoices_[voice.index].mailbox, value);
    return Result<void>::Success();
}

void AudioMixer::StopAll(uint32 fadeFrames) noexcept {
    // RT가 아직 이전 epoch를 처리하지 않았으면 그 fade와 새 fade 중 짧은 쪽을 낸다(마지막 쓰기가
    // 앞선 StopAll(0)을 지우지 않게 — 3.23.4절).
    if (AtomicLoad<MemoryOrder::Acquire>(&publishedStopAllEpoch_) != controlStopAllEpoch_ &&
        controlStopAllFade_ < fadeFrames) {
        fadeFrames = controlStopAllFade_;
    }

    controlStopAllFade_ = fadeFrames;
    ++controlStopAllEpoch_;
    const uint64 value = (static_cast<uint64>(controlStopAllEpoch_) << HighWordShift) | fadeFrames;
    AtomicStore<MemoryOrder::Release>(&stopAllWord_, value);
}

bool AudioMixer::ReadTerminal(uint32 index, uint32 generation, AudioVoiceTerminal& terminal,
                              uint64& releaseMixFrame) const noexcept {
    const SharedVoice& shared = sharedVoices_[index];
    const uint64 word = AtomicLoad<MemoryOrder::Acquire>(&shared.terminal);
    if (word == 0 || static_cast<uint32>(word >> HighWordShift) != generation) {
        return false;
    }

    terminal.voice = AudioVoiceHandle{index, generation};
    terminal.reason = static_cast<AudioVoiceEndReason>(word & ReasonMask);
    terminal.endMixFrame = AtomicLoad<MemoryOrder::Relaxed>(&shared.endMixFrame);
    releaseMixFrame = AtomicLoad<MemoryOrder::Relaxed>(&shared.releaseMixFrame);
    return true;
}

Result<void> AudioMixer::ReleaseVoice(AudioVoiceHandle voice) noexcept {
    ControlVoice* slot = FindControl(voice);
    if (slot == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (slot->state == ControlState::Finished && CompletedMixFrame() >= slot->releaseMixFrame) {
        ROCKS_ASSERT(clips_[slot->clipIndex].users > 0);
        --clips_[slot->clipIndex].users;
    } else if (slot->state != ControlState::Reserved) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    slot->state = ControlState::Idle;
    slot->generation = NextGeneration(slot->generation);
    return Result<void>::Success();
}

Result<AudioVoiceStatus> AudioMixer::QueryVoice(AudioVoiceHandle voice) const noexcept {
    const ControlVoice* slot = FindControl(voice);
    if (slot == nullptr) {
        return Result<AudioVoiceStatus>::Failure(ErrorCode::StaleHandle);
    }

    AudioVoiceStatus status;
    if (slot->state == ControlState::Reserved) {
        status.state = AudioVoiceState::Reserved;
        return Result<AudioVoiceStatus>::Success(status);
    }

    // 읽기 순서: terminal → start(3.23.5절).
    AudioVoiceTerminal terminal;
    uint64 releaseMixFrame = UnsetMixFrame;
    const bool finished = ReadTerminal(voice.index, voice.generation, terminal, releaseMixFrame);
    const SharedVoice& shared = sharedVoices_[voice.index];
    const bool started =
        AtomicLoad<MemoryOrder::Acquire>(&shared.startGeneration) == voice.generation;
    if (started) {
        status.startMixFrame = AtomicLoad<MemoryOrder::Relaxed>(&shared.startMixFrame);
    }

    if (finished) {
        status.state = AudioVoiceState::Finished;
        status.endReason = terminal.reason;
        status.endMixFrame = terminal.endMixFrame;
        status.releaseMixFrame = releaseMixFrame;
    } else {
        status.state = started ? AudioVoiceState::Playing : AudioVoiceState::Prepared;
    }

    return Result<AudioVoiceStatus>::Success(status);
}

uint32 AudioMixer::CollectFinished(Span<AudioVoiceTerminal> out) noexcept {
    uint32 written = 0;
    for (uint32 index = 0; index < controlVoices_.Size() && written < out.Size(); ++index) {
        ControlVoice& slot = controlVoices_[index];
        if (slot.state != ControlState::Submitted) {
            continue;
        }

        uint64 releaseMixFrame = UnsetMixFrame;
        if (ReadTerminal(index, slot.generation, out[written], releaseMixFrame)) {
            slot.state = ControlState::Finished;
            slot.releaseMixFrame = releaseMixFrame;
            ++written;
        }
    }

    return written;
}

uint64 AudioMixer::CompletedMixFrame() const noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&completedMixFrame_);
}

AudioMixerCounters AudioMixer::CountersSnapshot() const noexcept {
    uint64 words[CounterSnapshotWords] = {};
    for (uint32 index = 0; index < CounterSnapshotWords; ++index) {
        words[index] = AtomicLoad<MemoryOrder::Relaxed>(&counterSnapshot_[index]);
    }

    AudioMixerCounters counters;
    uint32 next = 0;
    counters.lateCommands = words[next++];
    counters.lateFrames = words[next++];
    counters.deferredCommands = words[next++];
    counters.staleCommands = words[next++];
    for (uint64& ignored : counters.ignoredCommands) {
        ignored = words[next++];
    }

    counters.clampedParams = words[next++];
    counters.nonFiniteSamples = words[next++];
    counters.clippedSamples = words[next++];
    counters.skippedSeqlockReads = words[next++];
    counters.streamStarvedFrames = words[next++];
    counters.streamStartDelayFrames = words[next++];
    return counters;
}

// --- RT 쪽: 경계 값 처리 ---

void AudioMixer::AdoptLifetime(VoiceSlot& slot, uint32 generation) noexcept {
    slot.generation = generation;
    slot.state = AudioVoiceState::Prepared;
    slot.hasPendingParams = false;
    slot.pendingParamsSequence = 0;
    slot.lastAppliedSequence = 0;
    slot.stopReason = AudioVoiceEndReason::Stopped;
    slot.awaitingStream = false;
}

void AudioMixer::Finish(uint32 index, VoiceSlot& slot, AudioVoiceEndReason reason,
                        uint64 frame) noexcept {
    slot.state = AudioVoiceState::Finished;
    // 두 frame을 쓴 뒤 word를 출판한다. 이 뒤로 RT는 그 수명의 clip·PCM·DSP를 읽지 않는다.
    SharedVoice& shared = sharedVoices_[index];
    AtomicStore<MemoryOrder::Relaxed>(&shared.endMixFrame, frame);
    AtomicStore<MemoryOrder::Relaxed>(&shared.releaseMixFrame, mixFrame_ + config_.blockFrames);
    const uint64 word =
        (static_cast<uint64>(slot.generation) << HighWordShift) | static_cast<uint64>(reason);
    AtomicStore<MemoryOrder::Release>(&shared.terminal, word);
}

void AudioMixer::ApplyStop(uint32 index, VoiceSlot& slot, uint32 fadeFrames, uint64 frame,
                           AudioVoiceEndReason reason, bool countIgnored) noexcept {
    switch (slot.state) {
    case AudioVoiceState::Prepared:
        // 시작 전 Stop은 fade 없이 바로 끝낸다(UE StopInternal). 뒤에 오는 Play는 무시된다.
        Finish(index, slot, reason, frame);
        return;
    case AudioVoiceState::Playing:
        // 시작 수위를 기다리는 스트림 voice는 낸 소리가 없으므로 바로 끝낸다(3.40.1절).
        if (fadeFrames == 0 || slot.awaitingStream) {
            Finish(index, slot, reason, frame);
        } else {
            slot.state = AudioVoiceState::Stopping;
            slot.stopReason = reason;
            slot.fade.SetTarget(0.0f, fadeFrames);
        }

        return;
    case AudioVoiceState::Stopping:
        // 반복 Stop은 남은 fade보다 일찍 끝날 때만 받아들인다(steal의 fade 0 포함).
        if (fadeFrames == 0) {
            Finish(index, slot, reason, frame);
            return;
        }

        if (fadeFrames < slot.fade.RemainingFrames()) {
            slot.stopReason = reason;
            slot.fade.SetTarget(0.0f, fadeFrames);
            return;
        }

        break;
    case AudioVoiceState::Idle:
    case AudioVoiceState::Reserved:
    case AudioVoiceState::Finished:
        break;
    }

    // stop-all이 건너뛴 voice는 명령 무시가 아니므로 세지 않는다.
    if (countIgnored) {
        ++counters_.ignoredCommands[static_cast<uint32>(AudioRtCommandType::Stop)];
    }
}

void AudioMixer::ProcessStopAll(uint64 frame) noexcept {
    const uint64 word = AtomicLoad<MemoryOrder::Acquire>(&stopAllWord_);
    const uint32 epoch = static_cast<uint32>(word >> HighWordShift);
    if (epoch == processedStopAllEpoch_) {
        return;
    }

    processedStopAllEpoch_ = epoch;
    AtomicStore<MemoryOrder::Release>(&publishedStopAllEpoch_, epoch);
    const uint32 fadeFrames = static_cast<uint32>(word & LowWordMask);
    for (uint32 index = 0; index < voices_.Size(); ++index) {
        VoiceSlot& slot = voices_[index];
        if (slot.state == AudioVoiceState::Prepared || slot.state == AudioVoiceState::Playing ||
            slot.state == AudioVoiceState::Stopping) {
            ApplyStop(index, slot, fadeFrames, frame, AudioVoiceEndReason::StoppedAll, false);
        }
    }
}

void AudioMixer::ProcessMailboxes(uint64 frame) noexcept {
    for (uint32 index = 0; index < voices_.Size(); ++index) {
        VoiceSlot& slot = voices_[index];
        const uint64 value = AtomicLoad<MemoryOrder::Acquire>(&sharedVoices_[index].mailbox);
        if (value == 0 || value == slot.lastMailbox) {
            continue;
        }

        slot.lastMailbox = value;
        const uint32 generation = static_cast<uint32>(value >> HighWordShift);
        const int32 delta = GenerationDelta(generation, slot.generation);
        if (delta < 0) {
            ++counters_.staleCommands;
            continue;
        }

        // Play가 아직 대기 중인데 stop이 먼저 보이면 새 수명을 Prepared로 열고 바로 끝낸다.
        if (delta > 0) {
            AdoptLifetime(slot, generation);
        }

        ApplyStop(index, slot, static_cast<uint32>(value & LowWordMask), frame,
                  AudioVoiceEndReason::Stopped, true);
    }
}

bool AudioMixer::ReadSeqlock(uint32 index, uint32& generation, uint64& sequence,
                             AudioVoiceParams& params) noexcept {
    static_assert(SeqlockWords == SeqlockWordCount,
                  "Seqlock contents must match the SeqlockWord layout");
    static_assert(CounterSnapshotWords * sizeof(uint64) == sizeof(AudioMixerCounters),
                  "Counter snapshot must cover every AudioMixerCounters field");
    // seqlock 읽기(3.23.2절): version(acquire) → 내용(relaxed) → acquire fence → version 재확인.
    // 홀수·불일치면 건너뛰고 재시도하지 않는다(대기 금지).
    const SharedVoice& shared = sharedVoices_[index];
    const uint32 before = AtomicLoad<MemoryOrder::Acquire>(&shared.version);
    if ((before & SeqlockWritingBit) != 0) {
        ++counters_.skippedSeqlockReads;
        return false;
    }

    if (before == 0) {
        return false;
    }

    uint32 words[SeqlockWords] = {};
    for (uint32 word = 0; word < SeqlockWords; ++word) {
        words[word] = AtomicLoad<MemoryOrder::Relaxed>(&shared.contents[word]);
    }

    if (seqlockReadHook_ != nullptr) {
        seqlockReadHook_(*this, index);
    }

    AtomicThreadFence<MemoryOrder::Acquire>();
    const uint32 after = AtomicLoad<MemoryOrder::Relaxed>(&shared.version);
    if (before != after) {
        ++counters_.skippedSeqlockReads;
        return false;
    }

    generation = words[SeqlockGeneration];
    sequence = (static_cast<uint64>(words[SeqlockSequenceHigh]) << HighWordShift) |
               words[SeqlockSequenceLow];
    params.gain = BitsFloat(words[SeqlockGain]);
    params.pitch = BitsFloat(words[SeqlockPitch]);
    params.panMode = static_cast<AudioPanMode>(words[SeqlockPanMode]);
    params.azimuthDegrees = BitsFloat(words[SeqlockAzimuth]);
    params.nonSpatializedAmount = BitsFloat(words[SeqlockNonSpatialized]);
    params.lowPassCutoffHz = BitsFloat(words[SeqlockLowPass]);
    params.highPassCutoffHz = BitsFloat(words[SeqlockHighPass]);
    return true;
}

void AudioMixer::ApplySeqlock(uint32 index, VoiceSlot& slot, bool immediate) noexcept {
    uint32 generation = 0;
    uint64 sequence = 0;
    AudioVoiceParams params;
    if (!ReadSeqlock(index, generation, sequence, params) || generation != slot.generation ||
        sequence <= slot.lastAppliedSequence) {
        return;
    }

    slot.lastAppliedSequence = sequence;
    if (slot.state == AudioVoiceState::Prepared) {
        if (!slot.hasPendingParams || sequence > slot.pendingParamsSequence) {
            slot.params = params;
            slot.pendingParamsSequence = sequence;
            slot.hasPendingParams = true;
        }

        return;
    }

    ApplyParams(slot, params, immediate ? 0u : DefaultParamRampFrames, immediate);
}

void AudioMixer::ProcessSeqlocks() noexcept {
    for (uint32 index = 0; index < voices_.Size(); ++index) {
        VoiceSlot& slot = voices_[index];
        if (slot.state == AudioVoiceState::Prepared || slot.state == AudioVoiceState::Playing ||
            slot.state == AudioVoiceState::Stopping) {
            ApplySeqlock(index, slot, false);
        }
    }
}

// --- RT 쪽: 명령 ---

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

void AudioMixer::StartVoice(uint32 index, VoiceSlot& slot, const AudioRtCommand& command,
                            uint64 frame) noexcept {
    slot.clipIndex = command.clipIndex;
    slot.loop = command.loop;
    const AudioClipDesc& desc = clips_[slot.clipIndex].desc;
    // Play보다 뒤 순번으로 저장된 파라미터(SetParams·seqlock)가 있으면 그것이 이긴다.
    const bool pendingWins = slot.hasPendingParams && slot.pendingParamsSequence > command.sequence;
    const AudioVoiceParams initial = pendingWins ? slot.params : command.params;
    slot.lastAppliedSequence = pendingWins ? slot.pendingParamsSequence : command.sequence;

    // DSP 상태는 clip 채널 수로 다시 만든다(할당 없음). 인자는 늘 유효하다.
    auto resampler = LinearResampler::TryCreate(desc.channelCount, 1.0);
    auto lowPass = OnePoleLowPass::TryCreate(config_.sampleRate, desc.channelCount);
    auto highPass = OnePoleHighPass::TryCreate(config_.sampleRate, desc.channelCount);
    ROCKS_ASSERT(resampler && lowPass && highPass);
    slot.resampler = resampler.Value();
    slot.lowPass = lowPass.Value();
    slot.highPass = highPass.Value();
    slot.fade.Reset(1.0f);
    slot.sourceCursor = command.startFrame;
    ApplyParams(slot, initial, 0, true);
    slot.hasPendingParams = false;
    slot.state = AudioVoiceState::Playing;
    // 스트림 원본은 시작 수위를 채운 블록에서 시작 시각을 출판한다(3.40.1절).
    slot.awaitingStream = clips_[slot.clipIndex].stream != nullptr;
    if (!slot.awaitingStream) {
        PublishStart(index, slot, frame);
    }

    // PreparePlay 뒤 Play 적용 전에 낸 출판도 바로 반영한다(3.23.2절).
    ApplySeqlock(index, slot, true);
}

void AudioMixer::PublishStart(uint32 index, VoiceSlot& slot, uint64 frame) noexcept {
    SharedVoice& shared = sharedVoices_[index];
    AtomicStore<MemoryOrder::Relaxed>(&shared.startMixFrame, frame);
    AtomicStore<MemoryOrder::Release>(&shared.startGeneration, slot.generation);
}

bool AudioMixer::StreamReadyToStart(const VoiceSlot& slot) const noexcept {
    const ClipSlot& clip = clips_[slot.clipIndex];
    const AudioStreamSnapshot snapshot = clip.stream->Snapshot();
    if (snapshot.state != AudioStreamState::Running) {
        return true;
    }

    uint32 wanted = StreamStartFrames;
    if (!slot.loop) {
        const uint32 remaining = clip.desc.frameCount - static_cast<uint32>(slot.sourceCursor);
        wanted = remaining < wanted ? remaining : wanted;
    }

    return snapshot.readable >= wanted;
}

uint32 AudioMixer::StreamRenderableFrames(VoiceSlot& slot, uint32 chunk,
                                          uint32 available) const noexcept {
    const ClipSlot& clip = clips_[slot.clipIndex];
    const uint32 needed = slot.resampler.InputFramesNeeded(chunk).Value();
    uint64 ringNeeded = needed;
    if (!slot.loop) {
        // 원본 끝 뒤는 0 꼬리라 링을 읽지 않는다.
        const int64 remaining =
            static_cast<int64>(clip.desc.frameCount) - static_cast<int64>(slot.sourceCursor);
        ringNeeded = remaining <= 0
                         ? 0
                         : (static_cast<uint64>(remaining) < needed ? static_cast<uint64>(remaining)
                                                                    : needed);
    }

    if (available >= ringNeeded) {
        return chunk;
    }

    // 끝까지의 PCM이 다 오지 않았으므로 0 꼬리도 없다 — 가진 입력으로 낼 수 있는 만큼이다.
    return slot.resampler.OutputFramesFor(available, chunk);
}

void AudioMixer::SignalStreamRefill() noexcept {
    if (config_.streamRefillSignal == nullptr) {
        return;
    }

    for (const VoiceSlot& slot : voices_) {
        if (slot.state != AudioVoiceState::Playing && slot.state != AudioVoiceState::Stopping) {
            continue;
        }

        const AudioStreamRing* ring = clips_[slot.clipIndex].stream;
        if (ring != nullptr && ring->State() == AudioStreamState::Running &&
            ring->ReadableFrames() < ring->CapacityFrames() / StreamRefillDivisor) {
            config_.streamRefillSignal->Signal();
            return;
        }
    }
}

void AudioMixer::ApplyCommand(const AudioRtCommand& command, uint64 frame) noexcept {
    const auto isValidBinding = [this](const AudioRtCommand& play) noexcept {
        if (play.clipIndex >= clips_.Size() || clips_[play.clipIndex].desc.channelCount == 0) {
            return false;
        }

        const AudioClipDesc& desc = clips_[play.clipIndex].desc;
        uint32 loopBegin = 0;
        uint32 loopEnd = 0;
        LoopRange(desc, loopBegin, loopEnd);
        return play.startFrame < desc.frameCount && (!play.loop || play.startFrame < loopEnd);
    };

    const uint32 typeIndex = static_cast<uint32>(command.type);
    if (command.type == AudioRtCommandType::SetMasterGain) {
        const float gain = ClampCounted(command.masterGain, 0.0f, MaxMasterGain, DefaultMasterGain,
                                        counters_.clampedParams);
        masterGain_.SetTarget(gain, command.rampFrames);
        return;
    }

    if (command.voice.index >= voices_.Size()) {
        ++counters_.staleCommands;
        return;
    }

    const uint32 index = command.voice.index;
    VoiceSlot& slot = voices_[index];
    const int32 delta = GenerationDelta(command.voice.generation, slot.generation);
    if (command.voice.generation == 0 || delta < 0) {
        ++counters_.staleCommands;
        return;
    }

    if (delta > 0) {
        AdoptLifetime(slot, command.voice.generation);
    }

    switch (command.type) {
    case AudioRtCommandType::Play:
        if (slot.state != AudioVoiceState::Prepared) {
            break;
        }

        // StopAll보다 먼저 만든 Play는 시작하지 않는다(3.23.4절).
        if (GenerationDelta(command.stopAllEpoch, processedStopAllEpoch_) < 0) {
            Finish(index, slot, AudioVoiceEndReason::StoppedAll, frame);
            return;
        }

        // 제어 API를 거치지 않은 손 명령의 clip 묶음을 거른다(시험 전용, PreparePlay와 같은 규칙).
        if (!isValidBinding(command)) {
            Finish(index, slot, AudioVoiceEndReason::StartFailed, frame);
            return;
        }

        StartVoice(index, slot, command, frame);
        return;
    case AudioRtCommandType::Stop:
        ApplyStop(index, slot, command.fadeFrames, frame, AudioVoiceEndReason::Stopped, true);
        return;
    case AudioRtCommandType::SetParams:
        // 두 최신값 경로(seqlock·SetParams)는 제어 순번으로 비교한다(3.23.2절).
        if (command.sequence <= slot.lastAppliedSequence) {
            break;
        }

        if (slot.state == AudioVoiceState::Prepared) {
            if (!slot.hasPendingParams || command.sequence > slot.pendingParamsSequence) {
                slot.params = command.params;
                slot.pendingParamsSequence = command.sequence;
                slot.hasPendingParams = true;
                return;
            }

            break;
        }

        if (slot.state == AudioVoiceState::Playing || slot.state == AudioVoiceState::Stopping) {
            ApplyParams(slot, command.params, command.rampFrames, false);
            slot.lastAppliedSequence = command.sequence;
            return;
        }

        break;
    case AudioRtCommandType::SetMasterGain:
        break;
    }

    ++counters_.ignoredCommands[typeIndex];
}

// --- RT 쪽: 렌더 ---

RenderBlockResult AudioMixer::RenderBlock(Span<const AudioRtCommand> commands,
                                          Span<float> interleavedOut) noexcept {
    ROCKS_ASSERT(platform::IsRealtimeAudioFloatingPointEnvironment());
    const uint32 blockFrames = config_.blockFrames;
    ROCKS_ASSERT(interleavedOut.Size() == static_cast<usize>(blockFrames) * OutputChannelCount());
    master_.Clear();

    const uint64 blockStart = mixFrame_;
    const uint64 blockEnd = blockStart + blockFrames;
    // 블록 시작 순서: stop-all → stop mailbox → seqlock → 명령(3.23.7절).
    ProcessStopAll(blockStart);
    ProcessMailboxes(blockStart);
    ProcessSeqlocks();

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
    SignalStreamRefill();
    mixFrame_ = blockEnd;
    PublishBlockEnd();
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
            RenderVoice(index, slot, offset, frames);
        }
    }

    // 마스터 gain은 구간마다 건다. 블록 중간의 SetMasterGain이 앞 구간을 바꾸지 않게 한다.
    masterGain_.Apply(master_.View().Slice(offset, frames));
}

uint32 AudioMixer::FillInput(VoiceSlot& slot, uint32 frames) noexcept {
    const ClipSlot& clip = clips_[slot.clipIndex];
    if (clip.stream != nullptr) {
        // 링은 재생 순서(loop면 감은 순서)라 앞에서부터 읽는다. non-loop의 끝 뒤는 0 꼬리다.
        uint32 ringFrames = frames;
        if (!slot.loop) {
            const uint64 cursor = slot.sourceCursor;
            const uint64 remaining =
                cursor < clip.desc.frameCount ? clip.desc.frameCount - cursor : 0;
            ringFrames = remaining < frames ? static_cast<uint32>(remaining) : frames;
        }

        const PlanarView input = WithChannels(inputScratch_.View(frames), clip.desc.channelCount);
        clip.stream->Peek(ringFrames, input);
        for (uint32 channel = 0; channel < clip.desc.channelCount; ++channel) {
            float* target = input.channels[channel];
            for (uint32 frame = ringFrames; frame < frames; ++frame) {
                target[frame] = 0.0f;
            }
        }

        return frames;
    }

    const AudioClipDesc& desc = clip.desc;
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
    AudioStreamRing* stream = clips_[slot.clipIndex].stream;
    if (stream != nullptr) {
        // 링에서 온 frame만 버린다(non-loop의 끝 뒤 0 꼬리는 링에 없다).
        uint64 fromRing = consumed;
        if (!slot.loop) {
            const uint64 frameCount = clips_[slot.clipIndex].desc.frameCount;
            const uint64 remaining =
                slot.sourceCursor < frameCount ? frameCount - slot.sourceCursor : 0;
            fromRing = remaining < fromRing ? remaining : fromRing;
        }

        stream->Consume(static_cast<uint32>(fromRing));
    }

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

void AudioMixer::RenderVoice(uint32 index, VoiceSlot& slot, uint32 offset, uint32 frames) noexcept {
    const AudioClipDesc& desc = clips_[slot.clipIndex].desc;
    AudioStreamRing* stream = clips_[slot.clipIndex].stream;
    const uint32 sourceChannels = desc.channelCount;
    const uint32 outputs = OutputChannelCount();
    const uint64 segmentStartFrame = mixFrame_ + offset;
    // 시작 대기(3.40.1절 — UE IsPreparedToInit): 수위를 채우거나 끝 상태가 될 때까지 소리도 cursor
    // 진행도 없다. 채운 구간에서 시작 시각을 출판한다.
    if (slot.awaitingStream) {
        if (!StreamReadyToStart(slot)) {
            counters_.streamStartDelayFrames += frames;
            return;
        }

        slot.awaitingStream = false;
        PublishStart(index, slot, segmentStartFrame);
    }

    uint32 produced = 0;
    while (produced < frames) {
        uint32 chunk = frames - produced;
        // 요구 입력이 scratch를 넘으면 출력을 줄인다(pitch·rate 비율이 크면).
        uint32 needed = slot.resampler.InputFramesNeeded(chunk).Value();
        while (needed > MaxBlockFrames && chunk > 1) {
            chunk /= 2;
            needed = slot.resampler.InputFramesNeeded(chunk).Value();
        }

        // 끝 판정(3.22.5절): 자연 끝은 다음 출력 위치가 원본 끝 이상이 되는 frame, fade 끝은 fade
        // 길이를 다 낸 다음 frame이다. 같은 frame이면 자연 끝이 이긴다.
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
                ending = slot.stopReason;
            }
        }

        if (chunk > 0) {
            // 스트림 원본은 링에 있는 만큼만 낸다(3.40.1절 굶음). 수와 상태는 한 스냅숏에서 읽는다
            // (끝 판정이 그 사이의 마지막 Write·MarkEnded를 "모자람"으로 보지 않게).
            AudioStreamSnapshot snapshot;
            if (stream != nullptr) {
                snapshot = stream->Snapshot();
            }

            const uint32 rendered =
                stream != nullptr ? StreamRenderableFrames(slot, chunk, snapshot.readable) : chunk;
            if (rendered > 0) {
                needed = slot.resampler.InputFramesNeeded(rendered).Value();
                FillInput(slot, needed);
                const PlanarView input = WithChannels(inputScratch_.View(needed), sourceChannels);
                const PlanarView voice = WithChannels(voiceScratch_.View(rendered), sourceChannels);
                const uint32 consumed = slot.resampler.Process(input, voice).Value();
                AdvanceCursor(slot, consumed);
                slot.lowPass.Process(voice);
                slot.highPass.Process(voice);
                slot.gain.Apply(voice);
                slot.fade.Apply(voice);
                for (uint32 source = 0; source < sourceChannels; ++source) {
                    for (uint32 output = 0; output < outputs; ++output) {
                        slot.channelGains[source][output].MixAdd(
                            voice.channels[source], master_.Channel(output) + offset + produced,
                            rendered);
                    }
                }
            }

            if (rendered < chunk) {
                // 굶은 구간은 무음이다. 리샘플러·cursor·필터·gain은 멈추고(이어 재생), fade만
                // 진행해 Stop·StopAll·종료 drain이 생산자와 상관없이 끝나게 한다. 자연 끝은 PCM이
                // 올 때까지 미룬다. 끝 상태(Failed, 또는 Ended인데 모자람)면 스트림 실패로 끝낸다.
                const uint32 silent = chunk - rendered;
                counters_.streamStarvedFrames += silent;
                if (slot.state == AudioVoiceState::Stopping) {
                    slot.fade.Advance(silent);
                }

                if (ending == AudioVoiceEndReason::NaturalEnd) {
                    ending = AudioVoiceEndReason::None;
                }

                if (ending == AudioVoiceEndReason::None &&
                    snapshot.state != AudioStreamState::Running) {
                    ending = AudioVoiceEndReason::StreamFailed;
                }
            }

            produced += chunk;
        }

        if (ending != AudioVoiceEndReason::None) {
            Finish(index, slot, ending, segmentStartFrame + produced);
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

void AudioMixer::PublishBlockEnd() noexcept {
    uint64 words[CounterSnapshotWords] = {};
    uint32 next = 0;
    words[next++] = counters_.lateCommands;
    words[next++] = counters_.lateFrames;
    words[next++] = counters_.deferredCommands;
    words[next++] = counters_.staleCommands;
    for (const uint64 ignored : counters_.ignoredCommands) {
        words[next++] = ignored;
    }

    words[next++] = counters_.clampedParams;
    words[next++] = counters_.nonFiniteSamples;
    words[next++] = counters_.clippedSamples;
    words[next++] = counters_.skippedSeqlockReads;
    words[next++] = counters_.streamStarvedFrames;
    words[next++] = counters_.streamStartDelayFrames;
    for (uint32 word = 0; word < CounterSnapshotWords; ++word) {
        AtomicStore<MemoryOrder::Relaxed>(&counterSnapshot_[word], words[word]);
    }

    // ACK: 이 블록이 끝났다. 이번 블록에 terminal을 낸 voice는 RT가 더 건드리지 않는다.
    AtomicStore<MemoryOrder::Release>(&completedMixFrame_, mixFrame_);
}

} // namespace rocks::audio
