#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <Engine/Engine.h>
#include <EngineAudio/AttenuationSettings.generated.h>
#include <EngineAudio/AudioEngineSubsystem.h>
#include <EngineAudio/EngineAudioTypes.h>
#include <EngineAudio/SoundClip.generated.h>
#include <EngineAudio/SoundSource.generated.h>
#include <ModuleLoader/HostServices.h>
#include <Object/ObjectRegistry.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/Test.h>

// AU3a1 시험(Docs/Design/AudioSystem 3.35.12절): Inline 제어 + Manual render + Null Manual 장치로
// 프레임 진행·Pump만으로 순서를 만든다(시간 대기 없음).
using namespace rocks;
namespace audio_engine_tests_file {

namespace {

constexpr uint32 TestRate = audio::DefaultMixSampleRate;
// 프레임마다 장치 콜백이 가져가는 frame 수다(블록 256의 배수).
constexpr uint32 PumpFrames = 512;
constexpr uint32 MaxTestFrames = 64;

AudioEngineHostOptions DeterministicOptions(bool output, bool muted = false) {
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
    explicit HostOptionsScope(const AudioEngineHostOptions& options) : options_(options) {
        registered_ = test::ProcessModules().Services().Register(options_).IsOk();
    }

    ~HostOptionsScope() {
        if (registered_) {
            (void)test::ProcessModules().Services().Unregister(options_);
        }
    }

    bool Registered() const noexcept {
        return registered_;
    }

private:
    AudioEngineHostOptions options_;
    bool registered_ = false;
};

class FailingSubsystem final : public EngineSubsystem {
public:
    Result<void> Initialize(Engine&) noexcept override {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
};

struct AudioEngine {
    explicit AudioEngine(const AudioEngineHostOptions& options)
        : scope(options), engine(allocator) {
        subsystems[0] = AudioEngineSubsystemType();
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.subsystems = Span<const EngineSubsystemType>(subsystems, 1);
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        initialized = scope.Registered() && engine.Initialize(config).IsOk();
        audio = initialized ? engine.GetSubsystem<AudioEngineSubsystem>() : nullptr;
    }

    ~AudioEngine() {
        (void)engine.Shutdown();
    }

    // 프레임 하나: 엔진 프레임(결과 전달 → 묶음 처리) → 렌더 → 장치 콜백 소비 → 다시 렌더.
    bool Frame(uint32 pumpFrames = PumpFrames) {
        if (!engine.RunFrame()) {
            return false;
        }

        PumpDevice(audio, pumpFrames);
        return true;
    }

