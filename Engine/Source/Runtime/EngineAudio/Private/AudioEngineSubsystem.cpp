#include "AudioDeviceContext.h"
#include "AudioEngineSettingsInternal.h"
#include "ControlBatch.h"

#include <Core/Assert.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Math/MathUtility.h>
#include <EngineAudio/AudioEngineSubsystem.h>
#include <ModuleLoader/HostServices.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>
#include <Task/DedicatedLane.h>

ROCKS_DEFINE_NATIVE_TYPE_KEY(rocks::AudioEngineSubsystem)
ROCKS_DEFINE_HOST_SERVICE(rocks::AudioEngineHostOptions)

namespace rocks {

namespace {

// 게임 ↔ 제어 순환 묶음 수다(3.35.5절). 하나는 게임이 채우고, 나머지가 제어 쪽·반환 큐에 있다.
constexpr usize ControlBatchCount = 3;
// 반환 큐 용량이다(2의 거듭제곱, 묶음 수보다 커야 Push가 실패하지 않는다).
constexpr usize ReturnQueueCapacity = 4;
static_assert(ReturnQueueCapacity > ControlBatchCount);

// 명령 링 포화는 구조상 생기지 않는다: Play 하나가 명령 하나이고 voice 상한까지만 살아 있으며, 그
// 밖에는 묶음마다 마스터 gain 하나뿐이다. Deferred는 방어 경로다(3.35.6절).
static_assert(MaxActiveVoicesSetting < audio::AudioRtCommandCapacity);

// 묶음 하나에 올 수 있는 결과 수의 상한이다(재생 하나는 결과 두 개 이하).
constexpr usize EventsPerBatch = 2 * MaxPlaybacks;
// 전달 목록 상한이다: 묶음 결과 전체(슬롯마다 둘 이하) + Ignored 끝.
constexpr usize DeliveringCapacity = 3 * MaxPlaybacks;

// 종료 꼬리 대기의 상한(ms)과 확인 간격(ms)이다(3.35.9절 — 꼬리 click 방지일 뿐 수명 장벽이
// 아니다).
constexpr uint64 ShutdownDrainTimeoutMs = 500;
constexpr uint32 ShutdownDrainPollMs = 1;
constexpr uint64 NanosecondsPerMillisecond = 1000 * 1000;

// 믹서 블록 크기 허용값이다(3.22.2절).
constexpr uint32 AllowedBlockFrames[] = {64, 128, 256, 512, 1024};

uint32 NearestBlockFrames(int32 requested) noexcept {
    uint32 best = AllowedBlockFrames[0];
    uint32 bestDistance = NumericLimits<uint32>::Max();
    for (const uint32 allowed : AllowedBlockFrames) {
        const int64 delta = static_cast<int64>(requested) - static_cast<int64>(allowed);
        const uint32 distance = static_cast<uint32>(delta < 0 ? -delta : delta);
        if (distance < bestDistance) {
            best = allowed;
            bestDistance = distance;
        }
    }

    return best;
}

bool InRange(float value, float low, float high) noexcept {
    return math::IsFinite(value) && value >= low && value <= high;
}

} // namespace

struct AudioEngineSubsystem::Control {
    explicit Control(Allocator& allocator) noexcept
        : lane(allocator),
          batches{ControlBatch(allocator), ControlBatch(allocator), ControlBatch(allocator)} {
    }

