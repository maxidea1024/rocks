#include "AudioEngineTestSupport.h"

#include <AudioAssets/AudioAssetsTypes.h>
#include <AudioAssets/ConcurrencyGroup.generated.h>
#include <AudioAssets/SoundClip.generated.h>
#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <TestSupport/Test.h>

// AU7b 동시 발음 시험이다(Docs/Design/AudioSystem 3.41.2절, UE FSoundConcurrencyManager): 규칙
// 일곱의 희생자·거부(동률 포함), 그룹 여럿 AND·희생자 중복 없음, 소유자 모드, 재시작 간격·빈 그룹
// 정리, volume scale(Default·CanRelease·Distance), steal fade(최소값·즉시 예외·fade 중
// Stop·SetVolume), cull은 voice 배정 전, 옵션 재정의·검사, 에셋 그룹 스냅숏, "발소리 폭주 중 대사".
// Inline 제어 + Manual 렌더 + Null Manual로 프레임·Pump로만 순서를 만든다.
using namespace rocks;
namespace audio_concurrency_tests_file {

using namespace rocks::engine_audio_test;

namespace {

constexpr uint32 OneSecond = TestRate;
// Play에서 Started가 게임에 닿기까지의 프레임 수다(수락·배정 → 렌더 → 수거 → 전달).
constexpr uint32 StartDelivery = 3;

SoundConcurrencySettings Rule(SoundConcurrencyRule rule, uint32 maxCount) noexcept {
    SoundConcurrencySettings settings;
    settings.resolutionRule = rule;
    settings.maxCount = maxCount;
    return settings;
}

SoundConcurrencyEntry Entry(uint64 key, const SoundConcurrencySettings& settings,
                            bool isOverride = false) noexcept {
    SoundConcurrencyEntry entry;
    entry.key = key;
    entry.isOverride = isOverride;
    entry.settings = settings;
    return entry;
}

SoundSourceRef Grouped(Allocator& allocator, const SharedPtr<const SoundPcm>& pcm, bool looping,
                       std::initializer_list<SoundConcurrencyEntry> entries,
                       float priority = DefaultSoundPriority,
                       const Optional<SoundAttenuation>& attenuation = {}) noexcept {
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.looping = looping;
    playable.priority = priority;
    playable.attenuation = attenuation;
    // 동시 발음만 본다: 가상화를 끄면 빼앗기거나 거부된 loop는 UE처럼 끝난다(가상화 가능 loop는
    // 가상이 된다 — AudioVirtualizationTests).
    playable.virtualizationMode = SoundVirtualizationMode::Disabled;
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

const PlaybackEvent* EndEvent(const EventLog& log, PlaybackHandle handle) noexcept {
    for (const PlaybackEvent& event : log.events) {
        if (event.handle == handle && event.kind == PlaybackEventKind::Ended) {
            return &event;
        }
    }

    return nullptr;
}

PlaybackEndReason EndOf(const EventLog& log, PlaybackHandle handle) noexcept {
    const PlaybackEvent* event = EndEvent(log, handle);
    return event != nullptr ? event->reason : PlaybackEndReason::None;
}

PlaybackHandle Play(AudioEngine& fixture, EventLog& log, const SoundSourceRef& sound,
                    PlayOptions options = {}) noexcept {
    options.onEvent = log.Callback(fixture.allocator);
    auto played = fixture.audio->Play2D(sound, Move(options));
    return played ? played.Value() : PlaybackHandle{};
}

PlaybackHandle PlayAt(AudioEngine& fixture, EventLog& log, AudioSceneId scene,
                      const SoundSourceRef& sound, double x) noexcept {
    PlayOptions options;
    options.onEvent = log.Callback(fixture.allocator);
    auto played = fixture.audio->PlayAtInScene(scene, sound, Vec3d(x, 0.0, 0.0), Move(options));
    return played ? played.Value() : PlaybackHandle{};
}

// 거리만 재는 감쇠(감쇠·패닝 없음 — 출력은 2D와 같다).
SoundAttenuation FlatAttenuation() noexcept {
    SoundAttenuation attenuation;
    attenuation.attenuate = false;
    attenuation.spatialize = false;
    return attenuation;
}

void StopAll(AudioEngine& fixture, std::initializer_list<PlaybackHandle> handles) noexcept {
    for (const PlaybackHandle handle : handles) {
        (void)fixture.audio->Stop(handle, 0);
    }
}

void RequireIdle(AudioEngine& fixture) noexcept {
    (void)fixture.RunUntilIdle();
    const AudioControlStats stats = fixture.audio->ControlStats();
    CHECK(stats.activeVoices == 0 && stats.stoppingVoices == 0 && stats.voicelessPlaybacks == 0);
    CHECK(stats.virtualPlaybacks == 0);
    CHECK(stats.releasingVoices == 0 && stats.registeredClips == 0);
    REQUIRE(fixture.Frame());
}

} // namespace

// PreventNew: 찬 그룹의 새 재생은 Started 없이 Ended(Concurrency)다. 그룹 하나라도 거부하면 전체
// 거부다 (AND — 다른 그룹에 자리가 있어도).
TEST(EngineAudio_ConcurrencyPreventNewAndGroupsCombine) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    const SoundConcurrencyEntry tight = Entry(11, Rule(SoundConcurrencyRule::PreventNew, 2));
    const SoundConcurrencyEntry loose = Entry(12, Rule(SoundConcurrencyRule::PreventNew, 8));
    const SoundSourceRef sound = Grouped(fixture.allocator, pcm, true, {loose, tight});
    EventLog log(fixture.allocator);
    const PlaybackHandle a = Play(fixture, log, sound);
    const PlaybackHandle b = Play(fixture, log, sound);
    const PlaybackHandle c = Play(fixture, log, sound);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(CountFor(log, a, PlaybackEventKind::Started) == 1);
    CHECK(CountFor(log, b, PlaybackEventKind::Started) == 1);
    CHECK(EndOf(log, c) == PlaybackEndReason::Concurrency);
    CHECK(CountFor(log, c, PlaybackEventKind::Started) == 0);
    AudioControlStats stats = fixture.audio->ControlStats();
    CHECK(stats.concurrencyRejects == 1);
    CHECK(stats.concurrencyGroups == 2);
    StopAll(fixture, {a, b});
    RequireIdle(fixture);
    CHECK(fixture.audio->ControlStats().concurrencyGroups == 0);
}

// StopOldest: 재생 시간이 가장 긴 재생을 빼앗는다. 같은 묶음에 수락해 재생 시간이 같으면 먼저
// 수락한 쪽이 희생자다. 빼앗긴 재생은 Ended(Stolen), 희생자는 그룹에서 바로 빠져 새 재생이 자리를
// 쓴다.
TEST(EngineAudio_ConcurrencyStopOldest) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    const SoundSourceRef sound = Grouped(fixture.allocator, pcm, true,
                                         {Entry(21, Rule(SoundConcurrencyRule::StopOldest, 2))});
    EventLog log(fixture.allocator);
    const PlaybackHandle first = Play(fixture, log, sound);
    const PlaybackHandle second = Play(fixture, log, sound);
    REQUIRE(fixture.Frames(StartDelivery));
    const PlaybackHandle third = Play(fixture, log, sound);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, first) == PlaybackEndReason::Stolen);
    CHECK(EndOf(log, second) == PlaybackEndReason::None);
    CHECK(CountFor(log, third, PlaybackEventKind::Started) == 1);
    // 이제 second가 더 오래됐다.
    const PlaybackHandle fourth = Play(fixture, log, sound);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, second) == PlaybackEndReason::Stolen);
    CHECK(EndOf(log, third) == PlaybackEndReason::None);
    CHECK(fixture.audio->ControlStats().concurrencySteals == 2);
    StopAll(fixture, {third, fourth});
    RequireIdle(fixture);
}

