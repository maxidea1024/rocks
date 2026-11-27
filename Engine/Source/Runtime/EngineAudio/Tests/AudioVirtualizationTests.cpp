#include "AudioEngineTestSupport.h"

#include <Base/InitializerList.h>
#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <EngineAudio/AudioEngineSettings.h>
#include <EngineAudio/AudioSpatialization.h>
#include <TestSupport/Test.h>

// AU7c 가상화 시험이다(Docs/Design/AudioSystem 3.41.3절, UE FAudioVirtualLoop): 가청 거리 밖 loop는
// voice 없이 가상으로 시작하고 들리면 실현된다(Started 한 번), 리스너가 크게 움직이면 바로
// 검사한다, 검사 간격 공식, Restart·SeekRestart 실현 위치, 동시 발음 거부·빼앗김 → 가상 → 실현,
// PlayWhenSilent, one-shot 비대상, 가상 재생의 Stop·scene 해제, 경계 왕복 churn 상한, 무작위 명령
// 불변식, steal fade 분리 voice, 실현 거부 동안 가상 유지, audio.virtual.* CVar. Inline 제어 +
// Manual 렌더 + Null Manual로 프레임·Pump로만 순서를 만든다.
using namespace rocks;
namespace audio_virtualization_tests_file {

using namespace rocks::engine_audio_test;

namespace {

constexpr uint32 OneSecond = TestRate;
constexpr uint32 StartDelivery = 3;
// 기본 감쇠(구, inner 400 + falloff 3600)의 최대 가청 거리다(cm).
constexpr double AudibleEdge = 4000.0;
constexpr uint32 CaptureFrames = 48;
// 복귀 fade-in이 끝나는 데 걸리는 상한(mix frame — 묶음 2개 + 믹서 ramp)이다.
constexpr uint64 ResumeSettleFrames = uint64{PumpFrames} * 2 + audio::DefaultParamRampFrames;

SoundSourceRef Loop(Allocator& allocator, const SharedPtr<const SoundPcm>& pcm,
                    SoundVirtualizationMode mode = SoundVirtualizationMode::Restart,
                    std::initializer_list<SoundConcurrencyEntry> entries = {},
                    bool looping = true) noexcept {
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.looping = looping;
    playable.virtualizationMode = mode;
    SoundAttenuation attenuation;
    // 감쇠 1인 넓은 내부 반경(출력 비교가 쉽게) — 최대 가청 거리는 inner + falloff다.
    attenuation.spatialize = false;
    attenuation.shapeExtents.x = 400.0f;
    playable.attenuation = attenuation;
    for (const SoundConcurrencyEntry& entry : entries) {
        playable.concurrency[playable.concurrencyCount++] = entry;
    }

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

PlaybackHandle PlayAt(AudioEngine& fixture, EventLog& log, AudioSceneId scene,
                      const SoundSourceRef& sound, double x, PlayOptions options = {}) noexcept {
    options.onEvent = log.Callback(fixture.allocator);
    auto played = fixture.audio->PlayAtInScene(scene, sound, Vec3d(x, 0.0, 0.0), Move(options));
    return played ? played.Value() : PlaybackHandle{};
}

void RequireIdle(AudioEngine& fixture) noexcept {
    (void)fixture.RunUntilIdle();
    const AudioControlStats stats = fixture.audio->ControlStats();
    CHECK(stats.activeVoices == 0);
    CHECK(stats.stoppingVoices == 0);
    CHECK(stats.voicelessPlaybacks == 0);
    CHECK(stats.virtualPlaybacks == 0);
    CHECK(stats.releasingVoices == 0);
    CHECK(stats.registeredClips == 0);
    REQUIRE(fixture.Frame());
}

SoundConcurrencyEntry Entry(uint64 key, SoundConcurrencyRule rule, uint32 maxCount) noexcept {
    SoundConcurrencyEntry entry;
    entry.key = key;
    entry.settings.resolutionRule = rule;
    entry.settings.maxCount = maxCount;
    return entry;
}

// 가상으로 시작한 loop를 리스너가 다가가 실현한다. jump면 한 번에(강제 검사), 아니면 조금씩.
struct RealizeRun {
    explicit RealizeRun(Allocator& allocator) noexcept : capture(allocator) {
    }

    Array<float> capture;
    uint64 realizeFrame = 0;
    uint32 started = 0;
};

void RunRealize(SoundVirtualizationMode mode, bool startNear, bool freshAtRealize,
                RealizeRun& out) noexcept {
    constexpr uint32 MoveAt = 6;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    REQUIRE(out.capture.TryResize(static_cast<usize>(CaptureFrames) * PumpFrames * 2));
    fixture.audio->Device()->NullOutput()->CaptureTo(
        Span<float>(out.capture.Data(), out.capture.Size()));
    const SharedPtr<const SoundPcm> ramp =
        MakeRampPcm(fixture.allocator, 1.0f / OneSecond, OneSecond);
    const SoundSourceRef sound = Loop(fixture.allocator, ramp, mode);
    EventLog log(fixture.allocator);
    // 소리는 원점, 리스너는 멀리(가청 밖) 또는 가까이.
    fixture.audio->SetSceneListener(scene, Vec3d(startNear ? 100.0 : 20000.0, 0.0, 0.0),
                                    Quatd::Identity());
    PlaybackHandle handle =
        freshAtRealize ? PlaybackHandle{} : PlayAt(fixture, log, scene, sound, 0.0);
    for (uint32 frame = 0; frame < CaptureFrames; ++frame) {
        if (frame == MoveAt) {
            fixture.audio->SetSceneListener(scene, Vec3d(100.0, 0.0, 0.0), Quatd::Identity());
            out.realizeFrame = fixture.audio->Device()->Mixer().CompletedMixFrame();
            if (freshAtRealize) {
                handle = PlayAt(fixture, log, scene, sound, 0.0);
            }
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

// 가청 거리 밖 loop는 voice·clip 없이 가상으로 시작한다(Started 없음). 리스너가
// forceUpdateDistance보다 크게 움직이면 그 묶음에 검사해 실현되고 Started가 한 번 나간다.
TEST(EngineAudio_VirtualStartsAndRealizesOnListenerJump) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    EventLog log(fixture.allocator);
    const PlaybackHandle far =
        PlayAt(fixture, log, scene, Loop(fixture.allocator, pcm), AudibleEdge + 50000.0);
    REQUIRE(fixture.Frames(StartDelivery));
    AudioControlStats stats = fixture.audio->ControlStats();
    CHECK(stats.virtualPlaybacks == 1 && stats.virtualizations == 1);
    CHECK(stats.activeVoices == 0 && stats.registeredClips == 0);
    CHECK(CountFor(log, far, PlaybackEventKind::Started) == 0);
    fixture.audio->SetSceneListener(scene, Vec3d(AudibleEdge + 49900.0, 0.0, 0.0),
                                    Quatd::Identity());
    REQUIRE(fixture.Frames(StartDelivery));
    stats = fixture.audio->ControlStats();
    CHECK(stats.virtualPlaybacks == 0 && stats.realizations == 1);
    CHECK(CountFor(log, far, PlaybackEventKind::Started) == 1);
    // 다시 멀어지면(조금씩이 아니라 바로 — 살아 있는 loop는 묶음마다 본다) 가상이 된다.
    fixture.audio->SetSceneListener(scene, Vec3d(), Quatd::Identity());
    REQUIRE(fixture.Frames(2));
    CHECK(fixture.audio->ControlStats().virtualPlaybacks == 1);
    CHECK(CountFor(log, far, PlaybackEventKind::Started) == 1);
    REQUIRE(fixture.audio->Stop(far, 0));
    REQUIRE(fixture.Frames(2));
    CHECK(EndOf(log, far) == PlaybackEndReason::Stopped);
    RequireIdle(fixture);
}

// 검사 간격: lerp(0.1 s, 3.0 s, clamp((거리 − 최대 거리) ÷ 15000)). 최대 거리 + 7500에서 가상이 된
// loop는 리스너가 조금씩(강제 검사 거리 안) 다가와도 약 1.55 s 뒤 검사에서야 실현된다.
TEST(EngineAudio_VirtualCheckInterval) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    EventLog log(fixture.allocator);
    const double start = AudibleEdge + 7500.0;
    const uint64 begin = fixture.audio->Device()->Mixer().CompletedMixFrame();
    const PlaybackHandle loop = PlayAt(fixture, log, scene, Loop(fixture.allocator, pcm), start);
    REQUIRE(fixture.Frame());
    REQUIRE(fixture.audio->ControlStats().virtualPlaybacks == 1);
    // 묶음마다 2000 cm(< 2500)씩 다가가 4묶음 뒤 가청 안.
    double position = start;
    for (uint32 step = 0; step < 4; ++step) {
        position -= 2000.0;
        fixture.audio->SetSceneListener(scene, Vec3d(start - position, 0.0, 0.0),
                                        Quatd::Identity());
        REQUIRE(fixture.Frame());
    }

    uint64 realized = 0;
    for (uint32 frame = 0; frame < 2 * OneSecond / PumpFrames && realized == 0; ++frame) {
        if (fixture.audio->ControlStats().realizations != 0) {
            realized = fixture.audio->Device()->Mixer().CompletedMixFrame();
            break;
        }

        REQUIRE(fixture.Frame());
    }

    REQUIRE(realized != 0);
    const double expected = (0.1 + (3.0 - 0.1) * 0.5) * OneSecond;
    CHECK(static_cast<double>(realized - begin) >= expected - PumpFrames * 2);
    CHECK(static_cast<double>(realized - begin) <= expected + PumpFrames * 3);
    REQUIRE(fixture.audio->Stop(loop, 0));
    RequireIdle(fixture);
}

// 실현 위치: SeekRestart는 논리 커서에서 이어져 fade-in 뒤 가상이 되지 않은 기준 출력과 bit까지
// 같다. Restart는 startFrame부터라 같은 때 새로 시작한 재생과 같다. Started는 한 번이다.
TEST(EngineAudio_VirtualRealizePosition) {
    SystemAllocator allocator;
    RealizeRun seek(allocator);
    RealizeRun steady(allocator);
    RunRealize(SoundVirtualizationMode::SeekRestart, false, false, seek);
    RunRealize(SoundVirtualizationMode::SeekRestart, true, false, steady);
    REQUIRE(seek.realizeFrame == steady.realizeFrame);
    CHECK(seek.started == 1 && steady.started == 1);
    CHECK(SameFrom(seek.capture, steady.capture, seek.realizeFrame + ResumeSettleFrames));
    CHECK(!SameFrom(seek.capture, steady.capture, 0));

    RealizeRun restart(allocator);
    RealizeRun fresh(allocator);
    RunRealize(SoundVirtualizationMode::Restart, false, false, restart);
    RunRealize(SoundVirtualizationMode::Restart, true, true, fresh);
    REQUIRE(restart.realizeFrame == fresh.realizeFrame);
    CHECK(restart.started == 1);
    CHECK(SameFrom(restart.capture, fresh.capture, restart.realizeFrame));
}

// 동시 발음: 거부된 가상화 가능 loop는 끝나지 않고 가상이 됐다가 자리가 나면 실현된다. 빼앗긴
// loop도 가상이 된다(fade는 떼어 낸 voice가 마친다). Disabled loop는 거부·빼앗김에 끝난다.
TEST(EngineAudio_VirtualFromConcurrency) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    EventLog log(fixture.allocator);
    const SoundConcurrencyEntry prevent = Entry(201, SoundConcurrencyRule::PreventNew, 1);
    const PlaybackHandle first =
        PlayAt(fixture, log, scene,
               Loop(fixture.allocator, pcm, SoundVirtualizationMode::Restart, {prevent}), 100.0);
    const PlaybackHandle waiting =
        PlayAt(fixture, log, scene,
               Loop(fixture.allocator, pcm, SoundVirtualizationMode::Restart, {prevent}), 100.0);
    const PlaybackHandle disabled =
        PlayAt(fixture, log, scene,
               Loop(fixture.allocator, pcm, SoundVirtualizationMode::Disabled, {prevent}), 100.0);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, waiting) == PlaybackEndReason::None);
    CHECK(CountFor(log, waiting, PlaybackEventKind::Started) == 0);
    CHECK(EndOf(log, disabled) == PlaybackEndReason::Concurrency);
    CHECK(fixture.audio->ControlStats().virtualPlaybacks == 1);
    // 들리지만 그룹이 막는 동안(검사 간격 0.1 s를 여러 번 넘겨도) 실현을 다시 시도하되 가상으로
    // 남는다(끝나지 않는다 — UE AD 5410–5432).
    REQUIRE(fixture.Frames(OneSecond / PumpFrames / 2));
    CHECK(EndOf(log, waiting) == PlaybackEndReason::None);
    CHECK(CountFor(log, waiting, PlaybackEventKind::Started) == 0);
    CHECK(fixture.audio->ControlStats().virtualPlaybacks == 1);
    CHECK(fixture.audio->ControlStats().realizations == 0);
    REQUIRE(fixture.audio->Stop(first, 0));
    // 가상 검사 간격(가청 안 — 0.1 s) 안에 실현된다.
    for (uint32 frame = 0;
         frame < MaxTestFrames && CountFor(log, waiting, PlaybackEventKind::Started) == 0;
         ++frame) {
        REQUIRE(fixture.Frame());
    }