    // 장치 콜백을 frames만큼 부른다. Null Pump는 콜백 크기 이하만 받으므로 나눠 부르고, 그 사이에
    // 렌더해 링을 채운다.
    static void PumpDevice(AudioEngineSubsystem* audio, uint32 frames) {
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

    // 제어 쪽이 voice·clip을 모두 놓을 때까지 프레임을 돈다.
    bool RunUntilIdle() {
        for (uint32 frame = 0; frame < MaxTestFrames; ++frame) {
            const auto stats = audio->ControlStats();
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

SharedPtr<const SoundPcm> MakeConstantPcm(Allocator& allocator, float value, uint32 frames) {
    auto pcm = SoundPcm::TryAllocate(allocator, 1, frames, TestRate);
    if (!pcm) {
        return {};
    }

    for (float& sample : pcm.Value()->MutableChannel(0)) {
        sample = value;
    }

    return SharedPtr<const SoundPcm>(Move(pcm.Value()));
}

SoundSourceRef MakeSound(Allocator& allocator, const SharedPtr<const SoundPcm>& pcm, bool looping,
                         uint32 loopBegin = 0, uint32 loopEnd = 0) {
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.looping = looping;
    playable.loopBegin = loopBegin;
    playable.loopEnd = loopEnd;
    auto made = SoundSourceRef::TryCreate(allocator, playable);
    return made ? made.Value() : SoundSourceRef{};
}

// 결과 콜백이 쓰는 기록이다.
struct EventLog {
    explicit EventLog(Allocator& allocator) : events(allocator) {
    }

    PlaybackEventCallback Callback(Allocator& allocator) {
        auto made = PlaybackEventCallback::TryCreate(
            allocator,
            [this](const PlaybackEvent& event) noexcept { (void)events.TryPushBack(event); });
        return made ? Move(made.Value()) : PlaybackEventCallback{};
    }

    uint32 Count(PlaybackEventKind kind) const {
        uint32 count = 0;
        for (const PlaybackEvent& event : events) {
            count += event.kind == kind ? 1 : 0;
        }

        return count;
    }

    Array<PlaybackEvent> events;
};

float MaxAbs(Span<const float> samples) {
    float peak = 0.0f;
    for (const float sample : samples) {
        peak = platform::PlatformMath::Max(peak, platform::PlatformMath::Abs(sample));
    }

    return peak;
}

} // namespace

TEST(EngineAudio_RunTraitsFollowTargetAndFlags) {
    using build::TargetType;
    const StringView none[] = {StringView("--other")};
    const StringView noSound[] = {StringView("--no-sound")};
    const StringView mute[] = {StringView("--mute-audio")};
    const StringView allow[] = {StringView("--allow-commandlet-audio")};
    const StringView bench[] = {StringView("--benchmark")};
    const StringView withValue[] = {StringView("--no-sound=1")};
    const TargetType clients[] = {TargetType::Game, TargetType::Editor, TargetType::Client};
    for (const TargetType target : clients) {
        CHECK(ResolveAudioRunTraits(target, none, true).audioOutput);
        CHECK(!ResolveAudioRunTraits(target, noSound, true).audioOutput);
        CHECK(!ResolveAudioRunTraits(target, noSound, false).audioOutput);
        CHECK(!ResolveAudioRunTraits(target, bench, true).audioOutput);
        // 출시 구성은 개발 플래그(--benchmark)를 받지 않는다.
        CHECK(ResolveAudioRunTraits(target, bench, false).audioOutput);
        CHECK(ResolveAudioRunTraits(target, withValue, true).audioOutput);
        const AudioRunTraits muted = ResolveAudioRunTraits(target, mute, false);
        CHECK(muted.audioOutput);
        CHECK(muted.audioMuted);
    }

    CHECK(!ResolveAudioRunTraits(TargetType::Server, none, true).audioOutput);
    CHECK(!ResolveAudioRunTraits(TargetType::Server, allow, true).audioOutput);
    CHECK(!ResolveAudioRunTraits(TargetType::Program, none, true).audioOutput);
    CHECK(ResolveAudioRunTraits(TargetType::Program, allow, true).audioOutput);
    CHECK(!ResolveAudioRunTraits(TargetType::Program, allow, false).audioOutput);
    CHECK(!ResolveAudioRunTraits(TargetType::Game, none, true).audioMuted);
}

TEST(EngineAudio_SoundSourceRefValidatesSnapshot) {
    SystemAllocator allocator;
    CHECK(SoundPcm::TryAllocate(allocator, 0, 16, TestRate).Error() == ErrorCode::InvalidArgument);
    CHECK(SoundPcm::TryAllocate(allocator, 3, 16, TestRate).Error() == ErrorCode::InvalidArgument);
    CHECK(SoundPcm::TryAllocate(allocator, 1, 0, TestRate).Error() == ErrorCode::InvalidArgument);
    CHECK(SoundPcm::TryAllocate(allocator, 1, 16, 10).Error() == ErrorCode::InvalidArgument);

    SoundPlayable empty;
    CHECK(SoundSourceRef::TryCreate(allocator, empty).Error() == ErrorCode::InvalidArgument);

    auto nan = SoundPcm::TryAllocate(allocator, 1, 16, TestRate);
    REQUIRE(nan);
    nan.Value()->MutableChannel(0)[3] = platform::PlatformMath::QuietNaN<float>();
    SoundPlayable nanPlayable;
    nanPlayable.pcm = SharedPtr<const SoundPcm>(Move(nan.Value()));
    CHECK(SoundSourceRef::TryCreate(allocator, nanPlayable).Error() == ErrorCode::InvalidArgument);

    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(allocator, 0.5f, 100);
    REQUIRE(pcm);
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.loopBegin = 10;
    playable.loopEnd = 10;
    CHECK(SoundSourceRef::TryCreate(allocator, playable).Error() == ErrorCode::InvalidArgument);
    playable.loopEnd = 101;
    CHECK(SoundSourceRef::TryCreate(allocator, playable).Error() == ErrorCode::InvalidArgument);
    playable.loopEnd = 100;
    CHECK(SoundSourceRef::TryCreate(allocator, playable));
    playable.volume = audio::MaxVoiceGain + 1.0f;
    CHECK(SoundSourceRef::TryCreate(allocator, playable).Error() == ErrorCode::InvalidArgument);
    playable.volume = 1.0f;
    playable.pitch = 0.1f;
    CHECK(SoundSourceRef::TryCreate(allocator, playable).Error() == ErrorCode::InvalidArgument);
    playable.pitch = 1.0f;
    SoundAttenuation custom;
    custom.distance.model = audio::DistanceAttenuationModel::Custom;
    playable.attenuation = custom;
    CHECK(SoundSourceRef::TryCreate(allocator, playable).Error() == ErrorCode::InvalidArgument);
    SoundAttenuation negative;
    negative.innerRadius = -1.0f;
    playable.attenuation = negative;
    CHECK(SoundSourceRef::TryCreate(allocator, playable).Error() == ErrorCode::InvalidArgument);
    playable.attenuation = SoundAttenuation{};
    auto made = SoundSourceRef::TryCreate(allocator, playable);
    REQUIRE(made);
    CHECK(made.Value().IsValid());
    CHECK(made.Value().Get()->attenuation.HasValue());
    CHECK(made.Value().UseCount() == 1);
}

TEST(EngineAudio_MakePlayableCopiesAttenuationSnapshot) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterEngineAudioTypes(registration));
    {
        ObjectRegistry objects(allocator, types, {0x45415544494f3031ull, 1});
        auto clip = objects.NewObject<SoundClip>(objects.OwnerAccess(), TypeIdOf<SoundClip>());
        auto settings = objects.NewObject<AttenuationSettings>(objects.OwnerAccess(),
                                                               TypeIdOf<AttenuationSettings>());
        REQUIRE(clip);
        REQUIRE(settings);
        auto clipBorrow = objects.Resolve<SoundClip>(objects.OwnerAccess(), clip.Value().Handle());
        auto settingsBorrow =
            objects.Resolve<AttenuationSettings>(objects.OwnerAccess(), settings.Value().Handle());
        REQUIRE(clipBorrow);
        REQUIRE(settingsBorrow);
        SoundClip& sound = *clipBorrow.Value().Get().Value();
        AttenuationSettings& attenuation = *settingsBorrow.Value().Get().Value();
        CHECK(sound.MakePlayable(allocator, objects, objects.OwnerAccess()).Error() ==
              ErrorCode::NotReady);

        const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(allocator, 0.25f, 64);
        REQUIRE(sound.SetPcm(pcm));
        CHECK(sound.PcmRevision() == 1);
        sound.looping = true;
        sound.volume = 0.5f;
        attenuation.innerRadius = 50.0f;
        attenuation.model = SoundDistanceModel::NaturalSound;
        REQUIRE(objects.SetReference(objects.OwnerAccess(), clip.Value().Handle(),
                                     sound.attenuation, settings.Value().Handle()));
        auto first = sound.MakePlayable(allocator, objects, objects.OwnerAccess());
        REQUIRE(first);
        // 원본을 바꿔도 이미 만든 스냅숏은 그대로다.
        attenuation.innerRadius = 900.0f;
        sound.volume = 1.0f;
        const SoundPlayable& snapshot = *first.Value().Get();
        CHECK(snapshot.looping);
        CHECK(snapshot.volume == 0.5f);
        REQUIRE(snapshot.attenuation.HasValue());
        CHECK(snapshot.attenuation.Value().innerRadius == 50.0f);
        CHECK(snapshot.attenuation.Value().distance.model ==
              audio::DistanceAttenuationModel::NaturalSound);
        CHECK(snapshot.pcm.Get() == pcm.Get());
        CHECK(snapshot.revision == 1);
        attenuation.model = SoundDistanceModel::Custom;
        CHECK(sound.MakePlayable(allocator, objects, objects.OwnerAccess()).Error() ==
              ErrorCode::InvalidArgument);
    }

    CHECK(registration.Unregister());
}

TEST(EngineAudio_PlayStartsRendersAndEndsNaturally) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(fixture.audio->Device() != nullptr);
    CHECK(fixture.audio->Device()->Info().usingNull);
    Array<float> capture(fixture.allocator);
    REQUIRE(capture.TryResize(PumpFrames * 2 * 8));
    fixture.audio->Device()->NullOutput()->CaptureTo(Span<float>(capture.Data(), capture.Size()));

    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 1500);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, false);
    REQUIRE(sound.IsValid());
    EventLog log(fixture.allocator);
    PlayOptions options;
    options.onEvent = log.Callback(fixture.allocator);
    auto played = fixture.audio->Play2D(sound, Move(options));
    REQUIRE(played);
    CHECK(fixture.audio->Status(played.Value()) == PlaybackStatus::Pending);
    for (uint32 frame = 0; frame < MaxTestFrames && log.Count(PlaybackEventKind::Ended) == 0;
         ++frame) {
        REQUIRE(fixture.Frame());
    }

