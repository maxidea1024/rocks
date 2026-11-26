#include "AudioEngineTestSupport.h"

#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <Engine/Engine.h>
#include <AudioAssets/AttenuationSettings.generated.h>
#include <EngineAudio/AudioEngineSettings.h>
#include <EngineAudio/AudioEngineSubsystem.h>
#include <AudioAssets/AudioAssetsTypes.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AudioAssets/SoundSource.generated.h>
#include <ModuleLoader/HostServices.h>
#include <Object/ObjectRegistry.h>
#include <Platform/PlatformMath.h>
#include <Platform/Sync.h>
#include <TestSupport/Test.h>

// AU3a1 시험(Docs/Design/AudioSystem 3.35.12절): Inline 제어 + Manual render + Null Manual 장치로
// 프레임 진행·Pump만으로 순서를 만든다(시간 대기 없음).
using namespace rocks;
namespace audio_engine_tests_file {

namespace {

using namespace rocks::engine_audio_test;

class FailingSubsystem final : public EngineSubsystem {
public:
    Result<void> Initialize(Engine&) noexcept override {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
};

// 제어 쪽을 멈추는 gate다(beforeControlBatch — 이벤트로만 순서를 만든다).
struct ControlGate {
    platform::ManualResetEvent open{false};

    static void Wait(void* context) noexcept {
        (void)static_cast<ControlGate*>(context)->open.Wait();
    }
};

// 같은 입력을 두 제어 방식으로 돌려 결과 열(kind, reason, mixFrame)을 얻는다.
void RunReferencePlay(AudioControlMode mode, Array<PlaybackEvent>& out) {
    AudioEngineHostOptions options = DeterministicOptions(true);
    options.controlMode = mode;
    AudioEngine fixture(options);
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 700);
    const SharedPtr<const SoundPcm> loopPcm = MakeConstantPcm(fixture.allocator, 0.2f, 300);
    EventLog log(fixture.allocator);
    PlayOptions first;
    first.onEvent = log.Callback(fixture.allocator);
    PlayOptions second;
    second.onEvent = log.Callback(fixture.allocator);
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, false), Move(first)));
    auto loop = fixture.audio->Play2D(MakeSound(fixture.allocator, loopPcm, true), Move(second));
    REQUIRE(loop);
    for (uint32 frame = 0; frame < 12; ++frame) {
        if (frame == 6) {
            REQUIRE(fixture.audio->Stop(loop.Value(), 128));
        }

        REQUIRE(fixture.engine.RunFrame());
        fixture.audio->FlushControl();
        PumpDevice(fixture.audio, PumpFrames);
    }

    for (const PlaybackEvent& event : log.events) {
        REQUIRE(out.TryPushBack(event));
    }
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
    REQUIRE(RegisterAudioAssetsTypes(registration));
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

    // mono Direct는 양쪽 0.5 × 0.5 = 0.25이고(UE Linear 업믹스), 1500 frame 동안만 소리가 난다.
    // 시작 블록 이후 값은 정확히 0.25다(gain 1, 마스터 1, limiter 문턱 아래).
    uint32 loud = 0;
    uint32 exact = 0;
    for (usize frame = 0; frame + 1 < capture.Size(); frame += 2) {
        CHECK(capture[frame] == capture[frame + 1]);
        loud += capture[frame] > 0.1f ? 1 : 0;
        exact += capture[frame] == 0.25f ? 1 : 0;
    }

    CHECK(loud == 1500);
    CHECK(exact == 1500);
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

    // 놓기 대기(ACK 전) voice가 새 재생을 막지 않도록 믹서 표는 상한의 두 배다. Manual render에서는
    // terminal과 ACK가 같은 블록 끝에 게시되어 놓기가 바로 되므로(놓기 대기는 Thread render의
    // 경합에서만 생긴다) 표 크기로 여유를 확인한다.
    CHECK(fixture.audio->Device()->Mixer().Config().maxVoices == 64);
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

} // namespace audio_engine_tests_file