    CHECK(CountFor(log, waiting, PlaybackEventKind::Started) == 1);
    CHECK(fixture.audio->ControlStats().realizations == 1);

    // 빼앗김 → 가상: StopOldest 그룹의 오래된 loop가 가상이 된다(Ended 없음).
    const SoundConcurrencyEntry oldest = Entry(202, SoundConcurrencyRule::StopOldest, 1);
    const PlaybackHandle old =
        PlayAt(fixture, log, scene,
               Loop(fixture.allocator, pcm, SoundVirtualizationMode::Restart, {oldest}), 100.0);
    REQUIRE(fixture.Frames(StartDelivery));
    const PlaybackHandle fresh =
        PlayAt(fixture, log, scene,
               Loop(fixture.allocator, pcm, SoundVirtualizationMode::Restart, {oldest}), 100.0);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, old) == PlaybackEndReason::None);
    CHECK(fixture.audio->ControlStats().virtualPlaybacks == 1);
    CHECK(CountFor(log, fresh, PlaybackEventKind::Started) == 1);
    for (const PlaybackHandle handle : {waiting, old, fresh}) {
        REQUIRE(fixture.audio->Stop(handle, 0));
    }

    REQUIRE(fixture.Frames(2));
    CHECK(EndOf(log, old) == PlaybackEndReason::Stopped);
    RequireIdle(fixture);
}

