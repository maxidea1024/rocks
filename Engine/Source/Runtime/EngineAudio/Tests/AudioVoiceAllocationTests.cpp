#include "AudioEngineTestSupport.h"

#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <EngineAudio/AudioSpatialization.h>
#include <TestSupport/Test.h>

// AU7a voice 배정 시험이다(Docs/Design/AudioSystem 3.41.1절): 볼륨 가중 우선순위로 상한까지 voice를
// 주고(동률은 먼저 수락한 쪽), 상한 밖 one-shot은 VoiceLimit·loop는 voice 없이 남았다가 되찾는다
// (Restart는 startFrame부터, SeekRestart는 논리 커서에서), 무음 컬링·짧은 소리 거리 판정, Deferred
// 재생 빼앗기, 여유 부족 끊기, voice 없는 재생의 Stop. 모두 Inline 제어 + Manual 렌더 + Null
// Manual로 프레임·Pump로만 순서를 만든다(시간 대기 없음). 설정 문맥이 없으면 audio.voices.max는
// 32다.
using namespace rocks;
namespace audio_voice_allocation_tests_file {

using namespace rocks::engine_audio_test;

namespace {

constexpr uint32 VoiceLimit = 32;
// 상한을 채우는 재생의 우선순위·volume이다(W = 0.5 × 10 = 5 — 기본 재생 W 1보다 높다).
constexpr float FillerPriority = 10.0f;
constexpr float FillerVolume = 0.5f;
// 1 s 원본이다(TestRate).
constexpr uint32 OneSecond = TestRate;
// voice를 되찾을 때의 fade-in 길이(제어 쪽 ResumeFadeFrames = DefaultParamRampFrames — 3.41.1절)다.
constexpr uint32 ResumeFade = audio::DefaultParamRampFrames;
// 복귀 fade-in이 끝나는 데 걸리는 상한(mix frame)이다: 제어 쪽 ramp는 묶음마다(PumpFrames) 평가되고
// 그 값을 믹서가 DefaultParamRampFrames 동안 잇는다.
constexpr uint64 ResumeSettleFrames = uint64{PumpFrames} * 2 + ResumeFade;
// 출력 비교용 캡처 길이(프레임 묶음 수)다.
constexpr uint32 CaptureFrames = 48;
// Play에서 Started가 게임에 닿기까지의 프레임 수다: 수락·voice 배정 → 렌더 → 다음 묶음 수거 →
// 그다음 프레임 전달.
constexpr uint32 StartDelivery = 3;

SoundSourceRef MakePlayable(Allocator& allocator, const SharedPtr<const SoundPcm>& pcm,
                            bool looping, float priority = DefaultSoundPriority,
                            SoundVirtualizationMode mode = SoundVirtualizationMode::Restart,
                            bool bypassVolume = false,
                            const Optional<SoundAttenuation>& attenuation = {}) noexcept {
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.looping = looping;
    playable.priority = priority;
    playable.virtualizationMode = mode;
    playable.bypassVolumeScaleForPriority = bypassVolume;
    playable.attenuation = attenuation;
    auto made = SoundSourceRef::TryCreate(allocator, playable);
    return made ? made.Value() : SoundSourceRef{};
}

// handle의 사건을 센다.
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

PlaybackHandle PlayLogged(AudioEngine& fixture, EventLog& log, const SoundSourceRef& sound,
                          PlayOptions options = {}) noexcept {
    options.onEvent = log.Callback(fixture.allocator);
    auto played = fixture.audio->Play2D(sound, Move(options));
    return played ? played.Value() : PlaybackHandle{};
}

// 상한을 채우는 무음 one-shot(값 0 — 출력에 섞이지 않는다)들이다.
void PlayFillers(AudioEngine& fixture, EventLog& log, const SharedPtr<const SoundPcm>& silent,
                 float priority, Array<PlaybackHandle>& out) noexcept {
    const SoundSourceRef filler = MakePlayable(fixture.allocator, silent, true, priority);
    for (uint32 index = 0; index < VoiceLimit; ++index) {
        PlayOptions options;
        options.volume = FillerVolume;
        const PlaybackHandle handle = PlayLogged(fixture, log, filler, Move(options));
        REQUIRE(handle.generation != 0);
        REQUIRE(out.TryPushBack(handle));
    }
}

// 고정 위치 재생이다(리스너는 원점).
PlaybackHandle PlayAtLogged(AudioEngine& fixture, EventLog& log, AudioSceneId scene,
                            const SoundSourceRef& sound, double x) noexcept {
    PlayOptions options;
    options.onEvent = log.Callback(fixture.allocator);
    auto played = fixture.audio->PlayAtInScene(scene, sound, Vec3d(x, 0.0, 0.0), Move(options));
    return played ? played.Value() : PlaybackHandle{};
}

// L(ramp loop)을 재생하다가 stealAt 프레임에 상한을 채우는 재생을 넣고 resumeAt 프레임에 멈춘다.
// steal이 false면 채우는 재생이 L보다 낮아 L은 끊기지 않는다(기준 출력).
struct ResumeRun {
    explicit ResumeRun(Allocator& allocator) noexcept : capture(allocator) {
    }

