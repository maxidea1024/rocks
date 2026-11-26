#include "AudioEngineTestSupport.h"

#include <Core/Module/ModuleEntry.h>
#include <AudioAssets/AudioAssetsTypes.h>
#include <AudioAssets/SoundClip.generated.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <Object/ObjectRegistry.h>

#include <Core/Math/MathUtility.h>
#include <EngineAudio/AudioComponent.h>
#include <EngineAudio/AudioSpatialization.h>
#include <EngineAudio/AudioWorldSubsystem.h>
#include <EngineAudio/AudioComponent.generated.h>
#include <World/Actor.generated.h>

// AU3a2 시험(Docs/Design/AudioSystem 3.35.12절): World scene·리스너·emitter·공간화·AudioComponent.
using namespace rocks;
namespace audio_world_tests_file {

namespace {

using namespace rocks::engine_audio_test;

// 1/√2(정면 equal-power 이득)다.
constexpr float InverseSqrtTwo = 0.70710678f;
// 감쇠 없이 패닝만 보려는 넓은 내부 반경(cm)이다.
constexpr float WideInnerRadius = 100000.0f;

SoundAttenuation PanOnly() {
    SoundAttenuation attenuation;
    attenuation.innerRadius = WideInnerRadius;
    return attenuation;
}

Quatd Yaw(double degrees) {
    auto made = Quatd::TryFromAxisAngle(Vec3d(0.0, 0.0, 1.0), degrees * math::Pi<double> / 180.0);
    return made ? made.Value() : Quatd::Identity();
}

struct WorldScope {
    WorldScope(AudioEngine& fixture, WorldKind kind = WorldKind::Game, bool beginPlay = true)
        : engine(fixture) {
        subsystems[0] = AudioWorldSubsystem::Type();
        WorldCreateDesc desc;
        desc.name = "AudioTest";
        desc.subsystems = Span<const WorldSubsystemType>(subsystems, 1);
        desc.kind = kind;
        desc.beginPlay = beginPlay;
        auto created = fixture.engine.CreateWorld(desc);
        world = created ? created.Value() : nullptr;
        audio = world != nullptr ? world->GetSubsystem<AudioWorldSubsystem>() : nullptr;
    }

    // World를 지금 파괴한다(프레임 밖 — 즉시).
    bool Destroy() {
        const bool destroyed = engine.engine.DestroyWorld(world).IsOk();
        world = nullptr;
        audio = nullptr;
        return destroyed;
    }