// StopFarthest…: 새 재생보다 먼 재생 중 가장 먼 것을 빼앗는다. 모두 더 가까우면 ThenPreventNew·
// ThenOldest 모두 거부한다. ThenOldest는 같은 거리면 오래된 쪽을 빼앗는다.
TEST(EngineAudio_ConcurrencyStopFarthest) {
    for (SoundConcurrencyRule rule : {SoundConcurrencyRule::StopFarthestThenPreventNew,
                                      SoundConcurrencyRule::StopFarthestThenOldest}) {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        const AudioSceneId scene = fixture.audio->RegisterScene();
        const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
        const SoundSourceRef sound =
            Grouped(fixture.allocator, pcm, true, {Entry(31, Rule(rule, 2))}, DefaultSoundPriority,
                    FlatAttenuation());
        EventLog log(fixture.allocator);
        const PlaybackHandle near = PlayAt(fixture, log, scene, sound, 1000.0);
        const PlaybackHandle far = PlayAt(fixture, log, scene, sound, 3000.0);
        REQUIRE(fixture.Frames(StartDelivery));
        const PlaybackHandle closer = PlayAt(fixture, log, scene, sound, 500.0);
        REQUIRE(fixture.Frames(StartDelivery));
        CHECK(EndOf(log, far) == PlaybackEndReason::Stolen);
        CHECK(CountFor(log, closer, PlaybackEventKind::Started) == 1);
        // 그룹에 1000·500이 남았다. 더 먼 새 재생은 빼앗을 대상이 없어 거부된다.
        const PlaybackHandle farther = PlayAt(fixture, log, scene, sound, 5000.0);
        REQUIRE(fixture.Frames(2));
        CHECK(EndOf(log, farther) == PlaybackEndReason::Concurrency);
        StopAll(fixture, {near, closer});
        RequireIdle(fixture);
    }

    // ThenOldest 동거리: 같은 3000에 있는 둘 중 오래된 쪽이 진다.
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    const SoundSourceRef sound =
        Grouped(fixture.allocator, pcm, true,
                {Entry(32, Rule(SoundConcurrencyRule::StopFarthestThenOldest, 2))},
                DefaultSoundPriority, FlatAttenuation());
    EventLog log(fixture.allocator);
    const PlaybackHandle older = PlayAt(fixture, log, scene, sound, 3000.0);
    REQUIRE(fixture.Frames(2));
    const PlaybackHandle newer = PlayAt(fixture, log, scene, sound, 3000.0);
    REQUIRE(fixture.Frames(2));
    const PlaybackHandle incoming = PlayAt(fixture, log, scene, sound, 100.0);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, older) == PlaybackEndReason::Stolen);
    CHECK(EndOf(log, newer) == PlaybackEndReason::None);
    StopAll(fixture, {newer, incoming});
    RequireIdle(fixture);
}

