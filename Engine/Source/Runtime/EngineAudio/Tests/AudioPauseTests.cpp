#include "AudioEngineTestSupport.h"

#include <AudioAssets/MixSnapshotData.h>
#include <Base/InitializerList.h>
#include <AudioAssets/SoundCategoryChain.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <EngineAudio/AudioWorldSubsystem.h>
#include <TestSupport/Test.h>
#include <World/World.h>

// AU7e2 장면 일시정지 시험이다(Docs/Design/AudioSystem 3.44.8절): World pause·SetScenePaused 입력과
// 단일 스텝, UI 재생 예외, 일시정지 중 수락·정지, record 시계 동결(fade-in·차폐 간격)과 mix 시계
// 진행(믹스 fade), voice 자리 차지, 무음 믹스로 끝나는 one-shot, 반복 pause 뒤 SeekRestart 위치.
// Inline 제어 + Manual 렌더로 프레임·Pump만으로 순서를 만든다. 출력은 mono 상수 0.2의 Direct(0.5배)
// = 0.1 기준이다.
using namespace rocks;
namespace audio_pause_tests_file {

using namespace rocks::engine_audio_test;

namespace {

constexpr uint32 StartFrames = 6;
constexpr float DirectLevel = 0.1f;
constexpr uint32 OneSecond = TestRate;
// 일시정지 전환이 출력에 닿기까지의 프레임 수다(묶음 반영 → 믹서 fade 64 frame — 한 프레임 안).
constexpr uint32 SettleFrames = 2;
// 상한(audio.voices.max 기본)과 그것을 채우는 재생의 우선순위·volume이다.
constexpr uint32 VoiceLimit = 32;
constexpr float FillerPriority = 10.0f;
constexpr float FillerVolume = 0.5f;
constexpr uint32 CaptureFrames = 48;
// 복귀 fade-in이 끝나는 데 걸리는 상한(mix frame — 묶음 2개 + 믹서 ramp)이다.
constexpr uint64 ResumeSettleFrames = uint64{PumpFrames} * 2 + audio::DefaultParamRampFrames;
// 시험용 분류·믹스 키다.
constexpr uint64 UiCategoryKey = 0x7001;
constexpr uint64 GameCategoryKey = 0x7002;
constexpr uint64 MenuMixKey = 0x7101;

bool Near(float actual, float expected, float tolerance = 1.0e-4f) noexcept {
    return math::Abs(actual - expected) <= tolerance;
}

float Peak(AudioEngine& fixture) noexcept {
    Array<float> chunk(fixture.allocator);
    if (!chunk.TryResize(static_cast<usize>(PumpFrames) * 2)) {
        return -1.0f;
    }

    return CapturePeak(fixture, chunk).left;
}

SoundSourceRef Playable(Allocator& allocator, const SharedPtr<const SoundPcm>& pcm, bool looping,
                        SoundVirtualizationMode mode = SoundVirtualizationMode::Restart,
                        float priority = DefaultSoundPriority,
                        const Optional<SoundAttenuation>& attenuation = {}) noexcept {
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.looping = looping;
    playable.virtualizationMode = mode;
    playable.priority = priority;
    playable.attenuation = attenuation;
    auto made = SoundSourceRef::TryCreate(allocator, playable);
    return made ? made.Value() : SoundSourceRef{};
}

uint32 CountFor(const EventLog& log, PlaybackHandle handle, PlaybackEventKind kind) noexcept {
    uint32 count = 0;
    for (const PlaybackEvent& event : log.events) {
        count += event.handle == handle && event.kind == kind ? 1u : 0u;
    }

    return count;
}

PlaybackEndReason EndOf(const EventLog& log, PlaybackHandle handle) noexcept {
    for (const PlaybackEvent& event : log.events) {
        if (event.handle == handle && event.kind == PlaybackEventKind::Ended) {
            return event.reason;
        }
    }

    return PlaybackEndReason::None;
}

PlaybackHandle PlayIn(AudioEngine& fixture, EventLog& log, AudioSceneId scene,
                      const SoundSourceRef& sound, PlayOptions options = {}) noexcept {
    options.onEvent = log.Callback(fixture.allocator);
    auto played = fixture.audio->Play2DInScene(scene, sound, Move(options));
    return played ? played.Value() : PlaybackHandle{};
}

SoundCategoryChainRef UiChain(Allocator& allocator, uint64 key, bool ui) noexcept {
    auto made = SharedPtr<SoundCategoryChain>::TryMake(allocator);
    if (!made) {
        return {};
    }

    SoundCategoryChain& chain = *made.Value().Get();
    chain.links[0].key = key;
    chain.links[0].properties.isUiSound = ui;
    chain.depth = 1;
    return made.Value();
}

MixSnapshotRef VolumeMix(Allocator& allocator, uint64 category, float volume,
                         float fadeIn) noexcept {
    auto made = SharedPtr<MixSnapshotData>::TryMake(allocator);
    if (!made) {
        return {};
    }

    MixSnapshotData& data = *made.Value().Get();
    data.key = MenuMixKey;
    data.fadeInTime = fadeIn;
    data.fadeOutTime = 0.0f;
    data.adjusterCount = 1;
    data.adjusters[0].categoryKey = category;
    data.adjusters[0].volume = volume;
    return made.Value();
}

struct WorldScope {
    explicit WorldScope(AudioEngine& fixture) : engine(fixture) {
        subsystems[0] = AudioWorldSubsystem::Type();
        WorldCreateDesc desc;
        desc.name = "AudioPauseTest";
        desc.subsystems = Span<const WorldSubsystemType>(subsystems, 1);
        desc.kind = WorldKind::Game;
        desc.beginPlay = true;
        auto created = fixture.engine.CreateWorld(desc);
        world = created ? created.Value() : nullptr;
        audio = world != nullptr ? world->GetSubsystem<AudioWorldSubsystem>() : nullptr;
    }

