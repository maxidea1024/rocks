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

// AU7b 동시 발음 경계 시험이다(리뷰 반영 — 같은 거리·첫 거부·첫 묶음 scale·Priority·dB attack·voice
// 없는 희생자· 중복·표 용량·steal fade 레벨·에셋 중복·초과). 아래는 AudioConcurrencyTests.cpp와
// 같은 도우미다. 원래 설명: AU7b 동시 발음 시험이다(Docs/Design/AudioSystem 3.41.2절, UE
// FSoundConcurrencyManager): 규칙 일곱의 희생자·거부(동률 포함), 그룹 여럿 AND·희생자 중복 없음,
// 소유자 모드, 재시작 간격·빈 그룹 정리, volume scale(Default·CanRelease·Distance), steal
// fade(최소값·즉시 예외·fade 중 Stop·SetVolume), cull은 voice 배정 전, 옵션 재정의·검사, 에셋 그룹
// 스냅숏, "발소리 폭주 중 대사". Inline 제어 + Manual 렌더 + Null Manual로 프레임·Pump로만 순서를
// 만든다.
using namespace rocks;
namespace audio_concurrency_edge_tests_file {

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

// 2D(거리 0)·같은 위치: StopFarthestThenOldest는 가장 오래된 재생을 빼앗고(UE 기본 규칙 — 같은 거리
// 분기), ThenPreventNew는 거부한다.
TEST(EngineAudio_ConcurrencyFarthestSameDistance) {
    for (SoundConcurrencyRule rule : {SoundConcurrencyRule::StopFarthestThenOldest,
                                      SoundConcurrencyRule::StopFarthestThenPreventNew}) {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
        const SoundSourceRef sound =
            Grouped(fixture.allocator, pcm, true, {Entry(121, Rule(rule, 2))});
        EventLog log(fixture.allocator);
        const PlaybackHandle older = Play(fixture, log, sound);
        REQUIRE(fixture.Frames(2));
        const PlaybackHandle newer = Play(fixture, log, sound);
        REQUIRE(fixture.Frames(2));
        const PlaybackHandle incoming = Play(fixture, log, sound);
        REQUIRE(fixture.Frames(StartDelivery));
        if (rule == SoundConcurrencyRule::StopFarthestThenOldest) {
            CHECK(EndOf(log, older) == PlaybackEndReason::Stolen);
            CHECK(EndOf(log, newer) == PlaybackEndReason::None);
            CHECK(CountFor(log, incoming, PlaybackEventKind::Started) == 1);
            StopAll(fixture, {newer, incoming});
        } else {
            CHECK(EndOf(log, incoming) == PlaybackEndReason::Concurrency);
            CHECK(EndOf(log, older) == PlaybackEndReason::None);
            StopAll(fixture, {older, newer});
        }

        RequireIdle(fixture);
    }
}

// 첫 거부에서 평가를 멈춘다: 앞 그룹이 거부한 재생은 뒤 그룹의 마지막 재생 시각을 바꾸지 않는다(UE
// return nullptr).
TEST(EngineAudio_ConcurrencyRejectStopsEvaluation) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    SoundConcurrencySettings retrigger = Rule(SoundConcurrencyRule::PreventNew, 8);
    retrigger.retriggerTime = 0.1f;
    const SoundSourceRef sound =
        Grouped(fixture.allocator, pcm, true,
                {Entry(131, Rule(SoundConcurrencyRule::PreventNew, 1)), Entry(132, retrigger)});
    EventLog log(fixture.allocator);
    const PlaybackHandle first = Play(fixture, log, sound);
    // 0.15 s 뒤: 앞 그룹(최대 1)이 거부한다.
    REQUIRE(fixture.Frames(14));
    const PlaybackHandle rejected = Play(fixture, log, sound);
    REQUIRE(fixture.Frame());
    REQUIRE(fixture.audio->Stop(first, 0));
    REQUIRE(fixture.Frames(4));
    // 첫 재생에서 0.1 s가 넘게 지났다: 거부된 재생이 재시작 시각을 바꿨다면 여기서도 거부된다.
    const PlaybackHandle third = Play(fixture, log, sound);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, rejected) == PlaybackEndReason::Concurrency);
    CHECK(CountFor(log, third, PlaybackEventKind::Started) == 1);
    StopAll(fixture, {third});
    RequireIdle(fixture);
}