    Array<float> capture;
    uint64 resumeMixFrame = 0;
    uint32 started = 0;
    uint32 ended = 0;
};

void RunResume(SoundVirtualizationMode mode, bool steal, bool restartFresh, ResumeRun& out,
               float pitch = 1.0f) noexcept {
    constexpr uint32 StealAt = 4;
    constexpr uint32 ResumeAt = 10;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(out.capture.TryResize(static_cast<usize>(CaptureFrames) * PumpFrames * 2));
    fixture.audio->Device()->NullOutput()->CaptureTo(
        Span<float>(out.capture.Data(), out.capture.Size()));
    const SharedPtr<const SoundPcm> ramp =
        MakeRampPcm(fixture.allocator, 1.0f / OneSecond, OneSecond);
    const SharedPtr<const SoundPcm> silent = MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
    const SoundSourceRef loop =
        MakePlayable(fixture.allocator, ramp, true, DefaultSoundPriority, mode);
    EventLog log(fixture.allocator);
    PlayOptions first;
    first.pitch = pitch;
    PlaybackHandle handle =
        restartFresh ? PlaybackHandle{} : PlayLogged(fixture, log, loop, Move(first));
    Array<PlaybackHandle> fillers(fixture.allocator);
    for (uint32 frame = 0; frame < CaptureFrames; ++frame) {
        if (frame == StealAt) {
            // 기준 실행은 채우는 재생의 W가 L보다 낮다(0.5 × 0.5 < 1): 가장 낮은 채우기 하나가
            // 진다.
            PlayFillers(fixture, log, silent, steal ? FillerPriority : 0.5f, fillers);
        }

        if (frame == ResumeAt) {
            for (const PlaybackHandle filler : fillers) {
                REQUIRE(fixture.audio->Stop(filler, 0));
            }

            out.resumeMixFrame = fixture.audio->Device()->Mixer().CompletedMixFrame();
            if (restartFresh) {
                PlayOptions fresh;
                fresh.pitch = pitch;
                handle = PlayLogged(fixture, log, loop, Move(fresh));
            }
        }

        REQUIRE(fixture.Frame());
    }

    out.started = CountFor(log, handle, PlaybackEventKind::Started);
    out.ended = CountFor(log, handle, PlaybackEventKind::Ended);
}

// 제어 쪽이 모두 놓일 때까지 돌고, 남은 계수를 하나씩 단언한다(실패하면 무엇이 남았는지 보인다).
void RequireIdle(AudioEngine& fixture) noexcept {
    (void)fixture.RunUntilIdle();
    const AudioControlStats stats = fixture.audio->ControlStats();
    CHECK(stats.activeVoices == 0);
    CHECK(stats.stoppingVoices == 0);
    CHECK(stats.voicelessPlaybacks == 0);
    CHECK(stats.releasingVoices == 0);
    CHECK(stats.registeredClips == 0);
    CHECK(stats.pendingDeferred == 0);
    REQUIRE(fixture.Frame());
}

// 두 캡처가 mix frame [from, 끝)에서 같다(mix frame m은 캡처 2m — 두 실행이 같은 장치 설정).
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

// 우선순위가 높은 새 재생이 상한에서 W가 가장 낮은 재생(동률이면 나중에 수락한 쪽)을 빼앗는다.
// 빼앗긴 loop는 끝나지 않고 voice 없이 남았다가 자리가 나면 되찾는다(Started는 처음 한 번뿐).
// 빼앗긴 one-shot은 VoiceLimit이다.
TEST(EngineAudio_VoicePriorityStealsLowest) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.001f, OneSecond);
    // 마지막 loop만 큰 값이라 출력으로 그 loop가 voice를 잃었는지 본다(W는 PCM 값과 무관하다).
    const SharedPtr<const SoundPcm> marked = MakeConstantPcm(fixture.allocator, 0.4f, OneSecond);
    const SharedPtr<const SoundPcm> silent = MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
    const SoundSourceRef loop = MakePlayable(fixture.allocator, pcm, true);
    const SoundSourceRef markedLoop = MakePlayable(fixture.allocator, marked, true);
    const SoundSourceRef shot = MakePlayable(fixture.allocator, pcm, false);
    EventLog log(fixture.allocator);
    Array<PlaybackHandle> loops(fixture.allocator);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    for (uint32 index = 0; index < VoiceLimit - 1; ++index) {
        const PlaybackHandle handle =
            PlayLogged(fixture, log, index == VoiceLimit - 2 ? markedLoop : loop);
        REQUIRE(handle.generation != 0);
        REQUIRE(loops.TryPushBack(handle));
    }