    AudioEngine& engine;
    WorldSubsystemType subsystems[1];
    World* world = nullptr;
    AudioWorldSubsystem* audio = nullptr;
};

struct SpawnedAudio {
    ObjectHandle actor;
    ObjectHandle component;
};

SpawnedAudio SpawnAudio(World& world, const SoundSourceRef& sound, const Vec3d& location,
                        bool autoActivate) {
    SpawnedAudio spawned;
    auto actor = world.SpawnActorDeferred<Actor>();
    if (!actor) {
        return spawned;
    }

    auto component = world.AddComponent<AudioComponent>(actor.Value());
    if (!component) {
        return spawned;
    }

    (void)world.With<Actor>(actor.Value(), [&](Actor& owner) noexcept {
        return owner.SetRootComponent(component.Value());
    });
    (void)world.With<AudioComponent>(component.Value(), [&](AudioComponent& audio) noexcept {
        audio.SetSound(sound);
        audio.autoActivate = autoActivate;
        audio.SetMobility(ComponentMobility::Movable);
        return audio.SetRelativeLocation(location);
    });
    if (!world.FinishSpawning(actor.Value())) {
        return spawned;
    }

    spawned.actor = actor.Value();
    spawned.component = component.Value();
    return spawned;
}

template <class Function>
bool WithAudio(World& world, ObjectHandle component, Function&& function) {
    return world
        .With<AudioComponent>(component,
                              [&](AudioComponent& audio) noexcept {
                                  function(audio);
                                  return Result<void>::Success();
                              })
        .IsOk();
}

} // namespace

TEST(EngineAudioWorld_NonSpatializedAmountMatchesUe) {
    CHECK(NonSpatializedAmount(0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(NonSpatializedAmount(0.0f, 500.0f, 100.0f) == 1.0f);
    CHECK(NonSpatializedAmount(10.0f, 0.0f, 0.0f) == 0.0f);
    // E > 0: 선형(S − d) / (S − E).
    CHECK(NonSpatializedAmount(750.0f, 1000.0f, 500.0f) == 0.5f);
    CHECK(NonSpatializedAmount(400.0f, 1000.0f, 500.0f) == 1.0f);
    CHECK(NonSpatializedAmount(1200.0f, 1000.0f, 500.0f) == 0.0f);
    // E > S는 E = S로 본다(구간 하한 1).
    CHECK(NonSpatializedAmount(999.5f, 1000.0f, 2000.0f) == 0.5f);
    // E = 0: 1 − 1/r², r = S/d.
    CHECK(NonSpatializedAmount(500.0f, 1000.0f, 0.0f) == 0.75f);
    CHECK(NonSpatializedAmount(2000.0f, 1000.0f, 0.0f) == 0.0f);
}

TEST(EngineAudioWorld_SpatialParamsFollowListenerFrame) {
    SoundAttenuation linear;
    const Vec3d origin;
    // 기본 Linear: inner 400 cm 안 1, 감쇠 거리 3600 cm의 중간 0.5, 끝 너머 0.
    CHECK(ComputeSpatialParams(linear, 1, origin, Quatd::Identity(), Vec3d(300.0, 0.0, 0.0))
              .attenuationGain == 1.0f);
    CHECK(math::Abs(
              ComputeSpatialParams(linear, 1, origin, Quatd::Identity(), Vec3d(2200.0, 0.0, 0.0))
                  .attenuationGain -
              0.5f) < 1.0e-5f);
    CHECK(ComputeSpatialParams(linear, 1, origin, Quatd::Identity(), Vec3d(5000.0, 0.0, 0.0))
              .attenuationGain == 0.0f);
    // 오른쪽(+Y)은 90°, 리스너를 180° 돌리면 270°다.
    const SpatialParams right =
        ComputeSpatialParams(linear, 1, origin, Quatd::Identity(), Vec3d(0.0, 100.0, 0.0));
    CHECK(right.azimuthPan);
    CHECK(math::Abs(right.azimuthDegrees - 90.0f) < 1.0e-3f);
    const SpatialParams turned =
        ComputeSpatialParams(linear, 1, origin, Yaw(180.0), Vec3d(0.0, 100.0, 0.0));
    CHECK(math::Abs(turned.azimuthDegrees - 270.0f) < 1.0e-3f);
    // 큰 world 좌표에서도 상대 위치는 double로 뺀다.
    const Vec3d far(1.0e9, 1.0e9, 0.0);
    const SpatialParams farRight =
        ComputeSpatialParams(linear, 1, far, Quatd::Identity(), far + Vec3d(0.0, 100.0, 0.0));
    CHECK(math::Abs(farRight.azimuthDegrees - 90.0f) < 1.0e-3f);
    // 같은 자리는 방향이 없어 비공간화 1이다.
    CHECK(ComputeSpatialParams(linear, 1, origin, Quatd::Identity(), origin).nonSpatializedAmount ==
          1.0f);
    // attenuate·spatialize는 따로다. stereo 원본은 Direct에 감쇠만 한다.
    SoundAttenuation panOnly = linear;
    panOnly.attenuate = false;
    const SpatialParams noAttenuation =
        ComputeSpatialParams(panOnly, 1, origin, Quatd::Identity(), Vec3d(9000.0, 0.0, 0.0));
    CHECK(noAttenuation.attenuationGain == 1.0f);
    CHECK(noAttenuation.azimuthPan);
    SoundAttenuation attenuateOnly = linear;
    attenuateOnly.spatialize = false;
    CHECK(!ComputeSpatialParams(attenuateOnly, 1, origin, Quatd::Identity(), Vec3d(0.0, 100.0, 0.0))
               .azimuthPan);
    const SpatialParams stereo =
        ComputeSpatialParams(linear, 2, origin, Quatd::Identity(), Vec3d(2200.0, 0.0, 0.0));
    CHECK(!stereo.azimuthPan);
    CHECK(math::Abs(stereo.attenuationGain - 0.5f) < 1.0e-5f);
}

TEST(EngineAudioWorld_PlayAtPansLeftRightAndFront) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    CHECK(scope.audio->Scene().IsValid());
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    auto right = scope.audio->PlayAt(sound, Vec3d(0.0, 500.0, 0.0));
    REQUIRE(right);
    REQUIRE(fixture.Frames(4));
    StereoPeak peak = CapturePeak(fixture, chunk);
    // 방위각 90°: R만 1(mono 0.5 그대로).
    CHECK(math::Abs(peak.right - 0.5f) < 1.0e-3f);
    CHECK(peak.left < 1.0e-3f);
    // 리스너를 180° 돌리면 같은 소리가 왼쪽이다.
    scope.audio->SetListener(Vec3d(), Yaw(180.0));
    REQUIRE(fixture.Frames(3));
    peak = CapturePeak(fixture, chunk);
    CHECK(math::Abs(peak.left - 0.5f) < 1.0e-3f);
    CHECK(peak.right < 1.0e-3f);
    REQUIRE(scope.audio->Stop(right.Value(), 0));
    scope.audio->SetListener(Vec3d(), Quatd::Identity());
    REQUIRE(scope.audio->PlayAt(sound, Vec3d(500.0, 0.0, 0.0)));
    REQUIRE(fixture.Frames(4));
    peak = CapturePeak(fixture, chunk);
    // 정면은 양쪽 1/√2다.
    CHECK(math::Abs(peak.left - 0.5f * InverseSqrtTwo) < 1.0e-3f);
    CHECK(math::Abs(peak.right - 0.5f * InverseSqrtTwo) < 1.0e-3f);
    // 감쇠 스냅숏이 없는 위치 재생과 Play2D는 2D다(감쇠가 있어도 Play2D는 2D — UE PlaySound2D).
    CHECK(scope.audio->PlayAt(sound, Vec3d(platform::PlatformMath::QuietNaN<double>(), 0.0, 0.0))
              .Error() == ErrorCode::InvalidArgument);
}

TEST(EngineAudioWorld_DistanceAttenuatesOutput) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    SoundAttenuation attenuation;
    attenuation.spatialize = false;
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, attenuation);
    auto played = scope.audio->PlayAt(sound, Vec3d(2200.0, 0.0, 0.0));
    REQUIRE(played);
    REQUIRE(fixture.Frames(4));
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    const StereoPeak peak = CapturePeak(fixture, chunk);
    // Linear 중간(0.5) × mono Direct(0.25).
    CHECK(math::Abs(peak.left - 0.125f) < 1.0e-3f);
    CHECK(math::Abs(peak.right - 0.125f) < 1.0e-3f);
}