// PlayWhenSilent는 거리로 가상이 되지 않고(무음 컬링도 예외) voice를 쥔다. one-shot은 가상화하지
// 않는다(거리 밖 긴 one-shot은 무음 컬링으로 Inaudible).
TEST(EngineAudio_VirtualPlayWhenSilentAndOneShots) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond * 2);
    EventLog log(fixture.allocator);
    const PlaybackHandle silent = PlayAt(
        fixture, log, scene, Loop(fixture.allocator, pcm, SoundVirtualizationMode::PlayWhenSilent),
        AudibleEdge + 50000.0);
    const PlaybackHandle shot =
        PlayAt(fixture, log, scene,
               Loop(fixture.allocator, pcm, SoundVirtualizationMode::Restart, {}, false),
               AudibleEdge + 50000.0);
    REQUIRE(fixture.Frames(StartDelivery));
    const AudioControlStats stats = fixture.audio->ControlStats();
    CHECK(stats.virtualPlaybacks == 0);
    CHECK(stats.activeVoices == 1);
    CHECK(CountFor(log, silent, PlaybackEventKind::Started) == 1);
    CHECK(EndOf(log, shot) == PlaybackEndReason::Inaudible);
    REQUIRE(fixture.audio->Stop(silent, 0));
    RequireIdle(fixture);
}