    AudioEngine& engine;
    WorldSubsystemType subsystems[1];
    World* world = nullptr;
    AudioWorldSubsystem* audio = nullptr;
};

} // namespace

// World pause가 그 World 재생을 멈추고(64 frame fade 뒤 무음) 풀면 이어진다. 엔진 소유 재생은
// 멈추지 않는다. 디버그 단일 스텝 frame은 일시정지가 아니다.
TEST(EngineAudio_PauseFollowsWorld) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    REQUIRE(scope.audio->Play2D(MakeSound(fixture.allocator, pcm, true)));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel));

    scope.world->SetPaused(true);
    REQUIRE(fixture.Frames(SettleFrames));
    CHECK(Peak(fixture) == 0.0f);
    CHECK(fixture.audio->ControlStats().pausedPlaybacks == 1);
    CHECK(fixture.audio->Device()->Mixer().CountersSnapshot().pausedVoices == 1);

    // 단일 스텝 frame에는 흐르고 다음 frame에 다시 멈춘다.
    REQUIRE(scope.world->Clock().RequestSingleStep());
    REQUIRE(fixture.Frame());
    CHECK(scope.world->LastStep().singleStep);
    CHECK(fixture.audio->ControlStats().pausedPlaybacks == 0);
    REQUIRE(fixture.Frame());
    CHECK(fixture.audio->ControlStats().pausedPlaybacks == 1);

    // 엔진 소유 재생은 World pause와 상관없이 난다.
    auto engineOwned = fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true));
    REQUIRE(engineOwned);
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel));
    REQUIRE(fixture.audio->Stop(engineOwned.Value(), 0));
    REQUIRE(fixture.Frames(SettleFrames));
    CHECK(Peak(fixture) == 0.0f);

    scope.world->SetPaused(false);
    REQUIRE(fixture.Frames(SettleFrames));
    CHECK(Near(Peak(fixture), DirectLevel));
    CHECK(fixture.audio->ControlStats().pausedPlaybacks == 0);
    CHECK(fixture.audio->Device()->Mixer().CountersSnapshot().pausedVoices == 0);
}