namespace audio_engine_tests_file {

TEST(EngineAudio_OutputTailUsesPlayedFrames) {
    audio::AudioOutputRingCounters counters;
    counters.consumedFrames = 1000;
    counters.underrunFrames = 0;
    CHECK(IsOutputTailPlayed(counters, 1000));
    CHECK(!IsOutputTailPlayed(counters, 1001));
    // underrun 무음은 렌더한 소리를 낸 것이 아니다.
    counters.underrunFrames = 200;
    CHECK(!IsOutputTailPlayed(counters, 1000));
    CHECK(IsOutputTailPlayed(counters, 800));
}

TEST(EngineAudio_DeferredPlaysKeepOrderAndHonourStop) {
    AudioEngineHostOptions options = DeterministicOptions(true);
    // 명령 링 4칸: 묶음 처리 순서가 명령 → 마스터 gain이라 Play 넷이 링을 채우고, 나머지 Play 둘은
    // Deferred, 첫 마스터 gain은 다음 묶음으로 밀린다.
    options.commandCapacity = 4;
    AudioEngine fixture(options);
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, 480);
    const SoundSourceRef loop = MakeSound(fixture.allocator, pcm, true);
    EventLog log(fixture.allocator);
    Array<PlaybackHandle> handles(fixture.allocator);
    for (uint32 index = 0; index < 6; ++index) {
        PlayOptions play;
        play.onEvent = log.Callback(fixture.allocator);
        auto played = fixture.audio->Play2D(loop, Move(play));
        REQUIRE(played);
        REQUIRE(handles.TryPushBack(played.Value()));
    }

    // 렌더하지 않고 프레임만 돌려 링을 채운 채로 둔다.
    REQUIRE(fixture.engine.RunFrame());
    CHECK(fixture.audio->ControlStats().pendingDeferred == 2);
    CHECK(fixture.audio->ControlStats().deferredPlays == 2);
    CHECK(fixture.audio->ControlStats().masterGainRetries == 1);
    // 보내기 전 Stop은 fade만 적어 두었다가 제출 직후 낸다.
    REQUIRE(fixture.audio->Stop(handles[5], 0));
    // 링이 찬 동안 마스터 gain 변화는 다음 묶음으로 밀린다.
    fixture.audio->SetApplicationForeground(false);
    REQUIRE(fixture.engine.RunFrame());
    CHECK(fixture.audio->ControlStats().masterGainRetries == 2);
    REQUIRE(fixture.Frames(8));
    CHECK(fixture.audio->ControlStats().pendingDeferred == 0);
    // 밀린 첫 마스터 gain은 링이 빈 뒤 그때의 목표(비활성 창 0)로 한 번 나간다.
    CHECK(fixture.audio->ControlStats().masterGainCommands == 1);
    // 미룬 Play는 받은 순서대로 시작한다(시작 frame이 줄지 않는다).
    uint64 previousStart = 0;
    for (uint32 index = 0; index < 5; ++index) {
        bool found = false;
        for (const PlaybackEvent& event : log.events) {
            if (event.handle == handles[index] && event.kind == PlaybackEventKind::Started) {
                CHECK(event.mixFrame >= previousStart);
                previousStart = event.mixFrame;
                found = true;
            }
        }

        CHECK(found);
    }

    uint32 stoppedEnds = 0;
    for (const PlaybackEvent& event : log.events) {
        if (event.handle == handles[5] && event.kind == PlaybackEventKind::Ended) {
            CHECK(event.reason == PlaybackEndReason::Stopped);
            ++stoppedEnds;
        }
    }

    CHECK(stoppedEnds == 1);
}

TEST(EngineAudio_ShutdownSubmitsDeferredPlays) {
    SystemAllocator allocator;
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(allocator, 0.01f, 480);
    const SoundSourceRef loop = MakeSound(allocator, pcm, true);
    {
        AudioEngineHostOptions options = DeterministicOptions(true);
        options.commandCapacity = 2;
        AudioEngine fixture(options);
        REQUIRE(fixture.initialized);
        for (uint32 index = 0; index < 4; ++index) {
            REQUIRE(fixture.audio->Play2D(loop));
        }

        REQUIRE(fixture.engine.RunFrame());
        CHECK(fixture.audio->ControlStats().pendingDeferred != 0);
        REQUIRE(fixture.engine.Shutdown());
    }

    // RT에 닿지 못한 Play의 clip 등록은 믹서와 함께 사라지고 참조는 시험 쪽만 남는다.
    CHECK(pcm.UseCount() == 2);
    CHECK(loop.UseCount() == 1);
}