// cull 규칙: 수락 때 거부하지 않고 묶음마다 넘친 만큼 빼앗는다. voice 배정 전이라 진 새 재생은
// Started 없이 Stolen이다. StopLowestPriority 동률은 오래된 쪽, ThenPreventNew 동률은 새 쪽,
// StopQuietest는 크기 오름차순(동률은 새 쪽)이다.
TEST(EngineAudio_ConcurrencyCullRules) {
    struct Case {
        SoundConcurrencyRule rule;
        float priorities[3];
        float volumes[3];
        uint32 loser;
    };

    const Case cases[] = {
        {SoundConcurrencyRule::StopLowestPriority, {3.0f, 1.0f, 2.0f}, {1.0f, 1.0f, 1.0f}, 1},
        {SoundConcurrencyRule::StopLowestPriority, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, 0},
        {SoundConcurrencyRule::StopLowestPriorityThenPreventNew,
         {1.0f, 1.0f, 1.0f},
         {1.0f, 1.0f, 1.0f},
         2},
        {SoundConcurrencyRule::StopQuietest, {1.0f, 1.0f, 1.0f}, {0.5f, 0.2f, 0.8f}, 1},
        {SoundConcurrencyRule::StopQuietest, {1.0f, 1.0f, 1.0f}, {0.5f, 0.5f, 0.5f}, 2},
    };
    for (const Case& test : cases) {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
        EventLog log(fixture.allocator);
        PlaybackHandle handles[3];
        for (uint32 index = 0; index < 3; ++index) {
            const SoundSourceRef sound =
                Grouped(fixture.allocator, pcm, true, {Entry(41, Rule(test.rule, 2))},
                        test.priorities[index]);
            PlayOptions options;
            options.volume = test.volumes[index];
            handles[index] = Play(fixture, log, sound, Move(options));
        }

        REQUIRE(fixture.Frames(StartDelivery));
        for (uint32 index = 0; index < 3; ++index) {
            if (index == test.loser) {
                CHECK(EndOf(log, handles[index]) == PlaybackEndReason::Stolen);
                CHECK(CountFor(log, handles[index], PlaybackEventKind::Started) == 0);
            } else {
                CHECK(CountFor(log, handles[index], PlaybackEventKind::Started) == 1);
                CHECK(EndOf(log, handles[index]) == PlaybackEndReason::None);
            }
        }

        CHECK(fixture.audio->ControlStats().concurrencyRejects == 0);
        StopAll(fixture, {handles[0], handles[1], handles[2]});
        RequireIdle(fixture);
    }
}