// 코드 일시정지는 World 값과 OR다. 엔진 scene·낡은 scene은 InvalidArgument다.
TEST(EngineAudio_PauseCodeOrWorld) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    WorldScope scope(fixture);
    REQUIRE(scope.audio != nullptr);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    REQUIRE(scope.audio->Play2D(MakeSound(fixture.allocator, pcm, true)));
    REQUIRE(fixture.Frames(StartFrames));

    REQUIRE(scope.audio->SetPaused(true));
    REQUIRE(fixture.Frames(SettleFrames));
    CHECK(Peak(fixture) == 0.0f);
    // World pause를 걸고 코드 값을 풀어도 멈춰 있다.
    scope.world->SetPaused(true);
    REQUIRE(scope.audio->SetPaused(false));
    REQUIRE(fixture.Frames(SettleFrames));
    CHECK(Peak(fixture) == 0.0f);
    scope.world->SetPaused(false);
    REQUIRE(fixture.Frames(SettleFrames));
    CHECK(Near(Peak(fixture), DirectLevel));

    const AudioSceneId engineScene{EngineSceneIndex, EngineSceneEpoch};
    CHECK(fixture.audio->SetScenePaused(engineScene, true).Error() == ErrorCode::InvalidArgument);
    const AudioSceneId stale{scope.audio->Scene().index, scope.audio->Scene().epoch + 1};
    CHECK(fixture.audio->SetScenePaused(stale, true).Error() == ErrorCode::InvalidArgument);
}

// UI 재생(옵션·분류 표지)은 일시정지된 scene에서도 난다. 일시정지된 재생만 센다.
TEST(EngineAudio_PauseSkipsUiPlaybacks) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    EventLog log(fixture.allocator);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    const SoundSourceRef loop = MakeSound(fixture.allocator, pcm, true);
    PlayOptions optionUi;
    optionUi.isUiSound = true;
    REQUIRE(PlayIn(fixture, log, scene, loop, Move(optionUi)).IsValid());
    PlayOptions categoryUi;
    categoryUi.category = UiChain(fixture.allocator, UiCategoryKey, true);
    REQUIRE(PlayIn(fixture, log, scene, loop, Move(categoryUi)).IsValid());
    REQUIRE(PlayIn(fixture, log, scene, loop).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 3.0f));

    REQUIRE(fixture.audio->SetScenePaused(scene, true));
    REQUIRE(fixture.Frames(SettleFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 2.0f));
    CHECK(fixture.audio->ControlStats().pausedPlaybacks == 1);
}

// 일시정지 중 수락한 재생은 Started 없이 자리를 쥐고 기다리다 풀리면 시작한다. 일시정지 중 Stop은
// voice 없는 재생을 바로, voice 있는 재생을 소리 없이(정지 fade 뒤) 끝낸다.
TEST(EngineAudio_PauseDefersStartAndStopsSilently) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    EventLog log(fixture.allocator);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    const SoundSourceRef loop = MakeSound(fixture.allocator, pcm, true);
    const PlaybackHandle playing = PlayIn(fixture, log, scene, loop);
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(CountFor(log, playing, PlaybackEventKind::Started) == 1);

    REQUIRE(fixture.audio->SetScenePaused(scene, true));
    REQUIRE(fixture.Frames(SettleFrames));
    const PlaybackHandle waiting = PlayIn(fixture, log, scene, loop);
    const PlaybackHandle dropped = PlayIn(fixture, log, scene, loop);
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(CountFor(log, waiting, PlaybackEventKind::Started) == 0);
    CHECK(fixture.audio->ControlStats().voicelessPlaybacks == 2);
    CHECK(fixture.audio->ControlStats().pausedPlaybacks == 3);

    // 정지 fade(기본 512 frame) 동안에도 소리가 새지 않는다.
    REQUIRE(fixture.audio->Stop(playing));
    REQUIRE(fixture.audio->Stop(dropped));
    for (uint32 frame = 0; frame < StartFrames; ++frame) {
        CHECK(Peak(fixture) == 0.0f);
    }

    CHECK(EndOf(log, playing) == PlaybackEndReason::Stopped);
    CHECK(EndOf(log, dropped) == PlaybackEndReason::Stopped);
    CHECK(CountFor(log, dropped, PlaybackEventKind::Started) == 0);

    REQUIRE(fixture.audio->SetScenePaused(scene, false));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(CountFor(log, waiting, PlaybackEventKind::Started) == 1);
    CHECK(Near(Peak(fixture), DirectLevel));
}