TEST(EngineAudioWorld_SceneRemovalEndsPlaysWithoutCallbacks) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    EventLog log(fixture.allocator);
    PlaybackHandle worldPlay;
    auto engineLoop = fixture.audio->Play2D(sound);
    REQUIRE(engineLoop);
    {
        WorldScope scope(fixture);
        REQUIRE(scope.audio != nullptr);
        PlayOptions options;
        options.onEvent = log.Callback(fixture.allocator);
        auto played = scope.audio->PlayAt(sound, Vec3d(100.0, 0.0, 0.0), Move(options));
        REQUIRE(played);
        worldPlay = played.Value();
        REQUIRE(fixture.Frames(4));
        CHECK(fixture.audio->Status(worldPlay) == PlaybackStatus::Playing);
        const usize before = log.events.Size();
        REQUIRE(scope.Destroy());
        // scene 해제와 함께 콜백이 끊기고 Status는 바로 Finished다.
        CHECK(fixture.audio->Status(worldPlay) == PlaybackStatus::Finished);
        REQUIRE(fixture.Frames(6));
        CHECK(log.events.Size() == before);
    }

    // 엔진 소유 재생은 World 교체를 넘어 이어진다.
    CHECK(fixture.audio->Status(engineLoop.Value()) == PlaybackStatus::Playing);
    // 다음 World는 같은 칸을 새 epoch로 받아 정상 재생한다.
    WorldScope next(fixture);
    REQUIRE(next.audio != nullptr);
    auto again = next.audio->PlayAt(sound, Vec3d(100.0, 0.0, 0.0));
    REQUIRE(again);
    REQUIRE(fixture.Frames(4));
    CHECK(fixture.audio->Status(again.Value()) == PlaybackStatus::Playing);
    REQUIRE(fixture.audio->Stop(engineLoop.Value()));
    REQUIRE(next.Destroy());
    REQUIRE(fixture.RunUntilIdle());
    CHECK(pcm.UseCount() == 2);
    CHECK(sound.UseCount() == 1);
}