// 가상 재생의 해제: scene 해제는 콜백 없이 Finished, Stop은 시작한 적이 없으면 끝 frame 없이
// Ended(Stopped)다.
TEST(EngineAudio_VirtualPlaybackRemoval) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    EventLog log(fixture.allocator);
    const PlaybackHandle a =
        PlayAt(fixture, log, scene, Loop(fixture.allocator, pcm), AudibleEdge + 50000.0);
    const PlaybackHandle b =
        PlayAt(fixture, log, scene, Loop(fixture.allocator, pcm), AudibleEdge + 50000.0);
    REQUIRE(fixture.Frames(2));
    REQUIRE(fixture.audio->ControlStats().virtualPlaybacks == 2);
    REQUIRE(fixture.audio->Stop(a, 0));
    REQUIRE(fixture.Frames(2));
    CHECK(EndOf(log, a) == PlaybackEndReason::Stopped);
    for (const PlaybackEvent& event : log.events) {
        if (event.handle == a) {
            CHECK(event.mixFrame == audio::UnsetMixFrame);
        }
    }

    fixture.audio->UnregisterScene(scene);
    CHECK(fixture.audio->Status(b) == PlaybackStatus::Finished);
    REQUIRE(fixture.Frames(2));
    CHECK(CountFor(log, b, PlaybackEventKind::Ended) == 0);
    RequireIdle(fixture);
}