// 희생자는 그룹이 여럿이어도 한 번만 빼앗는다. 소유자 제한: 소유자마다 따로 세고, 소유자가 없으면
// 전역 그룹이다. 재정의(isOverride) 키는 사운드마다다.
TEST(EngineAudio_ConcurrencyVictimDedupAndOwners) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    const SoundSourceRef both = Grouped(fixture.allocator, pcm, true,
                                        {Entry(51, Rule(SoundConcurrencyRule::StopOldest, 1)),
                                         Entry(52, Rule(SoundConcurrencyRule::StopOldest, 1))});
    EventLog log(fixture.allocator);
    const PlaybackHandle old = Play(fixture, log, both);
    REQUIRE(fixture.Frames(2));
    const PlaybackHandle fresh = Play(fixture, log, both);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, old) == PlaybackEndReason::Stolen);
    CHECK(fixture.audio->ControlStats().concurrencySteals == 1);

    SoundConcurrencySettings owned = Rule(SoundConcurrencyRule::PreventNew, 1);
    owned.limitToOwner = true;
    const SoundSourceRef perOwner = Grouped(fixture.allocator, pcm, true, {Entry(53, owned)});
    PlayOptions ownerA;
    ownerA.owner = 1;
    PlayOptions ownerB;
    ownerB.owner = 2;
    PlayOptions ownerAgain;
    ownerAgain.owner = 1;
    const PlaybackHandle a = Play(fixture, log, perOwner, Move(ownerA));
    const PlaybackHandle b = Play(fixture, log, perOwner, Move(ownerB));
    const PlaybackHandle again = Play(fixture, log, perOwner, Move(ownerAgain));
    const PlaybackHandle global = Play(fixture, log, perOwner);
    const PlaybackHandle globalAgain = Play(fixture, log, perOwner);
    // 재정의 키 둘은 서로 다른 그룹이다.
    const SoundSourceRef overrideOne = Grouped(
        fixture.allocator, pcm, true, {Entry(54, Rule(SoundConcurrencyRule::PreventNew, 1), true)});
    const SoundSourceRef overrideTwo = Grouped(
        fixture.allocator, pcm, true, {Entry(55, Rule(SoundConcurrencyRule::PreventNew, 1), true)});
    const PlaybackHandle o1 = Play(fixture, log, overrideOne);
    const PlaybackHandle o2 = Play(fixture, log, overrideTwo);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(CountFor(log, a, PlaybackEventKind::Started) == 1);
    CHECK(CountFor(log, b, PlaybackEventKind::Started) == 1);
    CHECK(EndOf(log, again) == PlaybackEndReason::Concurrency);
    CHECK(CountFor(log, global, PlaybackEventKind::Started) == 1);
    CHECK(EndOf(log, globalAgain) == PlaybackEndReason::Concurrency);
    CHECK(CountFor(log, o1, PlaybackEventKind::Started) == 1);
    CHECK(CountFor(log, o2, PlaybackEventKind::Started) == 1);
    StopAll(fixture, {fresh, a, b, global, o1, o2});
    RequireIdle(fixture);
}