    const PlaybackHandle victimShot = PlayLogged(fixture, log, shot);
    REQUIRE(fixture.Frames(2));
    REQUIRE(fixture.audio->ControlStats().activeVoices == VoiceLimit);
    // 높은 우선순위 one-shot 하나 → 같은 W(1) 중 가장 늦게 수락한 one-shot이 진다.
    PlayOptions high;
    high.priority = 50.0f;
    const PlaybackHandle winner =
        PlayLogged(fixture, log, MakePlayable(fixture.allocator, silent, false), Move(high));
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(CountFor(log, winner, PlaybackEventKind::Started) == 1);
    CHECK(EndOf(log, victimShot) == PlaybackEndReason::VoiceLimit);
    CHECK(fixture.audio->ControlStats().voiceSteals == 1);
    // loop 하나를 더 빼앗는다(마지막 loop가 진다 — 동률은 먼저 수락한 쪽이 남는다).
    PlayOptions higher;
    higher.priority = 60.0f;
    CHECK(CapturePeak(fixture, chunk).left > 0.15f);
    const PlaybackHandle second =
        PlayLogged(fixture, log, MakePlayable(fixture.allocator, silent, false), Move(higher));
    REQUIRE(fixture.Frame());
    // 표시한 loop(가장 늦게 수락한 loop)가 졌다: 그 값이 출력에서 사라진다.
    (void)CapturePeak(fixture, chunk);
    CHECK(CapturePeak(fixture, chunk).left < 0.1f);
    AudioControlStats stats = fixture.audio->ControlStats();
    CHECK(stats.voiceSteals == 2);
    CHECK(stats.voicelessPlaybacks == 1);
    CHECK(fixture.audio->Status(loops[VoiceLimit - 2]) == PlaybackStatus::Playing);
    // 승자 하나가 멈추면 빼앗긴 loop가 voice를 되찾는다(이벤트 없음).
    REQUIRE(fixture.audio->Stop(winner, 0));
    for (uint32 frame = 0;
         frame < MaxTestFrames && fixture.audio->ControlStats().voicelessPlaybacks != 0; ++frame) {
        REQUIRE(fixture.Frame());
    }

    stats = fixture.audio->ControlStats();
    CHECK(stats.voicelessPlaybacks == 0);
    CHECK(stats.activeVoices == VoiceLimit);
    (void)CapturePeak(fixture, chunk);
    CHECK(CapturePeak(fixture, chunk).left > 0.15f);
    CHECK(CountFor(log, loops[VoiceLimit - 2], PlaybackEventKind::Started) == 1);
    CHECK(CountFor(log, loops[VoiceLimit - 2], PlaybackEventKind::Ended) == 0);
    REQUIRE(fixture.audio->Stop(second, 0));
    for (const PlaybackHandle handle : loops) {
        REQUIRE(fixture.audio->Stop(handle, 0));
    }

    RequireIdle(fixture);
    for (const PlaybackHandle handle : loops) {
        CHECK(CountFor(log, handle, PlaybackEventKind::Ended) == 1);
        CHECK(EndOf(log, handle) == PlaybackEndReason::Stopped);
    }
}

// W = 들리는 크기 × 우선순위다: 같은 묶음에서 큰 소리 하나가 작은 소리 32개 중 마지막을 밀어낸다.
// bypassVolumeScaleForPriority면 크기를 곱하지 않아 동률이 되고 먼저 수락한 쪽이 이긴다.
TEST(EngineAudio_VoiceWeightUsesVolume) {
    for (bool bypass : {false, true}) {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
        const SoundSourceRef quiet =
            MakePlayable(fixture.allocator, pcm, false, DefaultSoundPriority,
                         SoundVirtualizationMode::Restart, bypass);
        const SoundSourceRef loud = MakePlayable(fixture.allocator, pcm, false);
        EventLog log(fixture.allocator);
        Array<PlaybackHandle> quiets(fixture.allocator);
        for (uint32 index = 0; index < VoiceLimit; ++index) {
            PlayOptions options;
            options.volume = 0.1f;
            REQUIRE(quiets.TryPushBack(PlayLogged(fixture, log, quiet, Move(options))));
        }

        const PlaybackHandle loudHandle = PlayLogged(fixture, log, loud);
        REQUIRE(fixture.Frames(StartDelivery));
        const PlaybackHandle loser = bypass ? loudHandle : quiets[VoiceLimit - 1];
        CHECK(EndOf(log, loser) == PlaybackEndReason::VoiceLimit);
        CHECK(CountFor(log, loser, PlaybackEventKind::Started) == 0);
        CHECK(CountFor(log, bypass ? quiets[VoiceLimit - 1] : loudHandle,
                       PlaybackEventKind::Started) == 1);
        CHECK(fixture.audio->ControlStats().voiceLimitRejects == 1);
    }
}