// "100개 환경 루프 경계 왕복": 리스너가 경계를 묶음마다 조금씩(강제 검사 거리 안) 넘나들어도, 가상
// 재생은 검사 간격(≥ 0.1 s)마다만 실현되므로 실현 수가 간격 규칙 상한 안이다.
TEST(EngineAudio_VirtualBoundaryChurnBounded) {
    constexpr uint32 Loops = 100;
    constexpr uint32 Frames = 60;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.001f, OneSecond);
    EventLog log(fixture.allocator);
    Array<PlaybackHandle> handles(fixture.allocator);
    for (uint32 index = 0; index < Loops; ++index) {
        REQUIRE(handles.TryPushBack(
            PlayAt(fixture, log, scene, Loop(fixture.allocator, pcm), AudibleEdge)));
    }

    for (uint32 frame = 0; frame < Frames; ++frame) {
        const double x = (frame % 2 == 0) ? 10.0 : -10.0;
        fixture.audio->SetSceneListener(scene, Vec3d(x, 0.0, 0.0), Quatd::Identity());
        REQUIRE(fixture.Frame());
    }

    // 묶음 60개 = 30720 frame ≈ 0.64 s: loop마다 실현은 간격(0.1 s)마다 하나 + 처음.
    const uint64 perLoop = static_cast<uint64>(Frames) * PumpFrames / (OneSecond / 10) + 1;
    CHECK(fixture.audio->ControlStats().realizations <= Loops * perLoop);
    for (const PlaybackHandle handle : handles) {
        CHECK(CountFor(log, handle, PlaybackEventKind::Started) <= 1);
        REQUIRE(fixture.audio->Stop(handle, 0));
    }

    RequireIdle(fixture);
    for (const PlaybackHandle handle : handles) {
        CHECK(CountFor(log, handle, PlaybackEventKind::Ended) == 1);
    }
}