TEST(EngineAudioWorld_ComponentFollowsEmitterAndStopsOnDestroy) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.world != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    const SpawnedAudio spawned = SpawnAudio(*scope.world, sound, Vec3d(0.0, 500.0, 0.0), true);
    REQUIRE(!spawned.component.IsNull());
    REQUIRE(fixture.Frames(5));
    bool playing = false;
    PlaybackHandle handle;
    REQUIRE(WithAudio(*scope.world, spawned.component, [&](AudioComponent& audio) {
        playing = audio.IsPlaying();
        handle = audio.Playback();
        CHECK(audio.Emitter().IsValid());
    }));
    CHECK(playing);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    StereoPeak peak = CapturePeak(fixture, chunk);
    CHECK(peak.right > 0.45f);
    CHECK(peak.left < 0.01f);
    // 움직이면 emitter가 따라가 소리가 왼쪽으로 간다.
    REQUIRE(WithAudio(*scope.world, spawned.component, [&](AudioComponent& audio) {
        CHECK(audio.SetRelativeLocation(Vec3d(0.0, -500.0, 0.0)));
    }));
    REQUIRE(fixture.Frames(3));
    peak = CapturePeak(fixture, chunk);
    CHECK(peak.left > 0.45f);
    CHECK(peak.right < 0.01f);
    // 파괴하면 재생이 끝나고 voice·clip이 모두 풀린다.
    REQUIRE(scope.world->DestroyActor(spawned.actor));
    REQUIRE(fixture.Frames(4));
    CHECK(fixture.audio->Status(handle) == PlaybackStatus::Finished);
    REQUIRE(fixture.RunUntilIdle());
    CHECK(pcm.UseCount() == 2);
}

TEST(EngineAudioWorld_ComponentManualPlayAndStopOnDisable) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.world != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    const SpawnedAudio spawned = SpawnAudio(*scope.world, sound, Vec3d(), false);
    REQUIRE(!spawned.component.IsNull());
    REQUIRE(fixture.Frames(3));
    PlaybackHandle first;
    REQUIRE(WithAudio(*scope.world, spawned.component, [&](AudioComponent& audio) {
        CHECK(!audio.IsPlaying());
        CHECK(audio.Play());
        first = audio.Playback();
    }));
    REQUIRE(fixture.Frames(3));
    PlaybackHandle second;
    REQUIRE(WithAudio(*scope.world, spawned.component, [&](AudioComponent& audio) {
        CHECK(audio.IsPlaying());
        CHECK(audio.SetVolumeMultiplier(0.5f));
        CHECK(audio.SetVolumeMultiplier(9.0f).Error() == ErrorCode::InvalidArgument);
        // 재생 중 Play는 이전 재생을 멈추고 새로 시작한다.
        CHECK(audio.Play());
        second = audio.Playback();
    }));
    CHECK(!(first == second));
    REQUIRE(fixture.Frames(4));
    CHECK(fixture.audio->Status(first) == PlaybackStatus::Finished);
    CHECK(fixture.audio->Status(second) == PlaybackStatus::Playing);
    // 꺼지면(stopOnDisable 기본) 멈춘다.
    REQUIRE(WithAudio(*scope.world, spawned.component,
                      [&](AudioComponent& audio) { CHECK(audio.SetEnabled(false)); }));
    REQUIRE(fixture.Frames(4));
    CHECK(fixture.audio->Status(second) == PlaybackStatus::Finished);
}

TEST(EngineAudioWorld_EmitterRemovedBeforePlayEndsWithoutSound) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    CHECK(scope.audio->RegisterEmitter(Vec3d(platform::PlatformMath::QuietNaN<double>(), 0.0, 0.0))
              .Error() == ErrorCode::InvalidArgument);
    auto emitter = scope.audio->RegisterEmitter(Vec3d(100.0, 0.0, 0.0));
    REQUIRE(emitter);
    EventLog log(fixture.allocator);
    PlayOptions options;
    options.onEvent = log.Callback(fixture.allocator);
    REQUIRE(scope.audio->PlayAttached(emitter.Value(), sound, Move(options)));
    // 같은 프레임에 emitter를 놓으면 그 재생은 소리 없이 EmitterRemoved로 끝난다.
    scope.audio->UnregisterEmitter(emitter.Value());
    REQUIRE(fixture.Frames(4));
    REQUIRE(log.events.Size() == 1);
    CHECK(log.events[0].reason == PlaybackEndReason::EmitterRemoved);
    // 낡은 emitter에 붙이는 재생은 Ignored다.
    auto stale = scope.audio->PlayAttached(emitter.Value(), sound);
    REQUIRE(stale);
    CHECK(fixture.audio->Status(stale.Value()) == PlaybackStatus::Ignored);
}