// 빼앗겼다 되찾은 loop의 위치: SeekRestart는 논리 커서에서 이어져 fade-in 뒤 끊기지 않은 기준
// 출력과 bit까지 같다. Restart는 startFrame부터라 같은 때 새로 시작한 재생과 같다. Started는 한
// 번이다.
TEST(EngineAudio_VoicelessLoopResumePosition) {
    SystemAllocator allocator;
    ResumeRun stolen(allocator);
    ResumeRun reference(allocator);
    RunResume(SoundVirtualizationMode::SeekRestart, true, false, stolen);
    RunResume(SoundVirtualizationMode::SeekRestart, false, false, reference);
    REQUIRE(stolen.resumeMixFrame == reference.resumeMixFrame);
    CHECK(stolen.started == 1 && stolen.ended == 0);
    CHECK(SameFrom(stolen.capture, reference.capture, stolen.resumeMixFrame + ResumeSettleFrames));
    CHECK(!SameFrom(stolen.capture, reference.capture, 0));

    // pitch 2배·0.5배: 논리 커서는 실효 pitch × rate 비로 진행한다(블록 경계에서 정수 위치).
    for (float pitch : {2.0f, 0.5f}) {
        ResumeRun fast(allocator);
        ResumeRun steady(allocator);
        RunResume(SoundVirtualizationMode::SeekRestart, true, false, fast, pitch);
        RunResume(SoundVirtualizationMode::SeekRestart, false, false, steady, pitch);
        CHECK(fast.started == 1);
        CHECK(SameFrom(fast.capture, steady.capture, fast.resumeMixFrame + ResumeSettleFrames));
    }

    ResumeRun restarted(allocator);
    ResumeRun fresh(allocator);
    RunResume(SoundVirtualizationMode::Restart, true, false, restarted);
    RunResume(SoundVirtualizationMode::Restart, true, true, fresh);
    REQUIRE(restarted.resumeMixFrame == fresh.resumeMixFrame);
    CHECK(restarted.started == 1);
    CHECK(SameFrom(restarted.capture, fresh.capture, restarted.resumeMixFrame));
}