    REQUIRE(log.events.Size() == 2);
    CHECK(log.events[0].kind == PlaybackEventKind::Started);
    CHECK(log.events[1].kind == PlaybackEventKind::Ended);
    CHECK(log.events[1].reason == PlaybackEndReason::NaturalEnd);
    CHECK(log.events[1].mixFrame > log.events[0].mixFrame);
    CHECK(fixture.audio->Status(played.Value()) == PlaybackStatus::Finished);

    // mono Direct는 양쪽 같은 값이고, 1500 frame 동안만 소리가 난다.
    uint32 loud = 0;
    for (usize frame = 0; frame + 1 < capture.Size(); frame += 2) {
        CHECK(capture[frame] == capture[frame + 1]);
        loud += capture[frame] > 0.1f ? 1 : 0;
    }

    CHECK(loud >= 1400);
    CHECK(loud <= 1500);
    REQUIRE(fixture.RunUntilIdle());
    // clip 등록이 풀리고 PCM 참조는 시험과 스냅숏만 남는다.
    CHECK(pcm.UseCount() == 2);
}

TEST(EngineAudio_StopEndsOnceAndStaleHandlesDoNothing) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    const SoundSourceRef loop = MakeSound(fixture.allocator, pcm, true);
    EventLog log(fixture.allocator);
    PlayOptions options;
    options.onEvent = log.Callback(fixture.allocator);
    auto first = fixture.audio->Play2D(loop, Move(options));
    REQUIRE(first);
    // 프레임 1: 처리·제출 → 렌더(시작), 프레임 2: 시작 수거, 프레임 3 시작에 전달.
    for (uint32 frame = 0; frame < 3; ++frame) {
        REQUIRE(fixture.Frame());
    }