// record 시계: 사용자 fade-in은 일시정지 동안 멈췄다가 그 값에서 이어진다(UE 컴포넌트 fader).
TEST(EngineAudio_PauseFreezesFadeIn) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    EventLog log(fixture.allocator);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    PlayOptions options;
    options.fadeInFrames = OneSecond;
    REQUIRE(PlayIn(fixture, log, scene, MakeSound(fixture.allocator, pcm, true), Move(options))
                .IsValid());
    REQUIRE(fixture.Frames(StartFrames * 2));
    const float before = Peak(fixture);
    CHECK(before > 0.0f && before < DirectLevel * 0.5f);

    REQUIRE(fixture.audio->SetScenePaused(scene, true));
    REQUIRE(fixture.Frames(StartFrames * 4));
    REQUIRE(fixture.audio->SetScenePaused(scene, false));
    REQUIRE(fixture.Frames(SettleFrames));
    // 묶음당 ramp 증가(0.1 × 512 / 48000)의 몇 배 안이다. 멈추지 않았다면 두 배쯤이다.
    const float step = DirectLevel * static_cast<float>(PumpFrames) / static_cast<float>(OneSecond);
    const float after = Peak(fixture);
    CHECK(after > before - step && after < before + step * 6.0f);
}

// record 시계: 차폐 검사 간격도 일시정지 동안 멈춘다(질의가 오지 않는다).
TEST(EngineAudio_PauseFreezesOcclusionInterval) {
    struct Query final : public AudioOcclusionQuery {
        bool IsOccluded(const Vec3d&, const Vec3d&, uint64) noexcept override {
            ++calls;
            return false;
        }

        uint32 calls = 0;
    };

    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    Query query;
    fixture.audio->SetSceneOcclusionQuery(scene, &query);
    SoundAttenuation attenuation;
    attenuation.spatialize = false;
    attenuation.shapeExtents.x = 100000.0f;
    attenuation.enableOcclusion = true;
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    auto played = fixture.audio->PlayAtInScene(scene,
                                               Playable(fixture.allocator, pcm, true,
                                                        SoundVirtualizationMode::Restart,
                                                        DefaultSoundPriority, attenuation),
                                               Vec3d(500.0, 0.0, 0.0));
    REQUIRE(played);
    REQUIRE(fixture.Frames(StartFrames));
    REQUIRE(fixture.audio->SetScenePaused(scene, true));
    REQUIRE(fixture.Frame());
    const uint64 requests = fixture.audio->ControlStats().occlusionRequests;
    // 간격(0.1 s ≈ 10 묶음)의 몇 배를 일시정지로 보낸다.
    REQUIRE(fixture.Frames(40));
    CHECK(fixture.audio->ControlStats().occlusionRequests == requests);
    REQUIRE(fixture.audio->SetScenePaused(scene, false));
    REQUIRE(fixture.Frames(20));
    CHECK(fixture.audio->ControlStats().occlusionRequests > requests);
}