// 재시작 간격: 간격 안의 새 재생은 거부되고, 간격(mix frame)이 지나면 받는다. 빈 그룹은 간격이 지난
// 뒤 지워진다.
TEST(EngineAudio_ConcurrencyRetrigger) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, 480);
    SoundConcurrencySettings settings = Rule(SoundConcurrencyRule::PreventNew, 8);
    settings.retriggerTime = 0.1f;
    const SoundSourceRef sound = Grouped(fixture.allocator, pcm, false, {Entry(61, settings)});
    EventLog log(fixture.allocator);
    const PlaybackHandle first = Play(fixture, log, sound);
    REQUIRE(fixture.Frame());
    const PlaybackHandle early = Play(fixture, log, sound);
    REQUIRE(fixture.Frames(2));
    CHECK(EndOf(log, early) == PlaybackEndReason::Concurrency);
    // 0.1 s = 4800 frame = 묶음 10개 남짓.
    REQUIRE(fixture.Frames(12));
    const PlaybackHandle late = Play(fixture, log, sound);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(CountFor(log, first, PlaybackEventKind::Started) == 1);
    CHECK(CountFor(log, late, PlaybackEventKind::Started) == 1);
    REQUIRE(fixture.Frames(14));
    CHECK(fixture.audio->ControlStats().concurrencyGroups == 0);
}

// volume scale Default: 새 재생이 들어오면 앞 재생이 volumeScale배로 줄어든다(attack 0 — 바로).
// CanRelease면 새 재생이 빠질 때 되돌아오고, 아니면 줄어든 채다.
TEST(EngineAudio_ConcurrencyVolumeScaleDefault) {
    for (bool canRelease : {false, true}) {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        const SharedPtr<const SoundPcm> loud = MakeConstantPcm(fixture.allocator, 0.4f, OneSecond);
        const SharedPtr<const SoundPcm> silent =
            MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
        SoundConcurrencySettings settings = Rule(SoundConcurrencyRule::PreventNew, 8);
        settings.volumeScale = 0.5f;
        settings.volumeScaleAttackTime = 0.0f;
        settings.volumeScaleReleaseTime = 0.0f;
        settings.volumeScaleCanRelease = canRelease;
        EventLog log(fixture.allocator);
        const PlaybackHandle first =
            Play(fixture, log, Grouped(fixture.allocator, loud, true, {Entry(71, settings)}));
        Array<float> chunk(fixture.allocator);
        REQUIRE(chunk.TryResize(PumpFrames * 2));
        REQUIRE(fixture.Frames(StartDelivery));
        (void)CapturePeak(fixture, chunk);
        // mono Direct 0.5배: 0.4 × 0.5 = 0.2.
        CHECK(math::Abs(CapturePeak(fixture, chunk).left - 0.2f) < 1.0e-3f);
        const PlaybackHandle second =
            Play(fixture, log, Grouped(fixture.allocator, silent, true, {Entry(71, settings)}));
        REQUIRE(fixture.Frames(StartDelivery));
        (void)CapturePeak(fixture, chunk);
        CHECK(math::Abs(CapturePeak(fixture, chunk).left - 0.1f) < 1.0e-3f);
        REQUIRE(fixture.audio->Stop(second, 0));
        REQUIRE(fixture.Frames(StartDelivery));
        (void)CapturePeak(fixture, chunk);
        const float expected = canRelease ? 0.2f : 0.1f;
        CHECK(math::Abs(CapturePeak(fixture, chunk).left - expected) < 1.0e-3f);
        StopAll(fixture, {first});
        RequireIdle(fixture);
    }
}