    CHECK(fixture.audio->Status(first.Value()) == PlaybackStatus::Playing);
    CHECK(fixture.audio->Stop(first.Value(), 64));
    CHECK(fixture.audio->Stop(first.Value(), 0));
    CHECK(fixture.audio->Stop(PlaybackHandle{}).Error() == ErrorCode::InvalidArgument);
    for (uint32 frame = 0; frame < 8; ++frame) {
        REQUIRE(fixture.Frame());
    }

    CHECK(log.Count(PlaybackEventKind::Started) == 1);
    REQUIRE(log.Count(PlaybackEventKind::Ended) == 1);
    CHECK(log.events[log.events.Size() - 1].reason == PlaybackEndReason::Stopped);

    // 슬롯을 다시 쓴 새 재생은 낡은 handle의 명령을 받지 않는다.
    auto second = fixture.audio->Play2D(loop);
    REQUIRE(second);
    CHECK(second.Value().slot == first.Value().slot);
    CHECK(second.Value().generation != first.Value().generation);
    CHECK(fixture.audio->Stop(first.Value()));
    CHECK(fixture.audio->SetParameter(first.Value(), PlaybackParameter::Volume, 0.0f));
    CHECK(fixture.audio->Status(first.Value()) == PlaybackStatus::Finished);
    for (uint32 frame = 0; frame < 4; ++frame) {
        REQUIRE(fixture.Frame());
    }