// steal fade 분리 voice: release 0.1 s 그룹에서 빼앗긴 가상화 가능 loop는 가상이 되고(Ended 없음),
// 떼어 낸 voice가 출력을 줄이다가 fade 끝 뒤 멈춘다. 새 재생은 무음 PCM이라 출력은 분리
// voice뿐이다. 두 번째 판은 fade 중 scene 해제가 분리 voice도 짧게 끊는다.
TEST(EngineAudio_VirtualStealFadeOnDetachedVoice) {
    constexpr float Release = 0.1f;
    constexpr uint32 FadeBatches = static_cast<uint32>(Release * OneSecond) / PumpFrames + 1;
    for (const bool removeScene : {false, true}) {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        const AudioSceneId scene = fixture.audio->RegisterScene();
        const SharedPtr<const SoundPcm> loud = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
        const SharedPtr<const SoundPcm> silent =
            MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
        SoundConcurrencyEntry oldest = Entry(401, SoundConcurrencyRule::StopOldest, 1);
        oldest.settings.voiceStealReleaseTime = Release;
        EventLog log(fixture.allocator);
        const PlaybackHandle old = PlayAt(
            fixture, log, scene,
            Loop(fixture.allocator, loud, SoundVirtualizationMode::Restart, {oldest}), 100.0);
        REQUIRE(fixture.Frames(StartDelivery * 2));
        REQUIRE(CountFor(log, old, PlaybackEventKind::Started) == 1);
        const PlaybackHandle fresh = PlayAt(
            fixture, log, scene,
            Loop(fixture.allocator, silent, SoundVirtualizationMode::Restart, {oldest}), 100.0);
        REQUIRE(fixture.Frame());
        CHECK(fixture.audio->ControlStats().virtualPlaybacks == 1);
        CHECK(EndOf(log, old) == PlaybackEndReason::None);
        Array<float> chunk(fixture.allocator);
        REQUIRE(chunk.TryResize(static_cast<usize>(PumpFrames) * 2));
        float previous = CapturePeak(fixture, chunk).left;
        CHECK(previous > 0.001f);
        if (removeScene) {
            fixture.audio->UnregisterScene(scene);
            REQUIRE(fixture.Frames(2));
            CHECK(CapturePeak(fixture, chunk).left == 0.0f);
            CHECK(CountFor(log, old, PlaybackEventKind::Ended) == 0);
            RequireIdle(fixture);
            continue;
        }

        // fade 동안 줄어들기만 하고, 끝(+ 믹서 정지 fade) 뒤에는 0이다.
        for (uint32 batch = 1; batch < FadeBatches; ++batch) {
            const float peak = CapturePeak(fixture, chunk).left;
            CHECK(peak <= previous);
            previous = peak;
        }

        REQUIRE(fixture.Frames(3));
        CHECK(CapturePeak(fixture, chunk).left == 0.0f);
        CHECK(fixture.audio->ControlStats().virtualPlaybacks == 1);
        CHECK(EndOf(log, old) == PlaybackEndReason::None);
        for (const PlaybackHandle handle : {old, fresh}) {
            REQUIRE(fixture.audio->Stop(handle, 0));
        }

        RequireIdle(fixture);
        CHECK(EndOf(log, old) == PlaybackEndReason::Stopped);
    }
}