// volume scale Distance: 가까운 재생이 1, 먼 재생이 volumeScale배다(묶음마다 다시 매긴다).
TEST(EngineAudio_ConcurrencyVolumeScaleDistance) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> loud = MakeConstantPcm(fixture.allocator, 0.4f, OneSecond);
    const SharedPtr<const SoundPcm> silent = MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
    SoundConcurrencySettings settings = Rule(SoundConcurrencyRule::PreventNew, 8);
    settings.volumeScale = 0.5f;
    settings.volumeScaleMode = SoundConcurrencyVolumeScaleMode::Distance;
    settings.volumeScaleAttackTime = 0.0f;
    EventLog log(fixture.allocator);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    const PlaybackHandle far = PlayAt(fixture, log, scene,
                                      Grouped(fixture.allocator, loud, true, {Entry(81, settings)},
                                              DefaultSoundPriority, FlatAttenuation()),
                                      3000.0);
    const PlaybackHandle near =
        PlayAt(fixture, log, scene,
               Grouped(fixture.allocator, silent, true, {Entry(81, settings)}, DefaultSoundPriority,
                       FlatAttenuation()),
               100.0);
    REQUIRE(fixture.Frames(StartDelivery));
    (void)CapturePeak(fixture, chunk);
    CHECK(math::Abs(CapturePeak(fixture, chunk).left - 0.1f) < 1.0e-3f);
    StopAll(fixture, {far, near});
    RequireIdle(fixture);
}

// steal fade: 희생자는 그룹들의 voiceStealReleaseTime 최소값 동안 줄어든 뒤 멈춘다(끝 frame ≥ 수락
// + fade). fade 중 SetVolume은 fade를 취소하지 않고, Stop은 사유를 Stolen으로 둔다. cull 규칙
// 그룹의 loop가 0.1 s 안쪽만 냈으면 fade 없이 멈춘다.
TEST(EngineAudio_ConcurrencyStealFade) {
    constexpr float Release = 0.2f;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    SoundConcurrencySettings slow = Rule(SoundConcurrencyRule::StopOldest, 1);
    slow.voiceStealReleaseTime = Release;
    SoundConcurrencySettings slower = Rule(SoundConcurrencyRule::StopOldest, 4);
    slower.voiceStealReleaseTime = Release * 4.0f;
    const SoundSourceRef sound =
        Grouped(fixture.allocator, pcm, true, {Entry(91, slow), Entry(92, slower)});
    EventLog log(fixture.allocator);
    const PlaybackHandle victim = Play(fixture, log, sound);
    REQUIRE(fixture.Frames(StartDelivery));
    const uint64 stealFrame = fixture.audio->Device()->Mixer().CompletedMixFrame();
    const PlaybackHandle winner = Play(fixture, log, sound);
    REQUIRE(fixture.Frame());
    // fade 중 SetVolume은 fade를 지우지 않는다.
    REQUIRE(fixture.audio->SetParameter(victim, PlaybackParameter::Volume, 1.0f, 0));
    for (uint32 frame = 0; frame < MaxTestFrames && EndEvent(log, victim) == nullptr; ++frame) {
        REQUIRE(fixture.Frame());
    }

    const PlaybackEvent* ended = EndEvent(log, victim);
    REQUIRE(ended != nullptr);
    CHECK(ended->reason == PlaybackEndReason::Stolen);
    // 최소값(0.2 s)이 쓰였다: 0.8 s보다 일찍, 0.2 s보다 늦게 끝난다.
    CHECK(ended->mixFrame >= stealFrame + static_cast<uint64>(Release * OneSecond));
    CHECK(ended->mixFrame < stealFrame + static_cast<uint64>(Release * 4.0f * OneSecond));
    // fade 중 Stop: 사유는 Stolen이다.
    const PlaybackHandle next = Play(fixture, log, sound);
    REQUIRE(fixture.Frame());
    REQUIRE(fixture.audio->Stop(winner, 0));
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, winner) == PlaybackEndReason::Stolen);

    // cull 규칙 loop가 0.1 s 안쪽만 냈으면 바로 멈춘다(512 frame 정지 fade).
    SoundConcurrencySettings cull = Rule(SoundConcurrencyRule::StopLowestPriority, 1);
    cull.voiceStealReleaseTime = 1.0f;
    const SoundSourceRef low = Grouped(fixture.allocator, pcm, true, {Entry(93, cull)}, 1.0f);
    const SoundSourceRef high = Grouped(fixture.allocator, pcm, true, {Entry(93, cull)}, 5.0f);
    const PlaybackHandle quick = Play(fixture, log, low);
    REQUIRE(fixture.Frame());
    const uint64 cullFrame = fixture.audio->Device()->Mixer().CompletedMixFrame();
    const PlaybackHandle strong = Play(fixture, log, high);
    for (uint32 frame = 0; frame < MaxTestFrames && EndEvent(log, quick) == nullptr; ++frame) {
        REQUIRE(fixture.Frame());
    }

    REQUIRE(EndEvent(log, quick) != nullptr);
    CHECK(EndEvent(log, quick)->reason == PlaybackEndReason::Stolen);
    CHECK(EndEvent(log, quick)->mixFrame < cullFrame + OneSecond / 2);
    StopAll(fixture, {next, strong});
    RequireIdle(fixture);
}