    CHECK(fixture.audio->Status(second.Value()) == PlaybackStatus::Playing);
    CHECK(fixture.audio->SetParameter(second.Value(), PlaybackParameter::Volume, 5.0f).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(fixture.audio->SetParameter(second.Value(), PlaybackParameter::Pitch, 0.1f).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(fixture.audio->Stop(second.Value()));
    REQUIRE(fixture.RunUntilIdle());
}

TEST(EngineAudio_PlayRejectsInvalidRequests) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 100);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 10, 50);
    CHECK(fixture.audio->Play2D(SoundSourceRef{}).Error() == ErrorCode::InvalidArgument);
    PlayOptions loud;
    loud.volume = audio::MaxVoiceGain * 2.0f;
    CHECK(fixture.audio->Play2D(sound, Move(loud)).Error() == ErrorCode::InvalidArgument);
    PlayOptions late;
    late.startFrame = 50;
    CHECK(fixture.audio->Play2D(sound, Move(late)).Error() == ErrorCode::InvalidArgument);
    PlayOptions inside;
    inside.startFrame = 49;
    CHECK(fixture.audio->Play2D(sound, Move(inside)));
    CHECK(fixture.audio->Stats().livePlaybacks == 1);
}

TEST(EngineAudio_NoOutputReturnsIgnoredHandles) {
    AudioEngine fixture(DeterministicOptions(false));
    REQUIRE(fixture.initialized);
    CHECK(fixture.audio->Device() == nullptr);
    CHECK(!fixture.audio->RunTraits().audioOutput);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 100);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, false);
    EventLog log(fixture.allocator);
    PlayOptions options;
    options.onEvent = log.Callback(fixture.allocator);
    auto played = fixture.audio->Play2D(sound, Move(options));
    REQUIRE(played);
    CHECK(fixture.audio->Status(played.Value()) == PlaybackStatus::Ignored);
    CHECK(fixture.audio->Stop(played.Value()));
    REQUIRE(fixture.Frame());
    REQUIRE(log.events.Size() == 1);
    CHECK(log.events[0].kind == PlaybackEventKind::Ended);
    CHECK(log.events[0].reason == PlaybackEndReason::Ignored);
    CHECK(fixture.audio->Status(played.Value()) == PlaybackStatus::Finished);

    // 재생 표 용량: MaxPlaybacks까지 받고 그 뒤 CapacityExceeded, 한 프레임 뒤 모두 풀린다.
    for (uint32 index = 0; index < MaxPlaybacks; ++index) {
        REQUIRE(fixture.audio->Play2D(sound));
    }

    CHECK(fixture.audio->Play2D(sound).Error() == ErrorCode::CapacityExceeded);
    REQUIRE(fixture.Frame());
    CHECK(fixture.audio->Stats().livePlaybacks == 0);
    CHECK(fixture.audio->Play2D(sound));
}

TEST(EngineAudio_VoiceLimitEndsExtraPlays) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, 480);
    const SoundSourceRef loop = MakeSound(fixture.allocator, pcm, true);
    Array<PlaybackHandle> handles(fixture.allocator);
    // 설정 문맥이 없으면 audio.voices.max는 기본 32다.
    for (uint32 index = 0; index < 33; ++index) {
        auto played = fixture.audio->Play2D(loop);
        REQUIRE(played);
        REQUIRE(handles.TryPushBack(played.Value()));
    }

    REQUIRE(fixture.Frame());
    REQUIRE(fixture.Frame());
    CHECK(fixture.audio->ControlStats().activeVoices == 32);
    CHECK(fixture.audio->ControlStats().voiceLimitRejects == 1);
    CHECK(fixture.audio->Status(handles[32]) == PlaybackStatus::Finished);
    // 같은 PCM·구간은 clip 등록 하나를 함께 쓴다.
    CHECK(fixture.audio->ControlStats().registeredClips == 1);
    for (const PlaybackHandle handle : handles) {
        REQUIRE(fixture.audio->Stop(handle));
    }

    // terminal이 나온 직후(아직 놓기 대기 — ACK 전)의 새 재생은 놓기 대기 voice에 막히지 않는다
    // (믹서 표 여유 MixerVoiceHeadroomFactor).
    for (uint32 frame = 0; frame < MaxTestFrames && fixture.audio->ControlStats().activeVoices != 0;
         ++frame) {
        REQUIRE(fixture.Frame());
    }

    REQUIRE(fixture.audio->ControlStats().activeVoices == 0);
    for (uint32 index = 0; index < 32; ++index) {
        REQUIRE(fixture.audio->Play2D(loop));
    }

    REQUIRE(fixture.Frame());
    CHECK(fixture.audio->ControlStats().voiceLimitRejects == 1);
}