TEST(EngineAudio_SetParameterRampsAndMerges) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    auto played = fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true));
    REQUIRE(played);
    REQUIRE(fixture.Frames(4));
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    // 긴 ramp는 묶음마다 중간값으로 게시된다(2D도 같은 경로 — 3.35.6절).
    REQUIRE(fixture.audio->SetParameter(played.Value(), PlaybackParameter::Volume, 0.0f,
                                        PumpFrames * 8));
    float previous = 1.0f;
    uint32 between = 0;
    for (uint32 frame = 0; frame < 12; ++frame) {
        const StereoPeak peak = CapturePeak(fixture, chunk);
        CHECK(peak.left <= previous + 1.0e-6f);
        between += peak.left > 0.01f && peak.left < 0.24f ? 1 : 0;
        previous = peak.left;
    }

    CHECK(between >= 3);
    CHECK(previous == 0.0f);
    // 같은 프레임의 두 번 호출은 마지막 값 하나로 합친다.
    REQUIRE(fixture.audio->SetParameter(played.Value(), PlaybackParameter::Volume, 2.0f, 0));
    REQUIRE(fixture.audio->SetParameter(played.Value(), PlaybackParameter::Volume, 1.0f, 0));
    CHECK(fixture.audio->Stats().mergedParameters == 1);
    REQUIRE(fixture.Frames(3));
    const StereoPeak settled = CapturePeak(fixture, chunk);
    CHECK(platform::PlatformMath::Abs(settled.left - 0.25f) < 1.0e-4f);
}

TEST(EngineAudio_CallbackMayStopItsPlayback) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 480);
    AudioEngineSubsystem* audio = fixture.audio;
    EventLog log(fixture.allocator);
    auto callback = PlaybackEventCallback::TryCreate(
        fixture.allocator, [&](const PlaybackEvent& event) noexcept {
            (void)log.events.TryPushBack(event);
            if (event.kind == PlaybackEventKind::Started) {
                (void)audio->Stop(event.handle, 0);
            }
        });
    REQUIRE(callback);
    PlayOptions options;
    options.onEvent = Move(callback.Value());
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true), Move(options)));
    REQUIRE(fixture.Frames(8));
    REQUIRE(log.events.Size() == 2);
    CHECK(log.events[1].kind == PlaybackEventKind::Ended);
    CHECK(log.events[1].reason == PlaybackEndReason::Stopped);
}

TEST(EngineAudio_LoopRangePlaysOnlyItsRange) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    // 값이 frame/1000인 PCM: 전체 loop는 0.999까지, [200, 400) loop는 0.4 미만만 낸다.
    const SharedPtr<const SoundPcm> pcm = MakeRampPcm(fixture.allocator, 0.001f, 1000);
    PlayOptions silent;
    silent.volume = 0.0f;
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true), Move(silent)));
    PlayOptions middle;
    middle.startFrame = 200;
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true, 200, 400), Move(middle)));
    REQUIRE(fixture.Frames(2));
    CHECK(fixture.audio->ControlStats().registeredClips == 2);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2 * 4));
    fixture.audio->Device()->NullOutput()->CaptureTo(Span<float>(chunk.Data(), chunk.Size()));
    REQUIRE(fixture.Frames(4));
    const StereoPeak peak = PeakOf(Span<const float>(chunk.Data(), chunk.Size()));
    // mono Direct 0.5배: 구간 끝 0.399 × 0.5 ≈ 0.2.
    CHECK(peak.left > 0.15f);
    CHECK(peak.left < 0.2001f);
}

TEST(EngineAudio_SettingsDriveMasterVolumeAndBootValues) {
    test::config::ConfigFixture settings(test::config::DevelopmentRules());
    config::ConfigContext& context = settings.Context();
    REQUIRE(RegisterEngineAudioSettings(context));
    REQUIRE(
        context.AddAssignment(config::ConfigLayer::Session, "test", "audio.master.volume", "0.5"));
    // 허용값이 아닌 블록 크기는 가장 가까운 값으로 보정한다(Warning).
    REQUIRE(context.AddAssignment(config::ConfigLayer::Session, "test", "audio.mixer.blockframes",
                                  "300"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    AudioEngine fixture(DeterministicOptions(true), &context);
    REQUIRE(fixture.initialized);
    CHECK(fixture.audio->Device()->Mixer().Config().blockFrames == 256);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true)));
    REQUIRE(fixture.Frames(4));
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    const StereoPeak peak = CapturePeak(fixture, chunk);
    CHECK(platform::PlatformMath::Abs(peak.left - 0.125f) < 1.0e-4f);
}