TEST(EngineAudioWorld_ReleasedSceneIgnoresLateWork) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true);
    // OnWorldEndPlay 뒤(scene을 놓은 뒤)에 온 등록·재생은 무효·Ignored다.
    scope.audio->OnWorldEndPlay(*scope.world);
    CHECK(!scope.audio->Scene().IsValid());
    CHECK(scope.audio->RegisterEmitter(Vec3d()).Error() == ErrorCode::InvalidArgument);
    auto late = scope.audio->Play2D(sound);
    REQUIRE(late);
    CHECK(fixture.audio->Status(late.Value()) == PlaybackStatus::Ignored);
    REQUIRE(fixture.Frames(2));
    CHECK(fixture.audio->Status(late.Value()) == PlaybackStatus::Finished);
}

TEST(EngineAudioWorld_EditorAndTryoutWorldsReleaseScenes) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    {
        // 편집 World는 OnWorldEndPlay가 없어 Deinitialize에서 scene을 놓는다.
        WorldScope editor(fixture, WorldKind::Editor, false);
        REQUIRE(editor.audio != nullptr);
        auto played = editor.audio->PlayAt(sound, Vec3d(100.0, 0.0, 0.0));
        REQUIRE(played);
        REQUIRE(fixture.Frames(3));
        CHECK(fixture.audio->Status(played.Value()) == PlaybackStatus::Playing);
        REQUIRE(editor.Destroy());
        CHECK(fixture.audio->Status(played.Value()) == PlaybackStatus::Finished);
        REQUIRE(fixture.RunUntilIdle());
    }

    {
        // Tryout Stop = World 파괴: 컴포넌트 재생·emitter·PCM 참조가 모두 끝난다.
        WorldScope tryout(fixture, WorldKind::Tryout);
        REQUIRE(tryout.world != nullptr);
        const SpawnedAudio spawned = SpawnAudio(*tryout.world, sound, Vec3d(0.0, 100.0, 0.0), true);
        REQUIRE(!spawned.component.IsNull());
        REQUIRE(fixture.Frames(4));
        CHECK(fixture.audio->ControlStats().activeVoices == 1);
        REQUIRE(tryout.Destroy());
        REQUIRE(fixture.RunUntilIdle());
    }

    CHECK(pcm.UseCount() == 2);
    CHECK(sound.UseCount() == 1);
}

TEST(EngineAudioWorld_SceneRemovalReachesDeferredPlays) {
    AudioEngineHostOptions options = DeterministicOptions(true);
    options.commandCapacity = 2;
    AudioEngine fixture(options);
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    {
        WorldScope scope(fixture);
        REQUIRE(scope.audio != nullptr);
        for (uint32 index = 0; index < 4; ++index) {
            REQUIRE(scope.audio->PlayAt(sound, Vec3d(100.0, 0.0, 0.0)));
        }

        // 렌더 없이 처리만 해 일부를 Deferred로 남긴 채 World를 없앤다.
        REQUIRE(fixture.engine.RunFrame());
        CHECK(fixture.audio->ControlStats().pendingDeferred != 0);
        REQUIRE(scope.Destroy());
    }

    REQUIRE(fixture.RunUntilIdle());
    // 마지막 Ended는 다음 프레임 시작에 전달되고 그때 슬롯이 풀린다.
    REQUIRE(fixture.Frame());
    CHECK(fixture.audio->ControlStats().pendingDeferred == 0);
    CHECK(fixture.audio->Stats().livePlaybacks == 0);
}