TEST(EngineAudio_LoopRangesRegisterSeparateClips) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 1000);
    const SoundSourceRef whole = MakeSound(fixture.allocator, pcm, true);
    const SoundSourceRef middle = MakeSound(fixture.allocator, pcm, true, 200, 400);
    auto a = fixture.audio->Play2D(whole);
    auto b = fixture.audio->Play2D(middle);
    auto c = fixture.audio->Play2D(whole);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    REQUIRE(fixture.Frame());
    CHECK(fixture.audio->ControlStats().registeredClips == 2);
    REQUIRE(fixture.audio->Stop(a.Value()));
    REQUIRE(fixture.audio->Stop(b.Value()));
    REQUIRE(fixture.audio->Stop(c.Value()));
    REQUIRE(fixture.RunUntilIdle());
    CHECK(pcm.UseCount() == 3);
}

TEST(EngineAudio_MasterGainFollowsMuteAndFocus) {
    {
        AudioEngine muted(DeterministicOptions(true, true));
        REQUIRE(muted.initialized);
        Array<float> capture(muted.allocator);
        REQUIRE(capture.TryResize(PumpFrames * 2 * 4));
        muted.audio->Device()->NullOutput()->CaptureTo(Span<float>(capture.Data(), capture.Size()));
        const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(muted.allocator, 0.5f, 480);
        REQUIRE(muted.audio->Play2D(MakeSound(muted.allocator, pcm, true)));
        for (uint32 frame = 0; frame < 4; ++frame) {
            REQUIRE(muted.Frame());
        }

        // 첫 마스터 gain은 ramp 0이라 처음부터 새지 않는다.
        CHECK(MaxAbs(Span<const float>(capture.Data(), capture.Size())) == 0.0f);
        CHECK(muted.audio->ControlStats().masterGainCommands == 1);
    }

    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    fixture.audio->SetApplicationForeground(false);
    Array<float> capture(fixture.allocator);
    REQUIRE(capture.TryResize(PumpFrames * 2));
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true)));
    fixture.audio->Device()->NullOutput()->CaptureTo(Span<float>(capture.Data(), capture.Size()));
    for (uint32 frame = 0; frame < 3; ++frame) {
        REQUIRE(fixture.Frame());
    }

    // 비활성 창 배수 기본 0(UE UnfocusedVolumeMultiplier).
    CHECK(MaxAbs(Span<const float>(capture.Data(), capture.Size())) == 0.0f);
    fixture.audio->SetApplicationForeground(true);
    for (uint32 frame = 0; frame < 3; ++frame) {
        REQUIRE(fixture.Frame());
    }

    fixture.audio->Device()->NullOutput()->CaptureTo(Span<float>(capture.Data(), capture.Size()));
    REQUIRE(fixture.Frame());
    CHECK(MaxAbs(Span<const float>(capture.Data(), capture.Size())) > 0.2f);
    // 값이 바뀐 묶음에서만 보낸다(첫 0 → 포커스 복귀 1).
    CHECK(fixture.audio->ControlStats().masterGainCommands == 2);
}

TEST(EngineAudio_FadeInRampsAcrossBatches) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    PlayOptions options;
    options.fadeInFrames = PumpFrames * 6;
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true), Move(options)));
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    float previous = 0.0f;
    float first = -1.0f;
    for (uint32 frame = 0; frame < 12; ++frame) {
        fixture.audio->Device()->NullOutput()->CaptureTo(Span<float>(chunk.Data(), chunk.Size()));
        REQUIRE(fixture.Frame());
        const float peak = MaxAbs(Span<const float>(chunk.Data(), chunk.Size()));
        if (peak > 0.0f && first < 0.0f) {
            first = peak;
        }

        CHECK(peak + 1.0e-6f >= previous);
        previous = peak;
    }

    // 처음은 목표보다 작고 끝에는 목표(mono Direct 0.5 × 0.5)에 닿는다.
    CHECK(first > 0.0f);
    CHECK(first < 0.2f);
    CHECK(platform::PlatformMath::Abs(previous - 0.25f) < 1.0e-3f);
}