// Distance 순서의 새 재생은 첫 묶음부터 제 scale이다(첫 렌더 블록에서 먼 재생이 이미 0.5배).
TEST(EngineAudio_ConcurrencyDistanceScaleFirstBatch) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    const SharedPtr<const SoundPcm> loud = MakeConstantPcm(fixture.allocator, 0.4f, OneSecond);
    const SharedPtr<const SoundPcm> silent = MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
    SoundConcurrencySettings settings = Rule(SoundConcurrencyRule::PreventNew, 8);
    settings.volumeScale = 0.5f;
    settings.volumeScaleMode = SoundConcurrencyVolumeScaleMode::Distance;
    EventLog log(fixture.allocator);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    const PlaybackHandle far = PlayAt(fixture, log, scene,
                                      Grouped(fixture.allocator, loud, true, {Entry(141, settings)},
                                              DefaultSoundPriority, FlatAttenuation()),
                                      3000.0);
    const PlaybackHandle near =
        PlayAt(fixture, log, scene,
               Grouped(fixture.allocator, silent, true, {Entry(141, settings)},
                       DefaultSoundPriority, FlatAttenuation()),
               100.0);
    // 이 프레임의 렌더가 첫 블록이다.
    CHECK(CapturePeak(fixture, chunk).left <= 0.1f + 1.0e-3f);
    StopAll(fixture, {far, near});
    RequireIdle(fixture);
}

// Priority 순서: 우선순위가 높은 재생이 1, 낮은 재생이 volumeScale배다.
TEST(EngineAudio_ConcurrencyVolumeScalePriority) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> loud = MakeConstantPcm(fixture.allocator, 0.4f, OneSecond);
    const SharedPtr<const SoundPcm> silent = MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
    SoundConcurrencySettings settings = Rule(SoundConcurrencyRule::PreventNew, 8);
    settings.volumeScale = 0.5f;
    settings.volumeScaleMode = SoundConcurrencyVolumeScaleMode::Priority;
    settings.volumeScaleAttackTime = 0.0f;
    EventLog log(fixture.allocator);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    const PlaybackHandle low =
        Play(fixture, log, Grouped(fixture.allocator, loud, true, {Entry(151, settings)}, 1.0f));
    const PlaybackHandle high =
        Play(fixture, log, Grouped(fixture.allocator, silent, true, {Entry(151, settings)}, 5.0f));
    REQUIRE(fixture.Frames(StartDelivery));
    (void)CapturePeak(fixture, chunk);
    CHECK(math::Abs(CapturePeak(fixture, chunk).left - 0.1f) < 1.0e-3f);
    StopAll(fixture, {low, high});
    RequireIdle(fixture);
}

// attack > 0: Default ducking은 dB 선형으로 내려간다(가운데에서 √0.5배 근처, 끝에서 0.5배).
TEST(EngineAudio_ConcurrencyVolumeScaleDecibelRamp) {
    constexpr float Attack = 0.2f;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> loud = MakeConstantPcm(fixture.allocator, 0.4f, OneSecond * 2);
    const SharedPtr<const SoundPcm> silent =
        MakeConstantPcm(fixture.allocator, 0.0f, OneSecond * 2);
    SoundConcurrencySettings settings = Rule(SoundConcurrencyRule::PreventNew, 8);
    settings.volumeScale = 0.5f;
    settings.volumeScaleAttackTime = Attack;
    EventLog log(fixture.allocator);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    const PlaybackHandle first =
        Play(fixture, log, Grouped(fixture.allocator, loud, true, {Entry(161, settings)}));
    REQUIRE(fixture.Frames(StartDelivery));
    const PlaybackHandle second =
        Play(fixture, log, Grouped(fixture.allocator, silent, true, {Entry(161, settings)}));
    // attack 0.2 s = 9600 frame ≈ 묶음 19개. 가운데(묶음 9–10개 뒤)는 0.2 × 10^(−3.01/20) ≈ 0.141.
    const uint32 half = static_cast<uint32>(Attack * OneSecond / 2.0f) / PumpFrames;
    REQUIRE(fixture.Frames(half));
    const float middle = CapturePeak(fixture, chunk).left;
    CHECK(middle > 0.12f && middle < 0.165f);
    REQUIRE(fixture.Frames(half + 4));
    (void)CapturePeak(fixture, chunk);
    CHECK(math::Abs(CapturePeak(fixture, chunk).left - 0.1f) < 1.0e-3f);
    StopAll(fixture, {first, second});
    RequireIdle(fixture);
}