TEST(EngineAudioWorld_NoEngineAudioGivesEmptyHandles) {
    SystemAllocator allocator;
    Engine engine(allocator);
    EngineConfig config;
    config.modules = &test::ProcessModules();
    config.fixedFrameDeltaSeconds = 1.0 / 60.0;
    REQUIRE(engine.Initialize(config));
    const WorldSubsystemType subsystems[] = {AudioWorldSubsystem::Type()};
    WorldCreateDesc desc;
    desc.name = "NoAudio";
    desc.subsystems = subsystems;
    auto world = engine.CreateWorld(desc);
    REQUIRE(world);
    auto* audio = world.Value()->GetSubsystem<AudioWorldSubsystem>();
    REQUIRE(audio != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(allocator, 0.1f, 64);
    auto played = audio->Play2D(MakeSound(allocator, pcm, false));
    REQUIRE(played);
    CHECK(!played.Value().IsValid());
    CHECK(audio->Status(played.Value()) == PlaybackStatus::Finished);
    CHECK(audio->RegisterEmitter(Vec3d()).Error() == ErrorCode::InvalidArgument);
    REQUIRE(engine.Shutdown());
}

TEST(EngineAudioWorld_StartedCallbackMayDestroyWorld) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    uint32 calls = 0;
    auto callback =
        PlaybackEventCallback::TryCreate(fixture.allocator, [&](const PlaybackEvent&) noexcept {
            // Tick 밖(OnFrameBegin)이라 즉시 파괴다 — 실행 중인 이 콜백 객체는 살아 있어야 한다.
            ++calls;
            (void)scope.Destroy();
        });
    REQUIRE(callback);
    PlayOptions options;
    options.onEvent = Move(callback.Value());
    REQUIRE(scope.audio->PlayAt(sound, Vec3d(100.0, 0.0, 0.0), Move(options)));
    REQUIRE(fixture.Frames(6));
    CHECK(calls == 1);
    REQUIRE(fixture.RunUntilIdle());
}

TEST(EngineAudioWorld_SameFrameWorldSwapKeepsNewScene) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    WorldScope first(fixture);
    REQUIRE(first.audio != nullptr);
    const AudioSceneId oldScene = first.audio->Scene();
    auto oldLoop = first.audio->PlayAt(sound, Vec3d(0.0, 500.0, 0.0));
    REQUIRE(oldLoop);
    REQUIRE(fixture.Frames(3));
    // 한 프레임 사이에 옛 World를 없애고 새 World를 만들어 재생한다(같은 scene 칸, 새 epoch).
    REQUIRE(first.Destroy());
    WorldScope second(fixture);
    REQUIRE(second.audio != nullptr);
    CHECK(second.audio->Scene().index == oldScene.index);
    CHECK(second.audio->Scene().epoch != oldScene.epoch);
    auto newLoop = second.audio->PlayAt(sound, Vec3d(0.0, -500.0, 0.0));
    REQUIRE(newLoop);
    REQUIRE(fixture.Frames(4));
    CHECK(fixture.audio->Status(oldLoop.Value()) == PlaybackStatus::Finished);
    CHECK(fixture.audio->Status(newLoop.Value()) == PlaybackStatus::Playing);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    // 옛 오른쪽 loop는 끝나고 새 왼쪽 loop만 들린다.
    const StereoPeak peak = CapturePeak(fixture, chunk);
    CHECK(peak.left > 0.45f);
    CHECK(peak.right < 0.01f);
    CHECK(fixture.audio->ControlStats().activeVoices == 1);
}

TEST(EngineAudioWorld_SameFrameEmitterReuseIsolatesGenerations) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, PanOnly());
    EventLog log(fixture.allocator);
    auto oldEmitter = scope.audio->RegisterEmitter(Vec3d(0.0, -500.0, 0.0));
    REQUIRE(oldEmitter);
    PlayOptions options;
    options.onEvent = log.Callback(fixture.allocator);
    REQUIRE(scope.audio->PlayAttached(oldEmitter.Value(), sound, Move(options)));
    scope.audio->UnregisterEmitter(oldEmitter.Value());
    // 놓은 칸을 같은 프레임에 새 generation으로 다시 받는다.
    auto newEmitter = scope.audio->RegisterEmitter(Vec3d(0.0, 500.0, 0.0));
    REQUIRE(newEmitter);
    CHECK(newEmitter.Value().slot == oldEmitter.Value().slot);
    CHECK(newEmitter.Value().generation != oldEmitter.Value().generation);
    auto played = scope.audio->PlayAttached(newEmitter.Value(), sound);
    REQUIRE(played);
    REQUIRE(fixture.Frames(4));
    REQUIRE(log.events.Size() == 1);
    CHECK(log.events[0].reason == PlaybackEndReason::EmitterRemoved);
    CHECK(fixture.audio->Status(played.Value()) == PlaybackStatus::Playing);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    const StereoPeak peak = CapturePeak(fixture, chunk);
    CHECK(peak.right > 0.45f);
    CHECK(peak.left < 0.01f);
}