// 무음 컬링: 들리는 크기 0인 one-shot은 Started 없이 Inaudible, loop는 voice 없이 남았다가 소리를
// 키우면 시작한다. fade-in(0에서 오르는 ramp)은 컬링하지 않는다. 재생 중 0으로 줄인 one-shot은
// Inaudible로, remainActiveIfDropped면 voice 없이 남아 끝까지 간 뒤 NaturalEnd다.
TEST(EngineAudio_SilentCulling) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond / 4);
    const SoundSourceRef shot = MakePlayable(fixture.allocator, pcm, false);
    const SoundSourceRef loop = MakePlayable(fixture.allocator, pcm, true);
    EventLog log(fixture.allocator);
    PlayOptions mute;
    mute.volume = 0.0f;
    const PlaybackHandle mutedShot = PlayLogged(fixture, log, shot, Move(mute));
    PlayOptions muteLoop;
    muteLoop.volume = 0.0f;
    const PlaybackHandle mutedLoop = PlayLogged(fixture, log, loop, Move(muteLoop));
    PlayOptions fade;
    fade.fadeInFrames = PumpFrames * 4;
    const PlaybackHandle fading = PlayLogged(fixture, log, shot, Move(fade));
    const PlaybackHandle loudShot = PlayLogged(fixture, log, shot);
    PlayOptions remain;
    remain.remainActiveIfDropped = true;
    const PlaybackHandle remaining = PlayLogged(fixture, log, shot, Move(remain));
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, mutedShot) == PlaybackEndReason::Inaudible);
    CHECK(CountFor(log, mutedShot, PlaybackEventKind::Started) == 0);
    CHECK(CountFor(log, mutedLoop, PlaybackEventKind::Ended) == 0);
    CHECK(fixture.audio->ControlStats().voicelessPlaybacks == 1);
    CHECK(CountFor(log, fading, PlaybackEventKind::Started) == 1);
    REQUIRE(CountFor(log, loudShot, PlaybackEventKind::Started) == 1);
    // 재생 중 0으로: 보통 one-shot은 Inaudible, remainActive는 voice 없이 남는다.
    REQUIRE(fixture.audio->SetParameter(loudShot, PlaybackParameter::Volume, 0.0f, 0));
    REQUIRE(fixture.audio->SetParameter(remaining, PlaybackParameter::Volume, 0.0f, 0));
    REQUIRE(fixture.audio->SetParameter(mutedLoop, PlaybackParameter::Volume, 1.0f, 0));
    REQUIRE(fixture.Frames(3));
    CHECK(EndOf(log, loudShot) == PlaybackEndReason::Inaudible);
    CHECK(CountFor(log, remaining, PlaybackEventKind::Ended) == 0);
    CHECK(CountFor(log, mutedLoop, PlaybackEventKind::Started) == 1);
    CHECK(fixture.audio->ControlStats().inaudibleEnds == 2);
    // 1/4 s 원본이 논리 커서로 끝까지 가면 NaturalEnd다(시작한 적이 있어 끝 frame이 있다).
    for (uint32 frame = 0;
         frame < MaxTestFrames && CountFor(log, remaining, PlaybackEventKind::Ended) == 0;
         ++frame) {
        REQUIRE(fixture.Frame());
    }

    CHECK(EndOf(log, remaining) == PlaybackEndReason::NaturalEnd);
    for (const PlaybackEvent& event : log.events) {
        if (event.handle == remaining && event.kind == PlaybackEventKind::Ended) {
            CHECK(event.mixFrame != audio::UnsetMixFrame);
        }
    }

    REQUIRE(fixture.audio->Stop(mutedLoop, 0));
    RequireIdle(fixture);
}

// 가청 거리 밖의 짧은(≤ 1 s) one-shot은 만들지 않고 Inaudible이다. 안이면 재생된다. voice 없는
// loop에 온 Stop은 바로 Ended(Stopped)이고 시작한 적이 없으면 끝 frame이 없다.
TEST(EngineAudio_ShortSoundRangeAndVoicelessStop) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    REQUIRE(scene.IsValid());
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    const SoundAttenuation attenuation;
    const SoundSourceRef shot = MakePlayable(fixture.allocator, pcm, false, DefaultSoundPriority,
                                             SoundVirtualizationMode::Restart, false, attenuation);
    const SoundSourceRef loop = MakePlayable(fixture.allocator, pcm, true, DefaultSoundPriority,
                                             SoundVirtualizationMode::Restart, false, attenuation);
    const double outside = static_cast<double>(MaxAudibleDistance(attenuation)) + 100.0;
    EventLog log(fixture.allocator);
    const PlaybackHandle far = PlayAtLogged(fixture, log, scene, shot, outside);
    const PlaybackHandle near = PlayAtLogged(fixture, log, scene, shot, 100.0);
    const PlaybackHandle farLoop = PlayAtLogged(fixture, log, scene, loop, outside);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, far) == PlaybackEndReason::Inaudible);
    CHECK(CountFor(log, far, PlaybackEventKind::Started) == 0);
    CHECK(CountFor(log, near, PlaybackEventKind::Started) == 1);
    // 거리 밖 loop는 끝나지 않고 가상으로 남는다(3.41.3절).
    CHECK(CountFor(log, farLoop, PlaybackEventKind::Ended) == 0);
    CHECK(fixture.audio->ControlStats().virtualPlaybacks == 1);
    REQUIRE(fixture.audio->Stop(farLoop, 0));
    REQUIRE(fixture.Frames(2));
    CHECK(EndOf(log, farLoop) == PlaybackEndReason::Stopped);
    REQUIRE(fixture.audio->Stop(near, 0));
    for (const PlaybackEvent& event : log.events) {
        if (event.handle == farLoop) {
            CHECK(event.mixFrame == audio::UnsetMixFrame);
        }
    }

    RequireIdle(fixture);
}

