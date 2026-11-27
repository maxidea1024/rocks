#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Engine/Engine.h>
#include <EngineAudio/AudioEngineSubsystem.h>
#include <ModuleLoader/HostServices.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/Test.h>

// EngineAudio 시험 공용 준비다(Docs/Design/AudioSystem 3.35.12절): Inline 제어 + Manual render +
// Null Manual 장치로 프레임 진행·Pump만으로 순서를 만든다(시간 대기 없음).
namespace rocks::engine_audio_test {

inline constexpr uint32 TestRate = audio::DefaultMixSampleRate;
// 프레임마다 장치 콜백이 가져가는 frame 수다(블록 256의 배수).
inline constexpr uint32 PumpFrames = 512;
inline constexpr uint32 MaxTestFrames = 64;

inline AudioEngineHostOptions DeterministicOptions(bool output, bool muted = false) noexcept {
    AudioEngineHostOptions options;
    options.controlMode = AudioControlMode::Inline;
    options.renderMode = audio::AudioRenderMode::Manual;
    options.nullMode = audio::NullAudioOutputMode::Manual;
    options.useBackend = false;
    AudioRunTraits traits;
    traits.audioOutput = output;
    traits.audioMuted = muted;
    options.traits = traits;
    return options;
}

// 호스트 선택값을 엔진 수명 동안만 등록한다(HostServices는 프로세스 공유 — 3.35.9절).
class HostOptionsScope {
public:
    explicit HostOptionsScope(const AudioEngineHostOptions& options) noexcept : options_(options) {
        registered_ = test::ProcessModules().Services().Register(options_).IsOk();
    }

    ~HostOptionsScope() noexcept {
        if (registered_) {
            (void)test::ProcessModules().Services().Unregister(options_);
        }
    }

    HostOptionsScope(const HostOptionsScope&) = delete;
    HostOptionsScope& operator=(const HostOptionsScope&) = delete;

    bool Registered() const noexcept {
        return registered_;
    }

private:
    AudioEngineHostOptions options_;
    bool registered_ = false;
};

// 장치 콜백을 frames만큼 부른다. Null Pump는 콜백 크기 이하만 받으므로 나눠 부르고, 그 사이에
// 렌더해 링을 채운다.
inline void PumpDevice(AudioEngineSubsystem* audio, uint32 frames) noexcept {
    audio::AudioOutputDevice* device = audio->Device();
    if (device == nullptr) {
        return;
    }

    device->RenderAvailable();
    const uint32 callback = device->Info().format.callbackFrames;
    while (frames != 0 && device->NullOutput() != nullptr) {
        const uint32 chunk = frames < callback ? frames : callback;
        device->NullOutput()->Pump(chunk);
        device->RenderAvailable();
        frames -= chunk;
    }
}

struct AudioEngine {
    // engineAllocator가 있으면 엔진(과 서브시스템)이 그것으로 할당한다(할당 계수 시험).
    explicit AudioEngine(const AudioEngineHostOptions& options,
                         config::ConfigContext* settings = nullptr,
                         Allocator* engineAllocator = nullptr) noexcept
        : scope(options), engine(engineAllocator != nullptr ? *engineAllocator : allocator) {
        subsystems[0] = AudioEngineSubsystemType();
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.subsystems = Span<const EngineSubsystemType>(subsystems, 1);
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.settings = settings;
        initialized = scope.Registered() && engine.Initialize(config).IsOk();
        audio = initialized ? engine.GetSubsystem<AudioEngineSubsystem>() : nullptr;
    }

    ~AudioEngine() noexcept {
        (void)engine.Shutdown();
    }

    // 프레임 하나: 엔진 프레임(결과 전달 → 묶음 처리) → 렌더 → 장치 콜백 소비.
    bool Frame(uint32 pumpFrames = PumpFrames) noexcept {
        if (!engine.RunFrame()) {
            return false;
        }

        PumpDevice(audio, pumpFrames);
        return true;
    }

    bool Frames(uint32 count) noexcept {
        for (uint32 frame = 0; frame < count; ++frame) {
            if (!Frame()) {
                return false;
            }
        }

        return true;
    }