// audio.virtual.allowplaywhensilent = 0이면 PlayWhenSilent loop도 거리 밖에서 가상이
// 된다(Restart처럼). 짧은 one-shot 판정은 허용값과 상관없이 사운드 모드만 보므로 remainActive
// one-shot은 끝나지 않고 voice 없이 남는다(UE AD 6356). 실행 중 audio.virtual.enabled = 0이면 남은
// 가상 재생을 바로 다시 수락한다(UE UpdateVirtualLoops disabled 분기).
TEST(EngineAudio_VirtualSettingsCVars) {
    test::config::ConfigFixture settings(test::config::DevelopmentRules());
    config::ConfigContext& context = settings.Context();
    REQUIRE(RegisterEngineAudioSettings(context));
    REQUIRE(context.AddAssignment(config::ConfigLayer::Session, "test",
                                  "audio.virtual.allowplaywhensilent", "0"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    AudioEngine fixture(DeterministicOptions(true), &context);
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond / 2);
    EventLog log(fixture.allocator);
    const PlaybackHandle loop = PlayAt(
        fixture, log, scene, Loop(fixture.allocator, pcm, SoundVirtualizationMode::PlayWhenSilent),
        AudibleEdge + 50000.0);
    PlayOptions keep;
    keep.remainActiveIfDropped = true;
    const PlaybackHandle shot =
        PlayAt(fixture, log, scene,
               Loop(fixture.allocator, pcm, SoundVirtualizationMode::PlayWhenSilent, {}, false),
               AudibleEdge + 50000.0, Move(keep));
    REQUIRE(fixture.Frames(StartDelivery));
    AudioControlStats stats = fixture.audio->ControlStats();
    CHECK(stats.virtualPlaybacks == 1);
    CHECK(stats.voicelessPlaybacks == 1);
    CHECK(EndOf(log, shot) == PlaybackEndReason::None);
    CHECK(stats.inaudibleEnds == 0);

    REQUIRE(
        context.AddAssignment(config::ConfigLayer::Session, "test", "audio.virtual.enabled", "0"));
    REQUIRE(fixture.Frames(2));
    stats = fixture.audio->ControlStats();
    CHECK(stats.virtualPlaybacks == 0);
    CHECK(stats.realizations == 1);
    CHECK(EndOf(log, loop) == PlaybackEndReason::None);
    for (const PlaybackHandle handle : {loop, shot}) {
        REQUIRE(fixture.audio->Stop(handle, 0));
    }

    RequireIdle(fixture);
}

// 무작위 명령(고정 seed): Play(가깝고 먼 loop·one-shot, 그룹 있음·없음), Stop, SetVolume, 리스너
// 이동을 섞어도 재생마다 Ended 정확히 1회·Started 1회 이하이고, 상한에 세는 voice ≤ 32다.
TEST(EngineAudio_VoiceManagementFuzz) {
    constexpr uint32 Frames = 120;
    constexpr uint32 MaxLive = 200;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.001f, OneSecond / 4);
    const SoundConcurrencyEntry group = Entry(301, SoundConcurrencyRule::StopOldest, 6);
    const SoundConcurrencyEntry quiet = Entry(302, SoundConcurrencyRule::StopQuietest, 4);
    const SoundSourceRef sounds[] = {
        Loop(fixture.allocator, pcm),
        Loop(fixture.allocator, pcm, SoundVirtualizationMode::SeekRestart, {group}),
        Loop(fixture.allocator, pcm, SoundVirtualizationMode::Disabled, {quiet}),
        Loop(fixture.allocator, pcm, SoundVirtualizationMode::Restart, {}, false),
        Loop(fixture.allocator, pcm, SoundVirtualizationMode::Restart, {group}, false),
    };
    EventLog log(fixture.allocator);
    Array<PlaybackHandle> handles(fixture.allocator);
    uint64 state = 0x9E3779B97F4A7C15ull;
    const auto next = [&](uint32 bound) noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<uint32>(state % bound);
    };

    for (uint32 frame = 0; frame < Frames; ++frame) {
        for (uint32 step = 0; step < 6; ++step) {
            const uint32 action = next(10);
            if (action < 5 && handles.Size() < MaxLive) {
                PlayOptions options;
                options.volume = next(4) == 0 ? 0.0f : 1.0f;
                options.priority = static_cast<float>(next(5));
                const double x = next(2) == 0 ? 100.0 : AudibleEdge + 3000.0;
                const PlaybackHandle handle =
                    PlayAt(fixture, log, scene, sounds[next(5)], x, Move(options));
                if (handle.generation != 0) {
                    REQUIRE(handles.TryPushBack(handle));
                }
            } else if (action < 7 && !handles.IsEmpty()) {
                (void)fixture.audio->Stop(handles[next(static_cast<uint32>(handles.Size()))],
                                          next(2) * 256);
            } else if (action < 8 && !handles.IsEmpty()) {
                (void)fixture.audio->SetParameter(
                    handles[next(static_cast<uint32>(handles.Size()))], PlaybackParameter::Volume,
                    next(2) == 0 ? 0.0f : 1.0f, 0);
            } else {
                const double x = static_cast<double>(next(3)) * 3000.0;
                fixture.audio->SetSceneListener(scene, Vec3d(x, 0.0, 0.0), Quatd::Identity());
            }
        }

        REQUIRE(fixture.Frame());
        CHECK(fixture.audio->ControlStats().activeVoices <= 32);
    }

    for (const PlaybackHandle handle : handles) {
        (void)fixture.audio->Stop(handle, 0);
    }

    RequireIdle(fixture);
    for (const PlaybackHandle handle : handles) {
        CHECK(CountFor(log, handle, PlaybackEventKind::Ended) == 1);
        CHECK(CountFor(log, handle, PlaybackEventKind::Started) <= 1);
    }
}

} // namespace audio_virtualization_tests_file