// 최대 가청 거리(UE GetMaxDimension): Linear·NaturalSound Continues(dB 비례)·Hold(world 최대)·
// distanceScale·감쇠 꺼짐.
TEST(EngineAudio_MaxAudibleDistance) {
    SoundAttenuation linear;
    CHECK(MaxAudibleDistance(linear) == 4000.0f);
    SoundAttenuation natural;
    natural.distance.model = audio::DistanceAttenuationModel::NaturalSound;
    CHECK(MaxAudibleDistance(natural) == 4000.0f);
    natural.distance.naturalSoundAttenuationDb = -30.0f;
    CHECK(MaxAudibleDistance(natural) == 7600.0f);
    natural.distance.naturalSoundFalloffMode = audio::NaturalSoundFalloffMode::Hold;
    CHECK(MaxAudibleDistance(natural) == MaxAudioWorldDistance);
    SoundAttenuation scaled;
    scaled.distance.distanceScale = 2.0f;
    CHECK(MaxAudibleDistance(scaled) == 2200.0f);
    scaled.distance.distanceScale = 0.0f;
    CHECK(MaxAudibleDistance(scaled) == MaxAudioWorldDistance);
    const Vec3d origin;
    CHECK(IsWithinAudibleRange(linear, origin, Vec3d(3999.0, 0.0, 0.0)));
    CHECK(!IsWithinAudibleRange(linear, origin, Vec3d(4000.0, 0.0, 0.0)));
    SoundAttenuation flat;
    flat.attenuate = false;
    CHECK(IsWithinAudibleRange(flat, origin, Vec3d(1.0e7, 0.0, 0.0)));
    CHECK(IsWithinAudibleRange(natural, origin, Vec3d(1.0e7, 0.0, 0.0)));
}

// 명령 링이 차서 Deferred인 재생이 상한 밖으로 밀리면 Reserved voice를 바로 놓고 Started가 나가지
// 않는다(loop는 voice 없이 남았다가 나중에 한 번만 Started).
TEST(EngineAudio_DeferredPlayDroppedWithoutStarted) {
    AudioEngineHostOptions options = DeterministicOptions(true);
    options.commandCapacity = 2;
    AudioEngine fixture(options);
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    const SharedPtr<const SoundPcm> shortPcm = MakeConstantPcm(fixture.allocator, 0.01f, 480);
    const SoundSourceRef loop = MakePlayable(fixture.allocator, pcm, true);
    EventLog log(fixture.allocator);
    Array<PlaybackHandle> loops(fixture.allocator);
    for (uint32 index = 0; index < 4; ++index) {
        REQUIRE(loops.TryPushBack(PlayLogged(fixture, log, loop)));
    }

    // 렌더 없이 묶음만 돌려 링을 비우지 않는다(Deferred가 남는다).
    REQUIRE(fixture.engine.RunFrame());
    REQUIRE(fixture.audio->ControlStats().pendingDeferred == 2);
    const SoundSourceRef high = MakePlayable(fixture.allocator, shortPcm, false, FillerPriority);
    for (uint32 index = 0; index < VoiceLimit; ++index) {
        REQUIRE(PlayLogged(fixture, log, high).generation != 0);
    }

    REQUIRE(fixture.engine.RunFrame());
    CHECK(fixture.audio->ControlStats().voicelessPlaybacks == 4);
    REQUIRE(fixture.Frame());
    for (const PlaybackHandle handle : loops) {
        CHECK(CountFor(log, handle, PlaybackEventKind::Started) == 0);
    }

    const auto allStarted = [&]() noexcept {
        for (const PlaybackHandle handle : loops) {
            if (CountFor(log, handle, PlaybackEventKind::Started) == 0) {
                return false;
            }
        }

        return true;
    };
    for (uint32 frame = 0; frame < MaxTestFrames && !allStarted(); ++frame) {
        REQUIRE(fixture.Frame());
    }

    REQUIRE(fixture.Frames(StartDelivery));
    for (const PlaybackHandle handle : loops) {
        CHECK(CountFor(log, handle, PlaybackEventKind::Started) == 1);
        REQUIRE(fixture.audio->Stop(handle, 0));
    }

    RequireIdle(fixture);
}