// mix 시계: 일시정지 메뉴 믹스의 fade-in은 일시정지 중에도 흐른다(UE 믹스는 장치 시계). 믹스가
// 분류를 0으로 만들면 일시정지된 one-shot은 Inaudible로 끝나고, loop는 voice 없이 남는다.
TEST(EngineAudio_PauseMixFadeFlowsAndSilencesOneShot) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    EventLog log(fixture.allocator);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond * 4);
    const SoundCategoryChainRef game = UiChain(fixture.allocator, GameCategoryKey, false);
    PlayOptions shotOptions;
    shotOptions.category = game;
    const PlaybackHandle shot =
        PlayIn(fixture, log, scene, Playable(fixture.allocator, pcm, false), Move(shotOptions));
    PlayOptions loopOptions;
    loopOptions.category = game;
    const PlaybackHandle loop =
        PlayIn(fixture, log, scene, Playable(fixture.allocator, pcm, true), Move(loopOptions));
    // 엔진 scene 재생으로 믹스 fade를 본다(같은 분류).
    PlayOptions engineOptions;
    engineOptions.category = game;
    REQUIRE(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true), Move(engineOptions)));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 3.0f));

    REQUIRE(fixture.audio->SetScenePaused(scene, true));
    REQUIRE(fixture.audio->PushMix(VolumeMix(fixture.allocator, GameCategoryKey, 0.0f, 0.2f)));
    // 0.2 s ≈ 19 묶음 뒤에는 엔진 재생도 0이고 one-shot은 끝났다.
    REQUIRE(fixture.Frames(30));
    CHECK(Peak(fixture) == 0.0f);
    CHECK(EndOf(log, shot) == PlaybackEndReason::Inaudible);
    CHECK(CountFor(log, loop, PlaybackEventKind::Ended) == 0);
    // 일시정지 loop와 엔진 loop가 voice 없이 남는다.
    CHECK(fixture.audio->ControlStats().voicelessPlaybacks == 2);
}

// voice 배정: 일시정지된 voice 없는 재생도 자리를 차지한다(UE PausedUpdate도 정렬·컬링). 자리
// 밖으로 밀린 일시정지 one-shot은 소리 없이 VoiceLimit로 끝난다.
TEST(EngineAudio_PausedPlaybacksHoldSlots) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    EventLog log(fixture.allocator);
    const SharedPtr<const SoundPcm> silent = MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    REQUIRE(fixture.audio->SetScenePaused(scene, true));
    REQUIRE(fixture.Frame());
    for (uint32 index = 0; index < VoiceLimit; ++index) {
        PlayOptions options;
        options.volume = FillerVolume;
        REQUIRE(PlayIn(fixture, log, scene,
                       Playable(fixture.allocator, silent, true, SoundVirtualizationMode::Restart,
                                FillerPriority),
                       Move(options))
                    .IsValid());
    }

    // 낮은 W의 UI loop는 자리를 얻지 못한다(일시정지 재생이 자리를 쥐고 있다).
    PlayOptions ui;
    ui.isUiSound = true;
    const PlaybackHandle uiLoop =
        PlayIn(fixture, log, scene, Playable(fixture.allocator, pcm, true), Move(ui));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Peak(fixture) == 0.0f);
    CHECK(fixture.audio->ControlStats().voicelessPlaybacks == VoiceLimit + 1);
    CHECK(CountFor(log, uiLoop, PlaybackEventKind::Started) == 0);

    // 높은 W의 UI one-shot은 일시정지 재생 하나를 밀어내고 바로 난다. 밀린 재생은 loop라 남는다.
    PlayOptions loud;
    loud.isUiSound = true;
    loud.priority = FillerPriority * 2.0f;
    const PlaybackHandle uiShot =
        PlayIn(fixture, log, scene, Playable(fixture.allocator, pcm, false), Move(loud));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(CountFor(log, uiShot, PlaybackEventKind::Started) == 1);
    CHECK(Near(Peak(fixture), DirectLevel));

    // 자리 밖 일시정지 one-shot은 시작하지 않고 VoiceLimit로 끝난다.
    PlayOptions low;
    low.volume = FillerVolume;
    const PlaybackHandle pausedShot =
        PlayIn(fixture, log, scene, Playable(fixture.allocator, pcm, false), Move(low));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(EndOf(log, pausedShot) == PlaybackEndReason::VoiceLimit);
    CHECK(CountFor(log, pausedShot, PlaybackEventKind::Started) == 0);
}

namespace {

// 반복 일시정지 뒤 SeekRestart 위치(3.44.6절): ramp loop L을 일시정지·해제를 두 번 한 뒤 stealAt
// 프레임에 상한을 채우는 재생으로 빼앗고 resumeAt에 돌려준다. steal이 false면 기준 출력이다.
// pausedSteal이면 두 번째 일시정지 중에 빼앗는다.
struct PauseResumeRun {
    explicit PauseResumeRun(Allocator& allocator) noexcept : capture(allocator) {
    }