    // 제어 쪽이 voice·clip을 모두 놓을 때까지 프레임을 돈다.
    bool RunUntilIdle() noexcept {
        for (uint32 frame = 0; frame < MaxTestFrames; ++frame) {
            const AudioControlStats stats = audio->ControlStats();
            if (stats.activeVoices == 0 && stats.releasingVoices == 0 &&
                stats.registeredClips == 0) {
                return true;
            }

            if (!Frame()) {
                return false;
            }
        }

        return false;
    }

    SystemAllocator allocator;
    EngineSubsystemType subsystems[1];
    HostOptionsScope scope;
    Engine engine;
    AudioEngineSubsystem* audio = nullptr;
    bool initialized = false;
};

// 값이 모두 value인 mono PCM이다.
inline SharedPtr<const SoundPcm> MakeConstantPcm(Allocator& allocator, float value,
                                                 uint32 frames) noexcept {
    auto pcm = SoundPcm::TryAllocate(allocator, 1, frames, TestRate);
    if (!pcm) {
        return {};
    }

    for (float& sample : pcm.Value()->MutableChannel(0)) {
        sample = value;
    }

    return SharedPtr<const SoundPcm>(Move(pcm.Value()));
}

// frame i의 값이 i × step인 mono PCM이다(재생 구간을 값으로 가린다).
inline SharedPtr<const SoundPcm> MakeRampPcm(Allocator& allocator, float step,
                                             uint32 frames) noexcept {
    auto pcm = SoundPcm::TryAllocate(allocator, 1, frames, TestRate);
    if (!pcm) {
        return {};
    }

    Span<float> samples = pcm.Value()->MutableChannel(0);
    for (uint32 frame = 0; frame < frames; ++frame) {
        samples[frame] = static_cast<float>(frame) * step;
    }

    return SharedPtr<const SoundPcm>(Move(pcm.Value()));
}

inline SoundSourceRef MakeSound(Allocator& allocator, const SharedPtr<const SoundPcm>& pcm,
                                bool looping, uint32 loopBegin = 0, uint32 loopEnd = 0,
                                const Optional<SoundAttenuation>& attenuation = {}) noexcept {
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.looping = looping;
    playable.loopBegin = loopBegin;
    playable.loopEnd = loopEnd;
    playable.attenuation = attenuation;
    auto made = SoundSourceRef::TryCreate(allocator, playable);
    return made ? made.Value() : SoundSourceRef{};
}

// 결과 콜백이 쓰는 기록이다.
struct EventLog {
    explicit EventLog(Allocator& allocator) noexcept : events(allocator) {
    }

    PlaybackEventCallback Callback(Allocator& allocator) noexcept {
        auto made = PlaybackEventCallback::TryCreate(
            allocator,
            [this](const PlaybackEvent& event) noexcept { (void)events.TryPushBack(event); });
        return made ? Move(made.Value()) : PlaybackEventCallback{};
    }

    uint32 Count(PlaybackEventKind kind) const noexcept {
        uint32 count = 0;
        for (const PlaybackEvent& event : events) {
            count += event.kind == kind ? 1 : 0;
        }

        return count;
    }

    Array<PlaybackEvent> events;
};

inline float MaxAbs(Span<const float> samples) noexcept {
    float peak = 0.0f;
    for (const float sample : samples) {
        peak = platform::PlatformMath::Max(peak, platform::PlatformMath::Abs(sample));
    }

    return peak;
}

// 교차 stereo capture의 채널별 절댓값 최대다.
struct StereoPeak {
    float left = 0.0f;
    float right = 0.0f;
};

inline StereoPeak PeakOf(Span<const float> interleaved) noexcept {
    StereoPeak peak;
    for (usize frame = 0; frame + 1 < interleaved.Size(); frame += 2) {
        peak.left =
            platform::PlatformMath::Max(peak.left, platform::PlatformMath::Abs(interleaved[frame]));
        peak.right = platform::PlatformMath::Max(
            peak.right, platform::PlatformMath::Abs(interleaved[frame + 1]));
    }

    return peak;
}

// capture 버퍼를 지정하고 frames만큼 프레임을 돌린 뒤 채널별 최대를 낸다.
inline StereoPeak CapturePeak(AudioEngine& fixture, Array<float>& buffer) noexcept {
    fixture.audio->Device()->NullOutput()->CaptureTo(Span<float>(buffer.Data(), buffer.Size()));
    (void)fixture.Frame();
    return PeakOf(Span<const float>(buffer.Data(), buffer.Size()));
}

} // namespace rocks::engine_audio_test