// 옵션 재정의(UE PlaySound ConcurrencySettings): 사운드에 그룹이 없어도 옵션 그룹으로 센다. 키
// 0·범위 밖 설정은 InvalidArgument, 수 초과는 CapacityExceeded다. 스냅숏도 같은 검사를 한다.
TEST(EngineAudio_ConcurrencyOptionOverrideAndValidation) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    // 그룹 없는 Disabled loop(옵션 그룹만 쓴다).
    const SoundSourceRef plain = Grouped(fixture.allocator, pcm, true, {});
    EventLog log(fixture.allocator);
    const auto overridden = [&]() noexcept {
        PlayOptions options;
        options.overrideConcurrency = true;
        options.concurrencyCount = 1;
        options.concurrency[0] = Entry(101, Rule(SoundConcurrencyRule::PreventNew, 1));
        return options;
    };
    const PlaybackHandle first = Play(fixture, log, plain, overridden());
    const PlaybackHandle second = Play(fixture, log, plain, overridden());
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(CountFor(log, first, PlaybackEventKind::Started) == 1);
    CHECK(EndOf(log, second) == PlaybackEndReason::Concurrency);

    PlayOptions zeroKey = overridden();
    zeroKey.concurrency[0].key = 0;
    CHECK(fixture.audio->Play2D(plain, Move(zeroKey)).Error() == ErrorCode::InvalidArgument);
    PlayOptions badScale = overridden();
    badScale.concurrency[0].settings.volumeScale = 1.5f;
    CHECK(fixture.audio->Play2D(plain, Move(badScale)).Error() == ErrorCode::InvalidArgument);
    PlayOptions tooMany = overridden();
    tooMany.concurrencyCount = MaxConcurrencyGroups + 1;
    CHECK(fixture.audio->Play2D(plain, Move(tooMany)).Error() == ErrorCode::CapacityExceeded);
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.concurrencyCount = 1;
    playable.concurrency[0] = Entry(102, Rule(SoundConcurrencyRule::PreventNew, 0));
    CHECK(SoundSourceRef::TryCreate(fixture.allocator, playable).Error() ==
          ErrorCode::InvalidArgument);
    StopAll(fixture, {first});
    RequireIdle(fixture);
}