TEST(EngineAudioWorld_PlaybackOutlivesCollectedSoundAsset) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 4800);
    PlaybackHandle handle;
    {
        // 에셋 객체 Registry에 SoundClip을 만들어 스냅숏을 떠서 재생한 뒤, Registry째 수거한다.
        TypeRegistry types(fixture.allocator);
        ModuleRegistration registration(types, fixture.allocator);
        REQUIRE(RegisterAudioAssetsTypes(registration));
        {
            ObjectRegistry objects(fixture.allocator, types, {0x45415544474331ull, 1});
            auto clip = objects.NewObject<SoundClip>(objects.OwnerAccess(), TypeIdOf<SoundClip>());
            REQUIRE(clip);
            auto borrow = objects.Resolve<SoundClip>(objects.OwnerAccess(), clip.Value().Handle());
            REQUIRE(borrow);
            SoundClip& sound = *borrow.Value().Get().Value();
            REQUIRE(sound.SetPcm(pcm));
            auto playable = sound.MakePlayable(fixture.allocator, objects, objects.OwnerAccess());
            REQUIRE(playable);
            auto played = scope.audio->Play2D(playable.Value());
            REQUIRE(played);
            handle = played.Value();
        }

        CHECK(registration.Unregister());
    }

    // 객체가 사라져도 재생은 스냅숏으로 끝까지 간다(4800 frame).
    Array<float> capture(fixture.allocator);
    REQUIRE(capture.TryResize(PumpFrames * 2 * 12));
    fixture.audio->Device()->NullOutput()->CaptureTo(Span<float>(capture.Data(), capture.Size()));
    REQUIRE(fixture.Frames(12));
    uint32 loud = 0;
    for (usize frame = 0; frame + 1 < capture.Size(); frame += 2) {
        loud += capture[frame] == 0.25f ? 1 : 0;
    }

    CHECK(loud == 4800);
    CHECK(fixture.audio->Status(handle) == PlaybackStatus::Finished);
    REQUIRE(fixture.RunUntilIdle());
    CHECK(pcm.UseCount() == 1);
}

TEST(EngineAudioWorld_SpatialPlaybackRampsWithAttenuation) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    SoundAttenuation attenuation;
    attenuation.spatialize = false;
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true, 0, 0, attenuation);
    auto played = scope.audio->PlayAt(sound, Vec3d(2200.0, 0.0, 0.0));
    REQUIRE(played);
    REQUIRE(fixture.Frames(4));
    // 공간 재생은 묶음마다 다시 게시되지만 사용자 ramp 중간값은 그 위에 곱해진다(3.35.6절).
    REQUIRE(
        scope.audio->SetParameter(played.Value(), PlaybackParameter::Volume, 0.0f, PumpFrames * 8));
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    uint32 between = 0;
    float previous = 1.0f;
    for (uint32 frame = 0; frame < 12; ++frame) {
        const StereoPeak peak = CapturePeak(fixture, chunk);
        CHECK(peak.left <= previous + 1.0e-6f);
        between += peak.left > 0.005f && peak.left < 0.12f ? 1 : 0;
        previous = peak.left;
    }

    CHECK(between >= 3);
    CHECK(previous == 0.0f);
}

TEST(EngineAudioWorld_TwoDimensionalPlaysIgnoreAttenuation) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.5f, 480);
    SoundAttenuation farAway;
    farAway.innerRadius = 0.0f;
    // Play2D는 감쇠 스냅숏이 있어도 2D다(UE PlaySound2D).
    const SoundSourceRef attenuated = MakeSound(fixture.allocator, pcm, true, 0, 0, farAway);
    auto flat = scope.audio->Play2D(attenuated);
    REQUIRE(flat);
    REQUIRE(fixture.Frames(4));
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    StereoPeak peak = CapturePeak(fixture, chunk);
    CHECK(math::Abs(peak.left - 0.25f) < 1.0e-4f);
    CHECK(math::Abs(peak.right - 0.25f) < 1.0e-4f);
    REQUIRE(scope.audio->Stop(flat.Value(), 0));
    // 감쇠 스냅숏이 없는 위치 재생도 2D다.
    const SoundSourceRef plain = MakeSound(fixture.allocator, pcm, true);
    REQUIRE(scope.audio->PlayAt(plain, Vec3d(0.0, 90000.0, 0.0)));
    REQUIRE(fixture.Frames(4));
    peak = CapturePeak(fixture, chunk);
    CHECK(math::Abs(peak.left - 0.25f) < 1.0e-4f);
    CHECK(math::Abs(peak.right - 0.25f) < 1.0e-4f);
}