// voice 없는 희생자(무음 loop)는 fade 없이 바로 Ended(Stolen)이고 Started가 없다.
TEST(EngineAudio_ConcurrencyVoicelessVictim) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    const SoundSourceRef sound = Grouped(fixture.allocator, pcm, true,
                                         {Entry(171, Rule(SoundConcurrencyRule::StopOldest, 1))});
    EventLog log(fixture.allocator);
    PlayOptions muted;
    muted.volume = 0.0f;
    const PlaybackHandle silentLoop = Play(fixture, log, sound, Move(muted));
    REQUIRE(fixture.Frames(2));
    REQUIRE(fixture.audio->ControlStats().voicelessPlaybacks == 1);
    const PlaybackHandle winner = Play(fixture, log, sound);
    REQUIRE(fixture.Frames(StartDelivery));
    CHECK(EndOf(log, silentLoop) == PlaybackEndReason::Stolen);
    CHECK(CountFor(log, silentLoop, PlaybackEventKind::Started) == 0);
    CHECK(CountFor(log, winner, PlaybackEventKind::Started) == 1);
    StopAll(fixture, {winner});
    RequireIdle(fixture);
}

// 같은 그룹 중복: 스냅숏·옵션 목록에 같은 (키, 재정의) 항목이 둘이면 InvalidArgument다.
TEST(EngineAudio_ConcurrencyDuplicateEntriesRejected) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    SoundPlayable playable;
    playable.pcm = pcm;
    playable.concurrencyCount = 2;
    playable.concurrency[0] = Entry(181, Rule(SoundConcurrencyRule::PreventNew, 2));
    playable.concurrency[1] = Entry(181, Rule(SoundConcurrencyRule::PreventNew, 2));
    CHECK(SoundSourceRef::TryCreate(fixture.allocator, playable).Error() ==
          ErrorCode::InvalidArgument);
    // 같은 키라도 재정의 표지가 다르면 다른 그룹이다.
    playable.concurrency[1].isOverride = true;
    CHECK(SoundSourceRef::TryCreate(fixture.allocator, playable));
    PlayOptions options;
    options.overrideConcurrency = true;
    options.concurrencyCount = 2;
    options.concurrency[0] = Entry(182, Rule(SoundConcurrencyRule::PreventNew, 2));
    options.concurrency[1] = Entry(182, Rule(SoundConcurrencyRule::PreventNew, 2));
    CHECK(fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true), Move(options)).Error() ==
          ErrorCode::InvalidArgument);
}

// 그룹 표가 차면 새 그룹이 필요한 재생은 아무도 빼앗지 않고 Failed다.
TEST(EngineAudio_ConcurrencyGroupTableFull) {
    constexpr uint32 Plays = 128;
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.01f, OneSecond);
    EventLog log(fixture.allocator);
    Array<PlaybackHandle> handles(fixture.allocator);
    uint64 key = 1000;
    // 재생 128개 × 그룹 8개 = 1024칸.
    for (uint32 play = 0; play < Plays; ++play) {
        SoundPlayable playable;
        playable.pcm = pcm;
        playable.looping = true;
        playable.concurrencyCount = MaxConcurrencyGroups;
        for (uint32 index = 0; index < MaxConcurrencyGroups; ++index) {
            playable.concurrency[index] = Entry(++key, Rule(SoundConcurrencyRule::StopOldest, 1));
        }

        auto sound = SoundSourceRef::TryCreate(fixture.allocator, playable);
        REQUIRE(sound);
        REQUIRE(handles.TryPushBack(Play(fixture, log, sound.Value())));
    }

    REQUIRE(fixture.Frames(2));
    REQUIRE(fixture.audio->ControlStats().concurrencyGroups == 1024);
    // 이미 있는 그룹 하나(빼앗기 대상 있음) + 새 그룹 하나: 새 칸이 없어 아무도 빼앗지 않는다.
    const SoundSourceRef overflow =
        Grouped(fixture.allocator, pcm, true,
                {Entry(1001, Rule(SoundConcurrencyRule::StopOldest, 1)),
                 Entry(++key, Rule(SoundConcurrencyRule::StopOldest, 1))});
    const PlaybackHandle failed = Play(fixture, log, overflow);
    REQUIRE(fixture.Frames(2));
    CHECK(EndOf(log, failed) == PlaybackEndReason::Failed);
    CHECK(fixture.audio->ControlStats().concurrencySteals == 0);
    for (const PlaybackHandle handle : handles) {
        CHECK(EndOf(log, handle) == PlaybackEndReason::None);
        (void)fixture.audio->Stop(handle, 0);
    }

    RequireIdle(fixture);
}