// 에셋 그룹: SoundClip이 참조한 ConcurrencyGroup의 설정이 스냅숏에 담기고, 키는 그룹 객체의 것이라
// 두 사운드가 같은 그룹을 공유한다. 재정의면 사운드 객체 키의 이 사운드만의 그룹이다.
TEST(EngineAudio_ConcurrencyAssetSnapshot) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterAudioAssetsTypes(registration));
    ObjectRegistry objects(allocator, types, {0x41555537424f3031ull, 1});
    auto group =
        objects.NewObject<ConcurrencyGroup>(objects.OwnerAccess(), TypeIdOf<ConcurrencyGroup>());
    auto first = objects.NewObject<SoundClip>(objects.OwnerAccess(), TypeIdOf<SoundClip>());
    auto second = objects.NewObject<SoundClip>(objects.OwnerAccess(), TypeIdOf<SoundClip>());
    REQUIRE(group && first && second);
    auto groupBorrow =
        objects.Resolve<ConcurrencyGroup>(objects.OwnerAccess(), group.Value().Handle());
    auto firstBorrow = objects.Resolve<SoundClip>(objects.OwnerAccess(), first.Value().Handle());
    auto secondBorrow = objects.Resolve<SoundClip>(objects.OwnerAccess(), second.Value().Handle());
    REQUIRE(groupBorrow && firstBorrow && secondBorrow);
    ConcurrencyGroup& concurrency = *groupBorrow.Value().Get().Value();
    SoundClip& a = *firstBorrow.Value().Get().Value();
    SoundClip& b = *secondBorrow.Value().Get().Value();
    concurrency.settings.maxCount = 3;
    concurrency.settings.resolutionRule = SoundConcurrencyRule::StopQuietest;
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(allocator, 0.1f, 64);
    REQUIRE(a.SetPcm(pcm));
    REQUIRE(b.SetPcm(pcm));
    REQUIRE(objects.AddReference(objects.OwnerAccess(), first.Value().Handle(), a.concurrency,
                                 group.Value().Handle()));
    REQUIRE(objects.AddReference(objects.OwnerAccess(), second.Value().Handle(), b.concurrency,
                                 group.Value().Handle()));
    auto snapshotA = a.MakePlayable(allocator, objects, objects.OwnerAccess());
    auto snapshotB = b.MakePlayable(allocator, objects, objects.OwnerAccess());
    REQUIRE(snapshotA && snapshotB);
    REQUIRE(snapshotA.Value().Get()->concurrencyCount == 1);
    const SoundConcurrencyEntry& entryA = snapshotA.Value().Get()->concurrency[0];
    const SoundConcurrencyEntry& entryB = snapshotB.Value().Get()->concurrency[0];
    CHECK(entryA.key != 0 && entryA.key == entryB.key && !entryA.isOverride);
    CHECK(entryA.settings.maxCount == 3);
    CHECK(entryA.settings.resolutionRule == SoundConcurrencyRule::StopQuietest);
    // 재정의는 사운드 객체 키다(두 사운드가 다르다).
    a.overrideConcurrency = true;
    b.overrideConcurrency = true;
    auto overrideA = a.MakePlayable(allocator, objects, objects.OwnerAccess());
    auto overrideB = b.MakePlayable(allocator, objects, objects.OwnerAccess());
    REQUIRE(overrideA && overrideB);
    CHECK(overrideA.Value().Get()->concurrency[0].isOverride);
    CHECK(overrideA.Value().Get()->concurrency[0].key !=
          overrideB.Value().Get()->concurrency[0].key);
    // 범위 밖 그룹 설정은 스냅숏을 만들지 않는다.
    a.overrideConcurrency = false;
    concurrency.settings.maxCount = 0;
    CHECK(a.MakePlayable(allocator, objects, objects.OwnerAccess()).Error() ==
          ErrorCode::InvalidArgument);
}

// "발소리 폭주 중 대사"(3.19절): 발소리 그룹(최대 4, StopOldest)을 묶음마다 쏟아부어도 그룹 수는
// 4를 넘지 않고, 다른 그룹의 대사는 시작해서 빼앗기지 않는다.
TEST(EngineAudio_ConcurrencyFootstepStormKeepsDialogue) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> step = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond / 4);
    const SharedPtr<const SoundPcm> speech =
        MakeConstantPcm(fixture.allocator, 0.01f, OneSecond * 2);
    const SoundSourceRef footstep = Grouped(
        fixture.allocator, step, false, {Entry(111, Rule(SoundConcurrencyRule::StopOldest, 4))});
    const SoundSourceRef dialogue =
        Grouped(fixture.allocator, speech, false,
                {Entry(112, Rule(SoundConcurrencyRule::PreventNew, 1))}, 10.0f);
    EventLog log(fixture.allocator);
    const PlaybackHandle line = Play(fixture, log, dialogue);
    for (uint32 frame = 0; frame < 30; ++frame) {
        for (uint32 burst = 0; burst < 6; ++burst) {
            REQUIRE(Play(fixture, log, footstep).generation != 0);
        }

        REQUIRE(fixture.Frame());
        CHECK(fixture.audio->ControlStats().activeVoices <= 5);
    }

    CHECK(CountFor(log, line, PlaybackEventKind::Started) == 1);
    CHECK(EndOf(log, line) == PlaybackEndReason::None);
    CHECK(fixture.audio->ControlStats().concurrencySteals > 0);
    REQUIRE(fixture.audio->Stop(line, 0));
    RequireIdle(fixture);
}

} // namespace audio_concurrency_tests_file