// 여유 부족: 긴 정지 fade 중인 voice 32개가 믹서 여유를 채우면, 자리를 받은 재생이 voice를 못 잡을
// 때 정지 중 voice 하나를 짧게 끊고(headroomCuts) loop는 다음 묶음에 시작한다.
TEST(EngineAudio_HeadroomCutsStoppingVoice) {
    constexpr uint32 LongFade = OneSecond * 10;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    const SoundSourceRef loop = MakePlayable(fixture.allocator, pcm, true);
    EventLog log(fixture.allocator);
    Array<PlaybackHandle> first(fixture.allocator);
    Array<PlaybackHandle> second(fixture.allocator);
    for (uint32 index = 0; index < VoiceLimit; ++index) {
        REQUIRE(first.TryPushBack(PlayLogged(fixture, log, loop)));
    }

    REQUIRE(fixture.Frames(2));
    for (const PlaybackHandle handle : first) {
        REQUIRE(fixture.audio->Stop(handle, LongFade));
    }

    for (uint32 index = 0; index < VoiceLimit; ++index) {
        REQUIRE(second.TryPushBack(PlayLogged(fixture, log, loop)));
    }

    REQUIRE(fixture.Frames(2));
    AudioControlStats stats = fixture.audio->ControlStats();
    REQUIRE(stats.activeVoices == VoiceLimit && stats.stoppingVoices == VoiceLimit);
    // 믹서 표(2 × 32)가 찼다. 높은 우선순위 loop는 자리를 받지만 voice가 없다 → 끊기·다음 묶음
    // 시작.
    PlayOptions high;
    high.priority = FillerPriority;
    const PlaybackHandle winner = PlayLogged(fixture, log, loop, Move(high));
    for (uint32 frame = 0;
         frame < MaxTestFrames && CountFor(log, winner, PlaybackEventKind::Started) == 0; ++frame) {
        REQUIRE(fixture.Frame());
    }

    stats = fixture.audio->ControlStats();
    CHECK(CountFor(log, winner, PlaybackEventKind::Started) == 1);
    CHECK(stats.headroomCuts >= 1);
    CHECK(stats.resumeFailures >= 1);
    REQUIRE(fixture.audio->Stop(winner, 0));
    for (const PlaybackHandle handle : second) {
        REQUIRE(fixture.audio->Stop(handle, 0));
    }

    for (const PlaybackHandle handle : first) {
        REQUIRE(fixture.audio->Stop(handle, 0));
    }

    RequireIdle(fixture);
}

// 이번 묶음에 끝난 voice의 자리는 같은 묶음의 새 재생이 받는다(수거를 배정보다 먼저 — 3.41.4절).
TEST(EngineAudio_EndedVoiceSlotReusedSameBatch) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> shortPcm = MakeConstantPcm(fixture.allocator, 0.01f, 480);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    EventLog log(fixture.allocator);
    const SoundSourceRef shortShot = MakePlayable(fixture.allocator, shortPcm, false);
    for (uint32 index = 0; index < VoiceLimit; ++index) {
        REQUIRE(PlayLogged(fixture, log, shortShot).generation != 0);
    }

    // 이 프레임의 렌더(512 frame)에서 480 frame one-shot 32개가 모두 끝난다.
    REQUIRE(fixture.Frame());
    const PlaybackHandle late =
        PlayLogged(fixture, log, MakePlayable(fixture.allocator, pcm, false));
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(CountFor(log, late, PlaybackEventKind::Started) == 1);
    CHECK(fixture.audio->ControlStats().voiceLimitRejects == 0);
    REQUIRE(fixture.audio->Stop(late, 0));
    RequireIdle(fixture);
}

// voice 없는 재생의 scene·emitter 해제: emitter는 Ended(EmitterRemoved), scene은 콜백 없이
// Finished다 (3.35.4절). SetVolume을 받은 voice 없는 loop는 시작한다.
TEST(EngineAudio_VoicelessPlaybackRemoval) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    REQUIRE(scene.IsValid());
    auto emitter = fixture.audio->RegisterEmitter(scene, Vec3d(1.0e6, 0.0, 0.0));
    REQUIRE(emitter);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, OneSecond);
    const SoundAttenuation attenuation;
    const SoundSourceRef loop = MakePlayable(fixture.allocator, pcm, true, DefaultSoundPriority,
                                             SoundVirtualizationMode::Restart, false, attenuation);
    EventLog log(fixture.allocator);
    PlayOptions attached;
    attached.onEvent = log.Callback(fixture.allocator);
    auto far = fixture.audio->PlayAttachedInScene(scene, emitter.Value(), loop, Move(attached));
    REQUIRE(far);
    const PlaybackHandle fixed = PlayAtLogged(fixture, log, scene, loop, 1.0e6);
    REQUIRE(fixture.Frames(StartDelivery));
    // 거리 밖 loop라 가상이다(voice 없음 — 3.41.3절).
    REQUIRE(fixture.audio->ControlStats().virtualPlaybacks == 2);
    fixture.audio->UnregisterEmitter(emitter.Value());
    REQUIRE(fixture.Frames(2));
    CHECK(EndOf(log, far.Value()) == PlaybackEndReason::EmitterRemoved);
    fixture.audio->UnregisterScene(scene);
    CHECK(fixture.audio->Status(fixed) == PlaybackStatus::Finished);
    REQUIRE(fixture.Frames(2));
    CHECK(CountFor(log, fixed, PlaybackEventKind::Ended) == 0);
    CHECK(fixture.audio->ControlStats().virtualPlaybacks == 0);
    RequireIdle(fixture);
}