TEST(EngineAudioWorld_WorldCommandsTouchOnlyItsOwnPlaybacks) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope first(fixture);
    WorldScope second(fixture);
    REQUIRE(first.audio != nullptr);
    REQUIRE(second.audio != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.1f, 480);
    const SoundSourceRef sound = MakeSound(fixture.allocator, pcm, true);
    auto mine = first.audio->Play2D(sound);
    auto engineOwned = fixture.audio->Play2D(sound);
    REQUIRE(mine);
    REQUIRE(engineOwned);
    REQUIRE(fixture.Frames(3));
    // 다른 World·엔진 소유 handle은 건드리지 않는다.
    CHECK(second.audio->Stop(mine.Value()));
    CHECK(first.audio->Stop(engineOwned.Value()));
    CHECK(second.audio->Status(mine.Value()) == PlaybackStatus::Finished);
    CHECK(first.audio->Stop(PlaybackHandle{}).Error() == ErrorCode::InvalidArgument);
    REQUIRE(fixture.Frames(3));
    CHECK(first.audio->Status(mine.Value()) == PlaybackStatus::Playing);
    CHECK(fixture.audio->Status(engineOwned.Value()) == PlaybackStatus::Playing);
}

TEST(EngineAudioWorld_ReimportPlaysNewPcmOnlyForNewPlays) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    TypeRegistry types(fixture.allocator);
    ModuleRegistration registration(types, fixture.allocator);
    REQUIRE(RegisterAudioAssetsTypes(registration));
    {
        ObjectRegistry objects(fixture.allocator, types, {0x45415552494d31ull, 1});
        MemoryBulkSource bulkSource(fixture.allocator);
        auto made = objects.NewObject<SoundClip>(objects.OwnerAccess(), TypeIdOf<SoundClip>());
        REQUIRE(made);
        auto borrow = objects.Resolve<SoundClip>(objects.OwnerAccess(), made.Value().Handle());
        REQUIRE(borrow);
        SoundClip& clip = *borrow.Value().Get().Value();
        // 원본 bulk(Int16 mono 480 frame, 값 v)를 넣는 것이 임포트·재임포트가 하는 일이다.
        const auto setSource = [&](int16 value) {
            Array<uint8> bytes(fixture.allocator);
            for (uint32 frame = 0; frame < 480; ++frame) {
                (void)bytes.TryPushBack(static_cast<uint8>(static_cast<uint16>(value)));
                (void)bytes.TryPushBack(static_cast<uint8>(static_cast<uint16>(value) >> 8));
            }

            auto ref = bulkSource.Add(Move(bytes));
            REQUIRE(ref);
            clip.channelCount = 1;
            clip.sampleRate = TestRate;
            clip.frameCount = 480;
            clip.source.format = SoundSourceStorage::Int16;
            clip.source.bulk = ref.Value();
        };
        clip.looping = true;
        setSource(4096);
        REQUIRE(clip.LoadEditorPcm(fixture.allocator, bulkSource));
        const uint64 firstRevision = clip.PcmRevision();
        auto first = clip.MakePlayable(fixture.allocator, objects, objects.OwnerAccess());
        REQUIRE(first);
        auto oldPlay = fixture.audio->Play2D(first.Value());
        REQUIRE(oldPlay);
        REQUIRE(fixture.Frames(3));
        // 재임포트(원본이 바뀜) → 다시 올리면 revision이 오르고 새 재생부터 새 PCM이다.
        setSource(12288);
        REQUIRE(clip.LoadEditorPcm(fixture.allocator, bulkSource));
        CHECK(clip.PcmRevision() == firstRevision + 1);
        auto second = clip.MakePlayable(fixture.allocator, objects, objects.OwnerAccess());
        REQUIRE(second);
        CHECK(second.Value().Get()->revision != first.Value().Get()->revision);
        REQUIRE(fixture.audio->Play2D(second.Value()));
        REQUIRE(fixture.Frames(3));
        Array<float> chunk(fixture.allocator);
        REQUIRE(chunk.TryResize(PumpFrames * 2));
        // 옛 재생(0.125)과 새 재생(0.375)이 함께 돈다: mono Direct 0.5배 합 = 0.25.
        StereoPeak peak = CapturePeak(fixture, chunk);
        CHECK(math::Abs(peak.left - 0.25f) < 1.0e-4f);
        REQUIRE(fixture.audio->Stop(oldPlay.Value(), 0));
        REQUIRE(fixture.Frames(3));
        peak = CapturePeak(fixture, chunk);
        CHECK(math::Abs(peak.left - 0.1875f) < 1.0e-4f);
    }

    CHECK(registration.Unregister());
}

} // namespace audio_world_tests_file