    DedicatedLane lane;
    ControlBatch batches[ControlBatchCount];
    bool laneStarted = false;
};

AudioEngineSubsystem::AudioEngineSubsystem(Allocator& allocator) noexcept
    : allocator_(allocator), playbacks_(allocator), freeSlots_(allocator), ignored_(allocator),
      delivering_(allocator), freeBatches_(allocator) {
}

AudioEngineSubsystem::~AudioEngineSubsystem() {
    // Engine은 늘 Deinitialize를 부른다. 초기화 도중 실패도 Initialize가 스스로 되돌린다.
    Drain();
}

Result<void> AudioEngineSubsystem::Initialize(Engine& engine) noexcept {
    options_ = AudioEngineHostOptions{};
    if (ModuleLoader* modules = engine.Modules()) {
        if (const auto* hostOptions = modules->Services().Find<AudioEngineHostOptions>()) {
            options_ = *hostOptions;
        }
    }

    traits_ = options_.traits.HasValue() ? options_.traits.Value() : CurrentAudioRunTraits();
    if (!playbacks_.TryResize(MaxPlaybacks) || !freeSlots_.TryReserve(MaxPlaybacks) ||
        !ignored_.TryReserve(MaxPlaybacks) || !delivering_.TryReserve(DeliveringCapacity) ||
        !freeBatches_.TryReserve(ControlBatchCount)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    // 낮은 슬롯부터 쓴다(시험 재현성).
    for (uint32 slot = MaxPlaybacks; slot > 0; --slot) {
        (void)freeSlots_.TryPushBack(slot - 1);
    }

    drained_ = false;
    if (traits_.audioOutput) {
        if (auto opened = OpenDevice(engine); !opened) {
            Drain();
            return opened;
        }

        if (auto started = StartControl(); !started) {
            Drain();
            return started;
        }
    } else {
        Log::Info(LogEngineAudio, "Audio output is disabled for this run; play requests are "
                                  "ignored");
    }

    accepting_ = true;
    return Result<void>::Success();
}

Result<void> AudioEngineSubsystem::OpenDevice(Engine& engine) noexcept {
    using namespace engine_audio_detail;
    int32 blockSetting = int32{audio::DefaultMixBlockFrames};
    int32 bufferSetting = 0;
    int32 rateSetting = 0;
    int32 voicesSetting = int32{DefaultMaxActiveVoices};
    if (engine.Settings() != nullptr) {
        blockSetting = MixerBlockFramesVar.Get();
        bufferSetting = MixerBufferCountVar.Get();
        rateSetting = MixerSampleRateVar.Get();
        voicesSetting = MaxVoicesVar.Get();
    }

    const uint32 blockFrames = NearestBlockFrames(blockSetting);
    if (static_cast<int32>(blockFrames) != blockSetting) {
        Log::Warning(LogEngineAudio,
                     "audio.mixer.blockframes {Requested} is not 64, 128, 256, 512 or 1024; using "
                     "{Used}",
                     blockSetting, blockFrames);
    }

    uint32 sampleRate = static_cast<uint32>(rateSetting < 0 ? 0 : rateSetting);
    if (sampleRate != 0 &&
        (sampleRate < MinMixerSampleRateSetting || sampleRate > MaxMixerSampleRateSetting)) {
        Log::Warning(LogEngineAudio,
                     "audio.mixer.samplerate {Requested} is outside 8000-192000; using the device "
                     "default",
                     sampleRate);
        sampleRate = 0;
    }

    const uint32 maxVoices = static_cast<uint32>(voicesSetting < 1 ? 1 : voicesSetting);
    audio::AudioMixerConfig mixerConfig;
    mixerConfig.sampleRate = sampleRate != 0 ? sampleRate : audio::DefaultMixSampleRate;
    mixerConfig.blockFrames = blockFrames;
    mixerConfig.maxVoices = maxVoices * MixerVoiceHeadroomFactor;

    UniquePtr<audio::AudioOutputBackend> backend;
    if (options_.useBackend && engine.Modules() != nullptr) {
        const auto* registry =
            engine.Modules()->Services().Find<audio::AudioOutputBackendRegistry>();
        if (registry != nullptr && registry->Factory() != nullptr) {
            auto made = registry->Factory()(allocator_);
            if (made) {
                backend = Move(made.Value());
            } else {
                Log::Warning(LogEngineAudio,
                             "Audio backend creation failed ({Error}); using Null "
                             "output",
                             ErrorCodeName(made.Error()));
            }
        }
    }

    audio::AudioOutputRequest request;
    request.sampleRate = sampleRate;
    request.callbackFrames = blockFrames;
    audio::AudioOutputDeviceOptions deviceOptions;
    deviceOptions.renderMode = options_.renderMode;
    deviceOptions.nullMode = options_.nullMode;
    deviceOptions.requestedBufferCount = static_cast<uint32>(bufferSetting < 0 ? 0 : bufferSetting);
    auto opened = audio::AudioOutputDevice::TryOpen(mixerConfig, Move(backend), request,
                                                    deviceOptions, allocator_);
    if (!opened) {
        Log::Error(LogEngineAudio, "Audio device open failed ({Error})",
                   ErrorCodeName(opened.Error()));
        return Result<void>::Failure(opened.Error());
    }

    device_ = Move(opened.Value());
    const audio::AudioOutputDeviceInfo& info = device_->Info();
    Log::Info(LogEngineAudio,
              "Audio device: {Backend}, {Rate} Hz, callback {Callback} frames, block {Block} "
              "frames, {Buffers} buffers, voices {Voices}",
              info.backendName, info.format.sampleRate, info.format.callbackFrames, blockFrames,
              info.bufferCount, maxVoices);
    if (info.usingNull && info.backendError != ErrorCode::None) {
        Log::Warning(
            LogEngineAudio,
            "Audio output device unavailable ({Error}, native {Native}); using Null output",
            ErrorCodeName(info.backendError), info.backendNativeError);
    }

    auto context = UniquePtr<AudioDeviceContext>::TryMake(allocator_, allocator_, *device_);
    if (!context) {
        return Result<void>::Failure(context.Error());
    }

    context_ = Move(context.Value());
    return context_->Initialize(maxVoices);
}

Result<void> AudioEngineSubsystem::StartControl() noexcept {
    auto control = UniquePtr<Control>::TryMake(allocator_, allocator_);
    if (!control) {
        return Result<void>::Failure(control.Error());
    }

    control_ = Move(control.Value());
    for (ControlBatch& batch : control_->batches) {
        if (!batch.events.TryReserve(EventsPerBatch)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    if (auto queue = returned_.Initialize(allocator_, ReturnQueueCapacity); !queue) {
        return queue;
    }

    DedicatedLaneConfig config;
    config.mode = options_.controlMode == AudioControlMode::Inline ? DedicatedLaneMode::Inline
                                                                   : DedicatedLaneMode::Thread;
    config.lane = TaskLane::Audio;
    config.thread.name = "AudioControl";
    if (auto started = control_->lane.Start(config); !started) {
        return started;
    }

    control_->laneStarted = true;
    current_ = &control_->batches[0];
    for (usize index = 1; index < ControlBatchCount; ++index) {
        (void)freeBatches_.TryPushBack(&control_->batches[index]);
    }

    return Result<void>::Success();
}

void AudioEngineSubsystem::OnFrameBegin(Engine& engine) noexcept {
    (void)engine;
    delivering_.Clear();
    if (control_ != nullptr && !drained_) {
        ControlBatch* batch = nullptr;
        while (returned_.TryPop(batch)) {
            for (const PlaybackEvent& event : batch->events) {
                (void)delivering_.TryPushBack(event);
            }

            batch->Recycle();
            (void)freeBatches_.TryPushBack(batch);
            ++stats_.returnedBatches;
        }
    }

    // 장치 없는 재생은 다음 프레임에 Ended(Ignored)다. 이번 전달 중 콜백이 더한 것은 다음
    // 프레임이다.
    const usize ignoredCount = ignored_.Size();
    for (usize index = 0; index < ignoredCount; ++index) {
        PlaybackEvent event;
        event.handle = ignored_[index];
        event.kind = PlaybackEventKind::Ended;
        event.reason = PlaybackEndReason::Ignored;
        (void)delivering_.TryPushBack(event);
    }

    if (ignoredCount != 0) {
        (void)ignored_.TryRemoveAt(0, ignoredCount);
    }

    // 지역 목록을 전달한다(콜백이 Play·Stop을 불러도 전달 중인 목록은 흔들리지 않는다).
    for (const PlaybackEvent& event : delivering_) {
        Deliver(event);
    }

    delivering_.Clear();
}

void AudioEngineSubsystem::OnFrameEnd(Engine& engine) noexcept {
    if (control_ == nullptr || drained_) {
        return;
    }

    current_->masterGain = MasterGainTarget(engine);
    SubmitCurrent();
}

void AudioEngineSubsystem::BeginShutdown(Engine& engine) noexcept {
    (void)engine;
    accepting_ = false;
}

Result<void> AudioEngineSubsystem::CompleteInFlightWork(Engine& engine) noexcept {
    (void)engine;
    Drain();
    return Result<void>::Success();
}

void AudioEngineSubsystem::Deinitialize(Engine& engine) noexcept {
    (void)engine;
    Drain();
    context_.Reset();
    device_.Reset();
    control_.Reset();
    returned_.Shutdown();
    current_ = nullptr;
    freeBatches_.Clear();
}

Result<PlaybackHandle> AudioEngineSubsystem::Play2D(const SoundSourceRef& sound,
                                                    PlayOptions options) noexcept {
    return Accept(sound, options, 0);
}

Result<void> AudioEngineSubsystem::Stop(PlaybackHandle handle, uint32 fadeFrames) noexcept {
    if (!handle.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    GamePlayback* playback = FindLive(handle);
    if (playback == nullptr || playback->status == PlaybackStatus::Ignored || drained_) {
        return Result<void>::Success();
    }

    ControlCommand command;
    command.type = ControlCommandType::Stop;
    command.handle = handle;
    command.fadeFrames = fadeFrames;
    if (!current_->commands.TryPushBack(Move(command))) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

Result<void> AudioEngineSubsystem::SetParameter(PlaybackHandle handle, PlaybackParameter parameter,
                                                float value, uint32 rampFrames) noexcept {
    const bool valid = parameter == PlaybackParameter::Volume
                           ? InRange(value, 0.0f, audio::MaxVoiceGain)
                           : InRange(value, audio::MinPitch, audio::MaxPitch);
    if (!handle.IsValid() || !valid) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    GamePlayback* playback = FindLive(handle);
    if (playback == nullptr || playback->status == PlaybackStatus::Ignored || drained_) {
        return Result<void>::Success();
    }

    // 같은 묶음 안의 같은 파라미터는 최신값 하나로 합친다(3.35.5절).
    const uint32 kind = parameter == PlaybackParameter::Volume ? 0 : 1;
    if (playback->parameterBatch == batchSerial_ && playback->parameterCommand[kind] != 0) {
        ControlCommand& merged = current_->commands[playback->parameterCommand[kind] - 1];
        merged.value = value;
        merged.rampFrames = rampFrames;
        return Result<void>::Success();
    }

    ControlCommand command;
    command.type = ControlCommandType::SetParameter;
    command.handle = handle;
    command.parameter = parameter;
    command.value = value;
    command.rampFrames = rampFrames;
    if (!current_->commands.TryPushBack(Move(command))) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    if (playback->parameterBatch != batchSerial_) {
        playback->parameterBatch = batchSerial_;
        playback->parameterCommand[0] = 0;
        playback->parameterCommand[1] = 0;
    }

    playback->parameterCommand[kind] = static_cast<uint32>(current_->commands.Size());
    return Result<void>::Success();
}

PlaybackStatus AudioEngineSubsystem::Status(PlaybackHandle handle) const noexcept {
    const GamePlayback* playback = FindLive(handle);
    return playback != nullptr ? playback->status : PlaybackStatus::Finished;
}

AudioEngineStats AudioEngineSubsystem::Stats() const noexcept {
    AudioEngineStats stats = stats_;
    stats.livePlaybacks = static_cast<uint32>(MaxPlaybacks - freeSlots_.Size());
    if (playbacks_.IsEmpty()) {
        stats.livePlaybacks = 0;
    }

    return stats;
}

void AudioEngineSubsystem::FlushControl() noexcept {
    if (control_ != nullptr && control_->laneStarted) {
        (void)control_->lane.Flush();
    }
}

AudioEngineSubsystem::ContextStats AudioEngineSubsystem::ControlStats() const noexcept {
    ContextStats out;
    if (context_ == nullptr) {
        return out;
    }

    const AudioDeviceContextStats stats = context_->Stats();
    out.activeVoices = stats.activeVoices;
    out.releasingVoices = stats.releasingVoices;
    out.registeredClips = stats.registeredClips;
    out.pendingDeferred = stats.pendingDeferred;
    out.deferredPlays = stats.deferredPlays;
    out.voiceLimitRejects = stats.voiceLimitRejects;
    out.failedPlays = stats.failedPlays;
    out.masterGainCommands = stats.masterGainCommands;
    return out;
}

Result<void> AudioEngineSubsystem::ValidateOptions(const SoundSourceRef& sound,
                                                   const PlayOptions& options) const noexcept {
    const SoundPlayable* playable = sound.Get();
    if (playable == nullptr || !InRange(options.volume, 0.0f, audio::MaxVoiceGain) ||
        !InRange(options.pitch, audio::MinPitch, audio::MaxPitch) ||
        options.startFrame >= playable->pcm->FrameCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (playable->looping && playable->loopEnd != 0 && options.startFrame >= playable->loopEnd) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<PlaybackHandle> AudioEngineSubsystem::Accept(const SoundSourceRef& sound,
                                                    PlayOptions& options, uint32 scene) noexcept {
    using Accepted = Result<PlaybackHandle>;
    if (!accepting_) {
        return Accepted::Failure(ErrorCode::ShuttingDown);
    }

    if (auto valid = ValidateOptions(sound, options); !valid) {
        return Accepted::Failure(valid.Error());
    }

    if (freeSlots_.IsEmpty()) {
        return Accepted::Failure(ErrorCode::CapacityExceeded);
    }

    const uint32 slot = freeSlots_[freeSlots_.Size() - 1];
    GamePlayback& playback = playbacks_[slot];
    const PlaybackHandle handle{slot, playback.generation};
    if (control_ == nullptr) {
        // 장치 없음: 유효 handle을 주고 다음 프레임에 Ended(Ignored)를 낸다(3.8.3절).
        (void)ignored_.TryPushBack(handle);
        playback.status = PlaybackStatus::Ignored;
    } else {
        ControlCommand command;
        command.type = ControlCommandType::Play;
        command.handle = handle;
        command.sound = sound;
        command.scene = scene;
        command.volume = options.volume;
        command.pitch = options.pitch;
        command.startFrame = options.startFrame;
        command.fadeInFrames = options.fadeInFrames;
        if (!current_->commands.TryPushBack(Move(command))) {
            return Accepted::Failure(ErrorCode::OutOfMemory);
        }

        playback.status = PlaybackStatus::Pending;
    }

    (void)freeSlots_.TryPopBack();
    playback.live = true;
    playback.onEvent = Move(options.onEvent);
    return Accepted::Success(handle);
}

AudioEngineSubsystem::GamePlayback* AudioEngineSubsystem::FindLive(PlaybackHandle handle) noexcept {
    if (handle.slot >= playbacks_.Size()) {
        return nullptr;
    }

    GamePlayback& playback = playbacks_[handle.slot];
    return playback.live && playback.generation == handle.generation ? &playback : nullptr;
}

const AudioEngineSubsystem::GamePlayback*
AudioEngineSubsystem::FindLive(PlaybackHandle handle) const noexcept {
    if (handle.slot >= playbacks_.Size()) {
        return nullptr;
    }

    const GamePlayback& playback = playbacks_[handle.slot];
    return playback.live && playback.generation == handle.generation ? &playback : nullptr;
}

void AudioEngineSubsystem::Deliver(const PlaybackEvent& event) noexcept {
    GamePlayback* playback = FindLive(event.handle);
    if (playback == nullptr) {
        return;
    }

    if (event.kind == PlaybackEventKind::Started) {
        if (playback->status == PlaybackStatus::Pending) {
            playback->status = PlaybackStatus::Playing;
        }

        if (playback->onEvent) {
            playback->onEvent(event);
        }

        return;
    }

    playback->status = PlaybackStatus::Finished;
    // 콜백이 같은 슬롯을 건드려도 되도록 먼저 꺼낸다. 슬롯은 콜백이 돌아온 뒤 놓는다.
    PlaybackEventCallback callback = Move(playback->onEvent);
    if (callback) {
        callback(event);
    }

    FreeSlot(event.handle.slot);
}

void AudioEngineSubsystem::FreeSlot(uint32 slot) noexcept {
    GamePlayback& playback = playbacks_[slot];
    playback.live = false;
    playback.onEvent = PlaybackEventCallback{};
    playback.parameterBatch = 0;
    ++playback.generation;
    if (playback.generation == 0) {
        playback.generation = 1;
    }

    (void)freeSlots_.TryPushBack(slot);
}

float AudioEngineSubsystem::MasterGainTarget(Engine& engine) const noexcept {
    using namespace engine_audio_detail;
    if (traits_.audioMuted) {
        return 0.0f;
    }

    float volume = 1.0f;
    float unfocused = 0.0f;
    if (engine.Settings() != nullptr) {
        volume = MasterVolumeVar.Get();
        unfocused = UnfocusedVolumeVar.Get();
    }

    return volume * (foreground_ ? 1.0f : unfocused);
}

void AudioEngineSubsystem::SubmitCurrent() noexcept {
    if (freeBatches_.IsEmpty()) {
        ++stats_.deferredFrames;
        return;
    }

    ControlBatch* batch = current_;
    auto task =
        Function<void()>::TryCreate(allocator_, [this, batch]() noexcept { ProcessBatch(batch); });
    if (!task) {
        ++stats_.postFailures;
        return;
    }

    // Inline 방식은 Post 안에서 바로 처리하고 반환 큐에 넣는다.
    if (!control_->lane.Post(Move(task.Value()))) {
        ++stats_.postFailures;
        return;
    }

    current_ = freeBatches_[freeBatches_.Size() - 1];
    (void)freeBatches_.TryPopBack();
    ++batchSerial_;
    ++stats_.submittedBatches;
}

void AudioEngineSubsystem::ProcessBatch(ControlBatch* batch) noexcept {
    // 제어 pipe 스레드(Inline이면 게임 스레드)다. 묶음은 반환 큐에 넣을 때까지 이쪽만 만진다.
    context_->Process(*batch);
    const bool pushed = returned_.TryPush(batch);
    ROCKS_ASSERT(pushed);
    (void)pushed;
}

void AudioEngineSubsystem::Drain() noexcept {
    if (drained_) {
        return;
    }

    drained_ = true;
    accepting_ = false;
    if (control_ != nullptr && control_->laneStarted) {
        // 보낸 묶음을 모두 처리시킨다. 이 뒤로 제어 쪽 작업이 없으므로 게임 스레드가 context를
        // 쓴다.
        (void)control_->lane.Shutdown();
        control_->laneStarted = false;
        ControlBatch* batch = nullptr;
        while (returned_.TryPop(batch)) {
            batch->Recycle();
        }

        if (context_ != nullptr && current_ != nullptr) {
            context_->Process(*current_);
            current_->Recycle();
            context_->StopAll(audio::DefaultStopFadeFrames);
            const audio::AudioOutputDeviceInfo& info = device_->Info();
            const bool manualNull =
                info.usingNull && options_.nullMode == audio::NullAudioOutputMode::Manual;
            if (options_.renderMode == audio::AudioRenderMode::Thread && !manualNull) {
                // 꼬리 대기(3.35.9절 4단계): voice가 모두 놓이고, 출력 링이 렌더한 frame을 다 낼
                // 때까지. 상한을 넘어도 아래 Close가 수명 장벽이다.
                const uint64 deadline = platform::MonotonicNanoseconds() +
                                        ShutdownDrainTimeoutMs * NanosecondsPerMillisecond;
                bool idle = false;
                while (platform::MonotonicNanoseconds() < deadline) {
                    if (!idle) {
                        context_->Process(*current_);
                        current_->Recycle();
                        idle = context_->IsIdle();
                    }

                    if (idle && device_->RingCountersSnapshot().consumedFrames >=
                                    device_->Mixer().CompletedMixFrame()) {
                        break;
                    }

                    platform::SleepFor(ShutdownDrainPollMs);
                }
            }
        }
    }

    if (device_ != nullptr) {
        device_->Close();
    }

    if (context_ != nullptr) {
        context_->Abandon();
    }

    // 종료 뒤에는 콜백을 부르지 않는다(UE도 종료 때 완료 delegate를 부르지 않는다).
    for (GamePlayback& playback : playbacks_) {
        playback.onEvent = PlaybackEventCallback{};
        playback.live = false;
    }

    ignored_.Clear();
    delivering_.Clear();
}

EngineSubsystemType AudioEngineSubsystemType() noexcept {
    EngineSubsystemType type;
    type.key = SubsystemKeyOf<AudioEngineSubsystem>();
    type.name = "Audio";
    type.create = [](Allocator& allocator) noexcept -> Result<UniquePtr<EngineSubsystem>> {
        auto created = UniquePtr<AudioEngineSubsystem>::TryMake(allocator, allocator);
        if (!created) {
            return Result<UniquePtr<EngineSubsystem>>::Failure(created.Error());
        }

        return Result<UniquePtr<EngineSubsystem>>::Success(
            UniquePtr<EngineSubsystem>(Move(created.Value())));
    };
    return type;
}

} // namespace rocks
