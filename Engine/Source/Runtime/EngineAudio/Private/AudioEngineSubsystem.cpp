#include "AudioDeviceContext.h"
#include "AudioEngineSettingsInternal.h"
#include "ControlBatch.h"
#include "ValueRange.h"

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
      delivering_(allocator), scenes_(allocator), sceneEpochs_(allocator), emitters_(allocator),
      freeEmitters_(allocator), pendingSceneRemovals_(allocator), freeBatches_(allocator) {
}

AudioEngineSubsystem::~AudioEngineSubsystem() {
    // Engine은 늘 Deinitialize를 부른다. 초기화 도중 실패도 Initialize가 스스로 되돌린다.
    Drain();
}

Result<void> AudioEngineSubsystem::Initialize(Engine& engine) noexcept {
    options_ = AudioEngineHostOptions{};
    hasSettings_ = engine.Settings() != nullptr;
    decoders_ = audio::AudioDecoderTable{};
    if (auto builtIn = audio::RegisterBuiltInAudioDecoders(decoders_); !builtIn) {
        return builtIn;
    }

    if (ModuleLoader* modules = engine.Modules()) {
        if (const auto* hostOptions = modules->Services().Find<AudioEngineHostOptions>()) {
            options_ = *hostOptions;
        }

        if (const auto* registry = modules->Services().Find<audio::AudioDecoderRegistry>()) {
            registry->Fill(decoders_);
        }
    }

    traits_ = options_.traits.HasValue() ? options_.traits.Value() : CurrentAudioRunTraits();
    if (!playbacks_.TryResize(MaxPlaybacks) || !freeSlots_.TryReserve(MaxPlaybacks) ||
        !ignored_.TryReserve(MaxPlaybacks) || !delivering_.TryReserve(DeliveringCapacity) ||
        !freeBatches_.TryReserve(ControlBatchCount) || !scenes_.TryResize(MaxAudioScenes) ||
        !sceneEpochs_.TryResize(MaxAudioScenes) || !emitters_.TryResize(MaxAudioEmitters) ||
        !freeEmitters_.TryReserve(MaxAudioEmitters) ||
        !pendingSceneRemovals_.TryReserve(MaxAudioScenes)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (uint32& epoch : sceneEpochs_) {
        epoch = 0;
    }

    // 엔진 scene은 종료까지 산다(엔진 소유 재생).
    scenes_[EngineSceneIndex].epoch = EngineSceneEpoch;
    sceneEpochs_[EngineSceneIndex] = EngineSceneEpoch;
    for (uint32 slot = MaxAudioEmitters; slot > 0; --slot) {
        (void)freeEmitters_.TryPushBack(slot - 1);
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

    // 스트림 디코드 서비스(3.40.2절): 제어가 Inline이면 서비스도 Inline이다(제어 묶음이 펌프한다).
    int32 ringSetting = int32{audio::StreamRingMinFrames};
    int32 streamsSetting = 0;
    if (engine.Settings() != nullptr) {
        ringSetting = StreamRingFramesVar.Get();
        streamsSetting = StreamMaxStreamsVar.Get();
    }

    audio::AudioStreamServiceConfig streamConfig;
    streamConfig.ringFrames = static_cast<uint32>(ringSetting);
    const uint32 defaultStreams = maxVoices * MixerVoiceHeadroomFactor;
    const uint32 cappedStreams =
        defaultStreams < audio::MaxStreamSlots ? defaultStreams : audio::MaxStreamSlots;
    streamConfig.maxStreams =
        streamsSetting > 0 ? static_cast<uint32>(streamsSetting) : cappedStreams;
    if (options_.streamSlots != 0) {
        streamConfig.maxStreams = options_.streamSlots;
    }

    streamConfig.mode = options_.controlMode == AudioControlMode::Inline
                            ? audio::AudioStreamServiceMode::Inline
                            : audio::AudioStreamServiceMode::Thread;
    auto streams = audio::AudioStreamService::TryCreate(allocator_, streamConfig);
    if (!streams) {
        Log::Error(LogEngineAudio, "Audio stream service creation failed ({Error})",
                   ErrorCodeName(streams.Error()));
        return Result<void>::Failure(streams.Error());
    }

    streams_ = Move(streams.Value());
    mixerConfig.streamRefillSignal = &streams_->WakeSignal();

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
    if (options_.commandCapacity != 0) {
        deviceOptions.commandCapacity = options_.commandCapacity;
    }
    auto opened = audio::AudioOutputDevice::TryOpen(mixerConfig, Move(backend), request,
                                                    deviceOptions, allocator_);
    if (!opened) {
        Log::Error(LogEngineAudio, "Audio device open failed ({Error})",
                   ErrorCodeName(opened.Error()));
        return Result<void>::Failure(opened.Error());
    }

    device_ = Move(opened.Value());
    if (auto started = streams_->Start(); !started) {
        Log::Error(LogEngineAudio, "Audio decode thread start failed ({Error})",
                   ErrorCodeName(started.Error()));
        return started;
    }

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

    auto context = UniquePtr<AudioDeviceContext>::TryMake(allocator_, allocator_, *device_,
                                                          streams_.Get(), &decoders_);
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

            lastControlStats_ = batch->stats;

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
    SnapshotScenes();
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
    return Accept(sound, options, AudioSceneId{EngineSceneIndex, EngineSceneEpoch}, Placement{});
}

Result<PlaybackHandle> AudioEngineSubsystem::Play2DInScene(AudioSceneId scene,
                                                           const SoundSourceRef& sound,
                                                           PlayOptions options) noexcept {
    return Accept(sound, options, scene, Placement{});
}

Result<PlaybackHandle> AudioEngineSubsystem::PlayAtInScene(AudioSceneId scene,
                                                           const SoundSourceRef& sound,
                                                           const Vec3d& position,
                                                           PlayOptions options) noexcept {
    if (!position.IsFinite()) {
        return Result<PlaybackHandle>::Failure(ErrorCode::InvalidArgument);
    }

    Placement placement;
    placement.kind = PlayPlacement::Fixed;
    placement.position = position;
    return Accept(sound, options, scene, placement);
}

Result<PlaybackHandle> AudioEngineSubsystem::PlayAttachedInScene(AudioSceneId scene,
                                                                 AudioEmitterId emitter,
                                                                 const SoundSourceRef& sound,
                                                                 PlayOptions options) noexcept {
    if (!emitter.IsValid()) {
        return Result<PlaybackHandle>::Failure(ErrorCode::InvalidArgument);
    }

    Placement placement;
    placement.kind = PlayPlacement::Emitter;
    placement.emitter = emitter;
    return Accept(sound, options, scene, placement);
}

AudioSceneId AudioEngineSubsystem::RegisterScene() noexcept {
    if (scenes_.IsEmpty()) {
        return {};
    }

    for (uint32 index = 0; index < MaxAudioScenes; ++index) {
        if (index == EngineSceneIndex || scenes_[index].epoch != 0) {
            continue;
        }

        uint32 epoch = sceneEpochs_[index] + 1;
        if (epoch == 0) {
            epoch = 1;
        }

        sceneEpochs_[index] = epoch;
        GameScene& scene = scenes_[index];
        scene.epoch = epoch;
        scene.listenerPosition = Vec3d();
        scene.listenerRotation = Quatd::Identity();
        // 제어 쪽에 새 epoch를 알린다(다음 묶음 스냅숏).
        scene.listenerDirty = true;
        return AudioSceneId{index, epoch};
    }

    return {};
}

void AudioEngineSubsystem::UnregisterScene(AudioSceneId scene) noexcept {
    if (!IsLiveScene(scene) || scene.index == EngineSceneIndex) {
        return;
    }

    scenes_[scene.index] = GameScene{};
    // 콜백을 이 자리에서 끊는다. 슬롯은 제어 쪽 Ended(또는 Ignored 끝)가 온 뒤 놓는다.
    for (GamePlayback& playback : playbacks_) {
        if (playback.live && playback.scene == scene.index && playback.sceneEpoch == scene.epoch) {
            playback.onEvent = PlaybackEventCallback{};
            playback.status = PlaybackStatus::Finished;
        }
    }

    if (control_ != nullptr && !drained_) {
        // 명령은 프레임 끝(SnapshotScenes)에 넣는다. 할당이 실패하면 다음 프레임에 다시 보낸다(남은
        // 재생은 Status가 Finished라 게임이 멈출 수 없으므로 해제를 잃으면 안 된다). 칸은 scene
        // 수만큼 미리 잡았다.
        (void)pendingSceneRemovals_.TryPushBack(scene);
    }

    for (uint32 slot = 0; slot < emitters_.Size(); ++slot) {
        const GameEmitter& emitter = emitters_[slot];
        if (emitter.live && emitter.scene == scene.index && emitter.sceneEpoch == scene.epoch) {
            UnregisterEmitter(AudioEmitterId{slot, emitter.generation});
        }
    }
}

void AudioEngineSubsystem::SetSceneListener(AudioSceneId scene, const Vec3d& position,
                                            const Quatd& rotation) noexcept {
    auto normalized = rotation.TryNormalize();
    if (!IsLiveScene(scene) || !position.IsFinite() || !normalized) {
        return;
    }

    GameScene& target = scenes_[scene.index];
    target.listenerPosition = position;
    target.listenerRotation = normalized.Value();
    target.listenerDirty = true;
}

Result<AudioEmitterId> AudioEngineSubsystem::RegisterEmitter(AudioSceneId scene,
                                                             const Vec3d& position) noexcept {
    using Registered = Result<AudioEmitterId>;
    if (!IsLiveScene(scene) || !position.IsFinite()) {
        return Registered::Failure(ErrorCode::InvalidArgument);
    }

    if (freeEmitters_.IsEmpty()) {
        if (!emitterFullWarned_) {
            emitterFullWarned_ = true;
            Log::Warning(LogEngineAudio,
                         "Audio emitter table is full ({Capacity}); new audio components play at a "
                         "fixed location",
                         MaxAudioEmitters);
        }

        return Registered::Failure(ErrorCode::CapacityExceeded);
    }

    const uint32 slot = freeEmitters_[freeEmitters_.Size() - 1];
    (void)freeEmitters_.TryPopBack();
    GameEmitter& emitter = emitters_[slot];
    emitter.live = true;
    emitter.scene = scene.index;
    emitter.sceneEpoch = scene.epoch;
    emitter.position = position;
    emitter.updateBatch = 0;
    PushEmitterUpdate(slot);
    return Registered::Success(AudioEmitterId{slot, emitter.generation});
}

void AudioEngineSubsystem::UpdateEmitter(AudioEmitterId id, const Vec3d& position) noexcept {
    if (id.slot >= emitters_.Size() || !position.IsFinite()) {
        return;
    }

    GameEmitter& emitter = emitters_[id.slot];
    if (!emitter.live || emitter.generation != id.generation) {
        return;
    }

    emitter.position = position;
    PushEmitterUpdate(id.slot);
}

void AudioEngineSubsystem::UnregisterEmitter(AudioEmitterId id) noexcept {
    if (id.slot >= emitters_.Size()) {
        return;
    }

    GameEmitter& emitter = emitters_[id.slot];
    if (!emitter.live || emitter.generation != id.generation) {
        return;
    }

    if (control_ != nullptr && !drained_) {
        ControlCommand command;
        command.type = ControlCommandType::RemoveEmitter;
        command.emitter = id;
        (void)current_->commands.TryPushBack(Move(command));
    }

    emitter.live = false;
    emitter.updateBatch = 0;
    ++emitter.generation;
    if (emitter.generation == 0) {
        emitter.generation = 1;
    }

    (void)freeEmitters_.TryPushBack(id.slot);
}

bool AudioEngineSubsystem::IsPlaybackInScene(PlaybackHandle handle,
                                             AudioSceneId scene) const noexcept {
    const GamePlayback* playback = FindLive(handle);
    return playback != nullptr && playback->scene == scene.index &&
           playback->sceneEpoch == scene.epoch;
}

bool AudioEngineSubsystem::IsLiveScene(AudioSceneId scene) const noexcept {
    return scene.IsValid() && scene.index < scenes_.Size() &&
           scenes_[scene.index].epoch == scene.epoch;
}

void AudioEngineSubsystem::PushEmitterUpdate(uint32 slot) noexcept {
    if (control_ == nullptr || drained_) {
        return;
    }

    GameEmitter& emitter = emitters_[slot];
    // 같은 묶음 안에서는 최신 위치 하나로 합친다.
    if (emitter.updateBatch == batchSerial_ && emitter.updateIndex != 0) {
        current_->emitters[emitter.updateIndex - 1].position = emitter.position;
        return;
    }

    EmitterUpdate update;
    update.emitter = AudioEmitterId{slot, emitter.generation};
    update.position = emitter.position;
    if (current_->emitters.TryPushBack(update)) {
        emitter.updateBatch = batchSerial_;
        emitter.updateIndex = static_cast<uint32>(current_->emitters.Size());
    }
}

void AudioEngineSubsystem::SnapshotScenes() noexcept {
    usize sent = 0;
    for (const AudioSceneId removed : pendingSceneRemovals_) {
        ControlCommand command;
        command.type = ControlCommandType::RemoveScene;
        command.scene = removed.index;
        command.sceneEpoch = removed.epoch;
        if (!current_->commands.TryPushBack(Move(command))) {
            break;
        }

        ++sent;
    }

    if (sent != 0) {
        (void)pendingSceneRemovals_.TryRemoveAt(0, sent);
    }

    for (uint32 index = 0; index < scenes_.Size(); ++index) {
        GameScene& scene = scenes_[index];
        if (scene.epoch == 0 || !scene.listenerDirty) {
            continue;
        }

        SceneUpdate update;
        update.index = index;
        update.epoch = scene.epoch;
        update.position = scene.listenerPosition;
        update.rotation = scene.listenerRotation;
        // 묶음을 보내지 못한 프레임이 이어져도 scene마다 하나만 둔다.
        bool merged = false;
        for (SceneUpdate& existing : current_->scenes) {
            if (existing.index == index) {
                existing = update;
                merged = true;
                break;
            }
        }

        if (merged || current_->scenes.TryPushBack(update)) {
            scene.listenerDirty = false;
        }
    }
}

Result<void> AudioEngineSubsystem::Stop(PlaybackHandle handle, uint32 fadeFrames) noexcept {
    if (!handle.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    GamePlayback* playback = FindLive(handle);
    if (playback == nullptr || playback->status == PlaybackStatus::Ignored ||
        playback->status == PlaybackStatus::Finished || drained_) {
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
    const bool valid =
        parameter == PlaybackParameter::Volume
            ? engine_audio_detail::IsFiniteInRange(value, 0.0f, audio::MaxVoiceGain)
            : engine_audio_detail::IsFiniteInRange(value, audio::MinPitch, audio::MaxPitch);
    if (!handle.IsValid() || !valid) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    GamePlayback* playback = FindLive(handle);
    if (playback == nullptr || playback->status == PlaybackStatus::Ignored ||
        playback->status == PlaybackStatus::Finished || drained_) {
        return Result<void>::Success();
    }

    // 같은 묶음 안의 같은 파라미터는 최신값 하나로 합친다(3.35.5절).
    const uint32 kind = PlaybackParameterIndex(parameter);
    if (playback->parameterBatch == batchSerial_ && playback->parameterCommand[kind] != 0) {
        ControlCommand& merged = current_->commands[playback->parameterCommand[kind] - 1];
        merged.value = value;
        merged.rampFrames = rampFrames;
        ++stats_.mergedParameters;
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
        for (uint32& index : playback->parameterCommand) {
            index = 0;
        }
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

AudioControlStats AudioEngineSubsystem::ControlStats() const noexcept {
    if (context_ == nullptr) {
        return AudioControlStats{};
    }

    // Inline 방식·drain 뒤에는 제어 쪽이 이 스레드뿐이라 바로 읽는다.
    if (drained_ || options_.controlMode == AudioControlMode::Inline) {
        return context_->Stats();
    }

    return lastControlStats_;
}

Result<void> AudioEngineSubsystem::ValidateOptions(const SoundSourceRef& sound,
                                                   const PlayOptions& options) const noexcept {
    const SoundPlayable* playable = sound.Get();
    if (playable == nullptr ||
        !engine_audio_detail::IsFiniteInRange(options.volume, 0.0f, audio::MaxVoiceGain) ||
        !engine_audio_detail::IsFiniteInRange(options.pitch, audio::MinPitch, audio::MaxPitch) ||
        options.startFrame >= playable->FrameCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (playable->looping && playable->loopEnd != 0 && options.startFrame >= playable->loopEnd) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<PlaybackHandle> AudioEngineSubsystem::Accept(const SoundSourceRef& sound,
                                                    PlayOptions& options, AudioSceneId scene,
                                                    const Placement& placement) noexcept {
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
    bool live = IsLiveScene(scene);
    if (live && placement.kind == PlayPlacement::Emitter) {
        const AudioEmitterId id = placement.emitter;
        live = id.slot < emitters_.Size() && emitters_[id.slot].live &&
               emitters_[id.slot].generation == id.generation;
    }

    if (control_ == nullptr || !live) {
        // 장치 없음: 유효 handle을 주고 다음 프레임에 Ended(Ignored)를 낸다(3.8.3절).
        (void)ignored_.TryPushBack(handle);
        playback.status = PlaybackStatus::Ignored;
    } else {
        ControlCommand command;
        command.type = ControlCommandType::Play;
        command.handle = handle;
        command.sound = sound;
        command.scene = scene.index;
        command.sceneEpoch = scene.epoch;
        command.placement = placement.kind;
        command.position = placement.position;
        command.emitter = placement.emitter;
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
    playback.scene = scene.index;
    playback.sceneEpoch = scene.epoch;
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

        // 콜백을 꺼내서 부른다. 콜백이 World를 즉시 파괴하면(Tick 밖 DestroyWorld) scene 해제가 이
        // 슬롯의 onEvent를 비우므로, 제자리에서 부르면 실행 중인 콜백 객체가 파괴된다.
        PlaybackEventCallback callback = Move(playback->onEvent);
        if (!callback) {
            return;
        }

        callback(event);
        // 그 사이 scene 해제·종료로 끊겼으면 되돌리지 않는다(Ended를 위해 다시 둔다).
        GamePlayback* again = FindLive(event.handle);
        if (again != nullptr && again->status != PlaybackStatus::Finished && !again->onEvent) {
            again->onEvent = Move(callback);
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
    if (options_.beforeControlBatch != nullptr) {
        options_.beforeControlBatch(options_.beforeControlBatchContext);
    }

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
        const bool stopped = control_->lane.Shutdown().IsOk();
        // 게임 스레드에서만 부르므로 WrongThread가 될 수 없다. 실패하면 lane이 도는 채로 context를
        // 쓰게 된다.
        ROCKS_ASSERT(stopped);
        (void)stopped;
        control_->laneStarted = false;
        ControlBatch* batch = nullptr;
        while (returned_.TryPop(batch)) {
            batch->Recycle();
        }

        if (context_ != nullptr && current_ != nullptr) {
            // 아직 보내지 못한 scene 해제를 마지막 묶음에 싣는다.
            SnapshotScenes();
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
                uint64 target = 0;
                while (platform::MonotonicNanoseconds() < deadline) {
                    if (!idle) {
                        context_->Process(*current_);
                        current_->Recycle();
                        idle = context_->IsIdle();
                        // AudioRT는 빈 칸마다 무음 블록도 계속 렌더하므로 목표는 idle을 본 순간 한
                        // 번 잡는다(그 뒤 렌더한 것은 무음이다).
                        target = device_->Mixer().CompletedMixFrame();
                    }

                    if (idle && IsOutputTailPlayed(device_->RingCountersSnapshot(), target)) {
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

    // 종료 순서(3.40.2절): 믹서 drain·장치 Close 뒤 디코드 스레드를 멈추고(활성 작업은 반납) 참조를
    // 놓는다.
    if (streams_ != nullptr) {
        streams_->Shutdown();
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

SoundCookedLoadOptions AudioEngineSubsystem::CookedLoadOptions() const noexcept {
    SoundCookedLoadOptions options;
    if (hasSettings_) {
        options.defaultLoading =
            static_cast<SoundLoadingBehavior>(engine_audio_detail::StreamDefaultLoadingVar.Get());
    }

    return options;
}

bool IsOutputTailPlayed(const audio::AudioOutputRingCounters& counters,
                        uint64 renderedFrames) noexcept {
    const uint64 played = counters.consumedFrames >= counters.underrunFrames
                              ? counters.consumedFrames - counters.underrunFrames
                              : 0;
    return played >= renderedFrames;
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