// voice 없는 loop·정지 fade 중 voice가 있는 채로 엔진을 내려도 종료가 끝나고 PCM 참조를 모두
// 놓는다.
TEST(EngineAudio_ShutdownWithVoicelessPlaybacks) {
    SystemAllocator allocator;
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(allocator, 0.01f, OneSecond);
    {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        const SoundSourceRef loop = MakePlayable(fixture.allocator, pcm, true);
        EventLog log(fixture.allocator);
        Array<PlaybackHandle> handles(fixture.allocator);
        for (uint32 index = 0; index < VoiceLimit + 4; ++index) {
            REQUIRE(handles.TryPushBack(PlayLogged(fixture, log, loop)));
        }

        REQUIRE(fixture.Frames(StartDelivery));
        REQUIRE(fixture.audio->Stop(handles[0], OneSecond));
        REQUIRE(fixture.Frame());
        CHECK(fixture.audio->ControlStats().voicelessPlaybacks == 3);
    }

    CHECK(pcm.UseCount() == 1);
}

// 같은 장면을 Inline·Thread 제어로 돌리면 이벤트(종류·사유·mix frame·handle)가 같다(Thread는
// FlushControl gate).
TEST(EngineAudio_VoiceAllocationThreadMatchesInline) {
    SystemAllocator allocator;
    Array<PlaybackEvent> inlined(allocator);
    Array<PlaybackEvent> threaded(allocator);
    for (AudioControlMode mode : {AudioControlMode::Inline, AudioControlMode::Thread}) {
        AudioEngineHostOptions options = DeterministicOptions(true);
        options.controlMode = mode;
        AudioEngine fixture(options);
        REQUIRE(fixture.initialized);
        const SharedPtr<const SoundPcm> pcm =
            MakeConstantPcm(fixture.allocator, 0.01f, OneSecond / 4);
        const SoundSourceRef loop = MakePlayable(fixture.allocator, pcm, true);
        const SoundSourceRef shot = MakePlayable(fixture.allocator, pcm, false);
        EventLog log(fixture.allocator);
        Array<PlaybackHandle> loops(fixture.allocator);
        for (uint32 frame = 0; frame < MaxTestFrames / 2; ++frame) {
            if (frame == 0) {
                for (uint32 index = 0; index < VoiceLimit; ++index) {
                    REQUIRE(loops.TryPushBack(PlayLogged(fixture, log, loop)));
                }
            }

            if (frame == 3) {
                PlayOptions high;
                high.priority = FillerPriority;
                REQUIRE(PlayLogged(fixture, log, shot, Move(high)).generation != 0);
                REQUIRE(PlayLogged(fixture, log, shot).generation != 0);
            }

            if (frame == 20) {
                for (const PlaybackHandle handle : loops) {
                    REQUIRE(fixture.audio->Stop(handle, 0));
                }
            }

            REQUIRE(fixture.engine.RunFrame());
            fixture.audio->FlushControl();
            PumpDevice(fixture.audio, PumpFrames);
        }

        Array<PlaybackEvent>& out = mode == AudioControlMode::Inline ? inlined : threaded;
        for (const PlaybackEvent& event : log.events) {
            REQUIRE(out.TryPushBack(event));
        }
    }

    REQUIRE(inlined.Size() == threaded.Size());
    REQUIRE(!inlined.IsEmpty());
    for (usize index = 0; index < inlined.Size(); ++index) {
        CHECK(inlined[index].kind == threaded[index].kind);
        CHECK(inlined[index].reason == threaded[index].reason);
        CHECK(inlined[index].mixFrame == threaded[index].mixFrame);
        CHECK(inlined[index].handle == threaded[index].handle);
    }
}

// 우선순위 옵션 범위: [0, 100] 밖·NaN은 InvalidArgument다. 사운드 스냅숏도 같은 범위를 검사한다.
TEST(EngineAudio_PriorityOptionRange) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, 480);
    const SoundSourceRef sound = MakePlayable(fixture.allocator, pcm, false);
    for (float bad : {-0.5f, 100.5f, platform::PlatformMath::QuietNaN<float>()}) {
        PlayOptions options;
        options.priority = bad;
        CHECK(fixture.audio->Play2D(sound, Move(options)).Error() == ErrorCode::InvalidArgument);
    }

    PlayOptions edge;
    edge.priority = MaxSoundPriority;
    CHECK(fixture.audio->Play2D(sound, Move(edge)));
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.priority = 101.0f;
    CHECK(SoundSourceRef::TryCreate(fixture.allocator, playable).Error() ==
          ErrorCode::InvalidArgument);
    RequireIdle(fixture);
}

} // namespace audio_voice_allocation_tests_file