// steal fade 중 SetVolume(1)을 받아도 소리는 계속 줄어든다(steal fader는 사용자 ramp와 따로다).
TEST(EngineAudio_ConcurrencyStealFadeLowersLevel) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SharedPtr<const SoundPcm> loud = MakeConstantPcm(fixture.allocator, 0.4f, OneSecond);
    const SharedPtr<const SoundPcm> silent = MakeConstantPcm(fixture.allocator, 0.0f, OneSecond);
    SoundConcurrencySettings settings = Rule(SoundConcurrencyRule::StopOldest, 1);
    settings.voiceStealReleaseTime = 0.4f;
    EventLog log(fixture.allocator);
    Array<float> chunk(fixture.allocator);
    REQUIRE(chunk.TryResize(PumpFrames * 2));
    const PlaybackHandle victim =
        Play(fixture, log, Grouped(fixture.allocator, loud, true, {Entry(191, settings)}));
    REQUIRE(fixture.Frames(StartDelivery));
    (void)CapturePeak(fixture, chunk);
    CHECK(math::Abs(CapturePeak(fixture, chunk).left - 0.2f) < 1.0e-3f);
    const PlaybackHandle winner =
        Play(fixture, log, Grouped(fixture.allocator, silent, true, {Entry(191, settings)}));
    REQUIRE(fixture.Frame());
    REQUIRE(fixture.audio->SetParameter(victim, PlaybackParameter::Volume, 1.0f, 0));
    // fade 0.4 s의 가운데(−40 dB): 0.2 × 0.01 = 0.002 근처, 첫 값보다 훨씬 작다.
    REQUIRE(fixture.Frames(18));
    CHECK(CapturePeak(fixture, chunk).left < 0.02f);
    for (uint32 frame = 0; frame < MaxTestFrames && EndEvent(log, victim) == nullptr; ++frame) {
        REQUIRE(fixture.Frame());
    }

    CHECK(EndOf(log, victim) == PlaybackEndReason::Stolen);
    StopAll(fixture, {winner});
    RequireIdle(fixture);
}

// 에셋 그룹 경계: 같은 그룹을 두 번 참조하면 하나로 보고(UE ConcurrencySet은 집합), 서로 다른 그룹
// 9개는 CapacityExceeded다.
TEST(EngineAudio_ConcurrencyAssetDuplicatesAndCapacity) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterAudioAssetsTypes(registration));
    ObjectRegistry objects(allocator, types, {0x41555537424f3032ull, 1});
    auto group =
        objects.NewObject<ConcurrencyGroup>(objects.OwnerAccess(), TypeIdOf<ConcurrencyGroup>());
    auto first = objects.NewObject<SoundClip>(objects.OwnerAccess(), TypeIdOf<SoundClip>());
    auto second = objects.NewObject<SoundClip>(objects.OwnerAccess(), TypeIdOf<SoundClip>());
    REQUIRE(group && first && second);
    auto firstBorrow = objects.Resolve<SoundClip>(objects.OwnerAccess(), first.Value().Handle());
    auto secondBorrow = objects.Resolve<SoundClip>(objects.OwnerAccess(), second.Value().Handle());
    REQUIRE(firstBorrow && secondBorrow);
    SoundClip& a = *firstBorrow.Value().Get().Value();
    SoundClip& b = *secondBorrow.Value().Get().Value();
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(allocator, 0.1f, 64);
    REQUIRE(a.SetPcm(pcm));
    REQUIRE(b.SetPcm(pcm));
    for (uint32 repeat = 0; repeat < 2; ++repeat) {
        REQUIRE(objects.AddReference(objects.OwnerAccess(), first.Value().Handle(), a.concurrency,
                                     group.Value().Handle()));
    }

    auto twice = a.MakePlayable(allocator, objects, objects.OwnerAccess());
    REQUIRE(twice);
    CHECK(twice.Value().Get()->concurrencyCount == 1);
    for (uint32 index = 0; index <= MaxConcurrencyGroups; ++index) {
        auto extra = objects.NewObject<ConcurrencyGroup>(objects.OwnerAccess(),
                                                         TypeIdOf<ConcurrencyGroup>());
        REQUIRE(extra);
        REQUIRE(objects.AddReference(objects.OwnerAccess(), second.Value().Handle(), b.concurrency,
                                     extra.Value().Handle()));
    }

    CHECK(b.MakePlayable(allocator, objects, objects.OwnerAccess()).Error() ==
          ErrorCode::CapacityExceeded);
}

} // namespace audio_concurrency_edge_tests_file