TEST(EngineAudio_CallbackMayPlayAgain) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 200);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, false);
    AudioEngineSubsystem* audio = fixture.audio;
    uint32 replays = 0;
    PlaybackHandle replayed;
    auto callback = PlaybackEventCallback::TryCreate(
        fixture.allocator, [&](const PlaybackEvent& event) noexcept {
            if (event.kind == PlaybackEventKind::Ended) {
                auto again = audio->Play2D(sound);
                replayed = again ? again.Value() : PlaybackHandle{};
                ++replays;
            }
        });
    REQUIRE(callback);
    PlayOptions options;
    options.onEvent = Move(callback.Value());
    auto first = fixture.audio->Play2D(sound, Move(options));
    REQUIRE(first);
    for (uint32 frame = 0; frame < MaxTestFrames && replays == 0; ++frame) {
        REQUIRE(fixture.Frame());
    }

    REQUIRE(replays == 1);
    REQUIRE(replayed.IsValid());
    // 콜백 안에서 받은 슬롯은 끝난 재생의 슬롯과 다르다(슬롯은 콜백이 돌아온 뒤 놓인다).
    CHECK(!(replayed == first.Value()));
    CHECK(fixture.audio->Status(replayed) != PlaybackStatus::Finished);
}

TEST(EngineAudio_ShutdownClosesPlaysAndReleasesPcm) {
    SystemAllocator allocator;
    const SharedPtr<const SoundPcm> loopPcm = MakeConstantPcm(allocator, 0.5f, 480);
    const SoundSourceRef loop = MakeSound(allocator, loopPcm, true);
    EventLog log(allocator);
    {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        PlayOptions options;
        options.onEvent = log.Callback(allocator);
        REQUIRE(fixture.audio->Play2D(loop, Move(options)));
        for (uint32 frame = 0; frame < 3; ++frame) {
            REQUIRE(fixture.Frame());
        }

        CHECK(loopPcm.UseCount() > 2);
        fixture.audio->BeginShutdown(fixture.engine);
        CHECK(fixture.audio->Play2D(loop).Error() == ErrorCode::ShuttingDown);
        REQUIRE(fixture.engine.Shutdown());
    }

    // 종료 뒤 콜백은 불리지 않고(Started만), 참조는 시험 쪽만 남는다.
    CHECK(log.Count(PlaybackEventKind::Started) == 1);
    CHECK(log.Count(PlaybackEventKind::Ended) == 0);
    CHECK(loopPcm.UseCount() == 2);
    CHECK(loop.UseCount() == 1);
}

TEST(EngineAudio_InitializeFailureOfLaterSubsystemCleansUp) {
    SystemAllocator allocator;
    HostOptionsScope scope(DeterministicOptions(true));
    REQUIRE(scope.Registered());
    const EngineSubsystemType subsystems[] = {
        AudioEngineSubsystemType(), MakeSubsystemType<EngineSubsystem, FailingSubsystem>("Fail")};
    EngineConfig config;
    config.modules = &test::ProcessModules();
    config.subsystems = subsystems;
    config.fixedFrameDeltaSeconds = 1.0 / 60.0;
    Engine engine(allocator);
    // 실패 정리가 BeginShutdown·CompleteInFlightWork 없이 Deinitialize만 불러도 lane·장치가 닫힌다.
    CHECK(engine.Initialize(config).Error() == ErrorCode::Unsupported);
}

TEST(EngineAudio_ThreadControlProducesSameEvents) {
    AudioEngineHostOptions options = DeterministicOptions(true);
    options.controlMode = AudioControlMode::Thread;
    AudioEngine fixture(options);
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 700);
    EventLog log(fixture.allocator);
    PlayOptions play;
    play.onEvent = log.Callback(fixture.allocator);
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, false), Move(play)));
    for (uint32 frame = 0; frame < MaxTestFrames && log.Count(PlaybackEventKind::Ended) == 0;
         ++frame) {
        REQUIRE(fixture.engine.RunFrame());
        // 보낸 묶음 처리를 Flush로 기다린 뒤 렌더한다(시간 대기 없음).
        fixture.audio->FlushControl();
        AudioEngine::PumpDevice(fixture.audio, PumpFrames);
    }

    REQUIRE(log.events.Size() == 2);
    CHECK(log.events[0].kind == PlaybackEventKind::Started);
    CHECK(log.events[1].reason == PlaybackEndReason::NaturalEnd);
}

} // namespace audio_engine_tests_file