TEST(EngineAudio_StalledControlDefersFramesWithoutLosingCommands) {
    ControlGate gate;
    AudioEngineHostOptions options = DeterministicOptions(true);
    options.controlMode = AudioControlMode::Thread;
    options.beforeControlBatch = &ControlGate::Wait;
    options.beforeControlBatchContext = &gate;
    AudioEngine fixture(options);
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 300);
    EventLog log(fixture.allocator);
    // 제어 쪽이 gate에 멈춘 동안 묶음이 바닥나면 그 프레임은 보내지 않고 명령을 다음 묶음에 남긴다.
    for (uint32 frame = 0; frame < 4; ++frame) {
        REQUIRE(fixture.engine.RunFrame());
    }

    CHECK(fixture.audio->Stats().deferredFrames >= 1);
    PlayOptions play;
    play.onEvent = log.Callback(fixture.allocator);
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, false), Move(play)));
    REQUIRE(fixture.engine.RunFrame());
    gate.open.Signal();
    for (uint32 frame = 0; frame < 16 && log.Count(PlaybackEventKind::Ended) == 0; ++frame) {
        fixture.audio->FlushControl();
        PumpDevice(fixture.audio, PumpFrames);
        REQUIRE(fixture.engine.RunFrame());
    }

    CHECK(log.Count(PlaybackEventKind::Started) == 1);
    CHECK(log.Count(PlaybackEventKind::Ended) == 1);
}

TEST(EngineAudio_InitializeFailureCleansUpThreadControl) {
    SystemAllocator allocator;
    AudioEngineHostOptions options = DeterministicOptions(true);
    options.controlMode = AudioControlMode::Thread;
    HostOptionsScope scope(options);
    REQUIRE(scope.Registered());
    const EngineSubsystemType subsystems[] = {
        AudioEngineSubsystemType(), MakeSubsystemType<EngineSubsystem, FailingSubsystem>("Fail")};
    EngineConfig config;
    config.modules = &test::ProcessModules();
    config.subsystems = subsystems;
    config.fixedFrameDeltaSeconds = 1.0 / 60.0;
    Engine engine(allocator);
    // 실행 중인 lane 스레드가 Deinitialize의 drain에서 닫힌다(실행 중 파괴 Fatal 없음).
    CHECK(engine.Initialize(config).Error() == ErrorCode::Unsupported);
}

// 엔진 디코더 표(Docs/Design/AudioSystem 3.37.1절): 내장 PCM16·ADPCM에 등록부의 factory를 더한다.
// 시험 실행은 AudioOpus를 링크하므로 등록부가 있으면 엔진 묶음 시작 때 Opus가 들어오고, 마지막
// 엔진이 묶음을 내리면 모듈 Shutdown이 지운다. 장치가 없는 실행도 표를 만든다.
TEST(EngineAudio_DecoderTableIncludesRegisteredCodecs) {
    {
        audio::AudioDecoderRegistry registry;
        REQUIRE(test::ProcessModules().Services().Register(registry).IsOk());
        {
            AudioEngine run(DeterministicOptions(false));
            REQUIRE(run.initialized);
            const audio::AudioDecoderTable& table = run.audio->DecoderTable();
            CHECK(table.Contains(audio::AudioCodecId::Pcm16));
            CHECK(table.Contains(audio::AudioCodecId::Adpcm));
            CHECK(table.Contains(audio::AudioCodecId::Opus));
            CHECK(registry.Factory(audio::AudioCodecId::Opus) != nullptr);
            auto decoder = table.Create(audio::AudioCodecId::Opus, run.allocator);
            CHECK(decoder.IsOk());
        }

        CHECK(registry.Factory(audio::AudioCodecId::Opus) == nullptr);
        REQUIRE(test::ProcessModules().Services().Unregister(registry));
    }

    // 등록부가 없으면 내장 디코더뿐이다.
    AudioEngine run(DeterministicOptions(true));
    REQUIRE(run.initialized);
    const audio::AudioDecoderTable& table = run.audio->DecoderTable();
    CHECK(table.Contains(audio::AudioCodecId::Pcm16));
    CHECK(table.Contains(audio::AudioCodecId::Adpcm));
    CHECK(!table.Contains(audio::AudioCodecId::Opus));
}

TEST(EngineAudio_ThreadAndInlineControlProduceSameEvents) {
    SystemAllocator allocator;
    Array<PlaybackEvent> inlineEvents(allocator);
    Array<PlaybackEvent> threadEvents(allocator);
    RunReferencePlay(AudioControlMode::Inline, inlineEvents);
    RunReferencePlay(AudioControlMode::Thread, threadEvents);
    REQUIRE(inlineEvents.Size() == 4);
    REQUIRE(threadEvents.Size() == inlineEvents.Size());
    for (usize index = 0; index < inlineEvents.Size(); ++index) {
        CHECK(threadEvents[index].kind == inlineEvents[index].kind);
        CHECK(threadEvents[index].reason == inlineEvents[index].reason);
        CHECK(threadEvents[index].mixFrame == inlineEvents[index].mixFrame);
        CHECK(threadEvents[index].handle == inlineEvents[index].handle);
    }
}

} // namespace audio_engine_tests_file