    Array<float> capture;
    uint64 resumeMixFrame = 0;
    uint32 started = 0;
};

void RunPauseResume(bool steal, bool pausedSteal, PauseResumeRun& out) noexcept {
    constexpr uint32 FirstPause = 2;
    constexpr uint32 FirstResume = 5;
    constexpr uint32 SecondPause = 7;
    constexpr uint32 SecondResume = 11;
    constexpr uint32 ResumeAt = 18;
    const uint32 stealAt = pausedSteal ? 9 : 13;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(out.capture.TryResize(static_cast<usize>(CaptureFrames) * PumpFrames * 2));
    fixture.audio->Device()->NullOutput()->CaptureTo(
        Span<float>(out.capture.Data(), out.capture.Size()));
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> ramp =
        MakeRampPcm(fixture.allocator, 1.0f / OneSecond, OneSecond);
    const SharedPtr<const SoundPcm> silent = MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
    EventLog log(fixture.allocator);
    const PlaybackHandle handle =
        PlayIn(fixture, log, scene,
               Playable(fixture.allocator, ramp, true, SoundVirtualizationMode::SeekRestart));
    Array<PlaybackHandle> fillers(fixture.allocator);
    for (uint32 frame = 0; frame < CaptureFrames; ++frame) {
        if (frame == FirstPause || frame == SecondPause) {
            REQUIRE(fixture.audio->SetScenePaused(scene, true));
        }

        if (frame == FirstResume || frame == SecondResume) {
            REQUIRE(fixture.audio->SetScenePaused(scene, false));
        }

        if (frame == stealAt) {
            // 엔진 scene의 채우는 재생이다. 기준 실행은 W가 L보다 낮다(0.5 × 0.5 < 1).
            const SoundSourceRef filler =
                Playable(fixture.allocator, silent, true, SoundVirtualizationMode::Restart,
                         steal ? FillerPriority : 0.5f);
            for (uint32 index = 0; index < VoiceLimit; ++index) {
                PlayOptions options;
                options.volume = FillerVolume;
                auto played = fixture.audio->Play2D(filler, Move(options));
                REQUIRE(played);
                REQUIRE(fillers.TryPushBack(played.Value()));
            }
        }

        if (frame == ResumeAt) {
            for (const PlaybackHandle filler : fillers) {
                REQUIRE(fixture.audio->Stop(filler, 0));
            }

            out.resumeMixFrame = fixture.audio->Device()->Mixer().CompletedMixFrame();
        }

        REQUIRE(fixture.Frame());
    }

    out.started = CountFor(log, handle, PlaybackEventKind::Started);
}

bool SameFrom(const Array<float>& left, const Array<float>& right, uint64 from) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = static_cast<usize>(from) * 2; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

} // namespace

// 논리 커서는 voice가 읽은 frame만 센다(일시정지 fade 동안 더 읽은 64 frame 포함): 일시정지를 두 번
// 한 뒤(또는 일시정지 중에) 빼앗겼다 되찾은 SeekRestart loop가 끊기지 않은 기준 출력과 bit까지
// 같다.
TEST(EngineAudio_PauseCursorMatchesMixer) {
    SystemAllocator allocator;
    for (bool pausedSteal : {false, true}) {
        PauseResumeRun stolen(allocator);
        PauseResumeRun reference(allocator);
        RunPauseResume(true, pausedSteal, stolen);
        RunPauseResume(false, pausedSteal, reference);
        REQUIRE(stolen.resumeMixFrame == reference.resumeMixFrame);
        CHECK(stolen.started == 1);
        CHECK(SameFrom(stolen.capture, reference.capture,
                       stolen.resumeMixFrame + ResumeSettleFrames));
        CHECK(!SameFrom(stolen.capture, reference.capture, 0));
    }
}

} // namespace audio_pause_tests_file
