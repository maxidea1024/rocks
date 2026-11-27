#include "AudioEngineTestSupport.h"

#include <Base/InitializerList.h>
#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Module/ModuleEntry.h>
#include <AudioAssets/AudioAssetsTypes.h>
#include <AudioAssets/AudioObjectKey.h>
#include <AudioAssets/MixSnapshot.generated.h>
#include <AudioAssets/SoundCategory.generated.h>
#include <EngineAudio/AudioEngineSettings.h>
#include <Object/ObjectRegistry.h>
#include <TestSupport/Test.h>

// AU7e1 분류·믹스 스냅숏 시험이다(Docs/Design/AudioSystem 3.43절): 사슬 계층(volume 곱·LPF 잎만),
// 사용자 볼륨 CVar, 믹스 push·pop·fade·duration·applyToChildren, override, base, passive,
// always-play 동시 발음 순위, 분류 감쇠 거리 배수, 재생 필터·스테레오 정규화, 에셋 사슬(순환·깊이).
// Inline 제어 + Manual 렌더로 프레임·Pump만으로 순서를 만든다. 출력은 mono 상수 0.2의 Direct(0.5배)
// = 0.1 기준이다.
using namespace rocks;
namespace audio_category_tests_file {

using namespace rocks::engine_audio_test;

namespace {

constexpr uint32 StartFrames = 6;
constexpr float DirectLevel = 0.1f;
// 분류 키(시험용 — 객체 없이 사슬을 만든다)다.
constexpr uint64 RootKey = 0x5001;
constexpr uint64 LeafKey = 0x5002;
constexpr uint64 OtherKey = 0x5003;
constexpr uint64 MixKey = 0x6001;

bool Near(float actual, float expected, float tolerance = 1.0e-4f) noexcept {
    return math::Abs(actual - expected) <= tolerance;
}

struct LinkSpec {
    uint64 key = 0;
    float volume = 1.0f;
    float lowPass = 20000.0f;
    SoundUserVolume userVolume = SoundUserVolume::None;
    bool alwaysPlay = false;
};

SoundCategoryChainRef Chain(Allocator& allocator, std::initializer_list<LinkSpec> links,
                            const MixSnapshotRef& passive = {}) noexcept {
    auto made = SharedPtr<SoundCategoryChain>::TryMake(allocator);
    if (!made) {
        return {};
    }

    SoundCategoryChain& chain = *made.Value().Get();
    for (const LinkSpec& spec : links) {
        SoundCategoryLink& link = chain.links[chain.depth++];
        link.key = spec.key;
        link.properties.volume = spec.volume;
        link.properties.lowPassFrequency = spec.lowPass;
        link.properties.userVolume = spec.userVolume;
        link.properties.alwaysPlay = spec.alwaysPlay;
    }

    if (passive) {
        chain.links[0].passive[0].mix = passive;
        chain.links[0].passiveCount = 1;
    }

    return made.Value();
}

MixSnapshotRef Mix(Allocator& allocator, uint64 key, uint64 category, float volume,
                   float fadeIn = 0.0f, float duration = -1.0f, float fadeOut = 0.0f,
                   bool applyToChildren = false) noexcept {
    auto made = SharedPtr<MixSnapshotData>::TryMake(allocator);
    if (!made) {
        return {};
    }

    MixSnapshotData& data = *made.Value().Get();
    data.key = key;
    data.fadeInTime = fadeIn;
    data.duration = duration;
    data.fadeOutTime = fadeOut;
    data.adjusterCount = 1;
    data.adjusters[0].categoryKey = category;
    data.adjusters[0].volume = volume;
    data.adjusters[0].applyToChildren = applyToChildren;
    return made.Value();
}

float Peak(AudioEngine& fixture) noexcept {
    Array<float> chunk(fixture.allocator);
    if (!chunk.TryResize(static_cast<usize>(PumpFrames) * 2)) {
        return -1.0f;
    }

    return CapturePeak(fixture, chunk).left;
}

PlaybackHandle Play(AudioEngine& fixture, const SoundCategoryChainRef& chain,
                    PlayOptions options = {}) noexcept {
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
    options.category = chain;
    auto played = fixture.audio->Play2D(MakeSound(fixture.allocator, pcm, true), Move(options));
    return played ? played.Value() : PlaybackHandle{};
}

} // namespace

// 계층: volume은 사슬 곱이다(잎 0.5 × 루트 0.5).
TEST(EngineAudio_CategoryChainVolume) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(Play(fixture, Chain(fixture.allocator, {{LeafKey, 0.5f}, {RootKey, 0.5f}})).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.25f));
}

// 사용자 볼륨: audio.volume.music이 Music 분류(와 그 자손)에 곱해진다.
TEST(EngineAudio_CategoryUserVolume) {
    test::config::ConfigFixture settings(test::config::DevelopmentRules());
    config::ConfigContext& context = settings.Context();
    REQUIRE(RegisterEngineAudioSettings(context));
    REQUIRE(
        context.AddAssignment(config::ConfigLayer::Session, "test", "audio.volume.music", "0.5"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    AudioEngine fixture(DeterministicOptions(true), &context);
    REQUIRE(fixture.initialized);
    LinkSpec root{RootKey};
    root.userVolume = SoundUserVolume::Music;
    REQUIRE(Play(fixture, Chain(fixture.allocator, {{LeafKey}, root})).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
}

// 믹스: push하면(fade 0) 잎 조정이 곱해지고 pop하면(무한·fade-out 0) 돌아간다. applyToChildren이
// 꺼진 루트 조정은 잎에 닿지 않고, 켜면 닿는다.
TEST(EngineAudio_CategoryMixPushPop) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const SoundCategoryChainRef chain = Chain(fixture.allocator, {{LeafKey}, {RootKey}});
    REQUIRE(Play(fixture, chain).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    const MixSnapshotRef duck = Mix(fixture.allocator, MixKey, LeafKey, 0.5f);
    REQUIRE(fixture.audio->PushMix(duck));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
    CHECK(fixture.audio->ControlStats().activeMixes == 1);
    REQUIRE(fixture.audio->PopMix(duck));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel));
    CHECK(fixture.audio->ControlStats().activeMixes == 0);

    const MixSnapshotRef rootOnly = Mix(fixture.allocator, MixKey + 1, RootKey, 0.5f);
    REQUIRE(fixture.audio->PushMix(rootOnly));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel));
    REQUIRE(fixture.audio->PopMix(rootOnly));
    const MixSnapshotRef children =
        Mix(fixture.allocator, MixKey + 2, RootKey, 0.5f, 0.0f, -1.0f, 0.0f, true);
    REQUIRE(fixture.audio->PushMix(children));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
    // 빈 ref는 거부한다.
    CHECK(fixture.audio->PushMix(MixSnapshotRef{}).Error() == ErrorCode::InvalidArgument);
}

// 믹스 fade-in은 진폭 선형으로 1 → a를 지나고, duration이 끝나면 fade-out 뒤 사라진다.
TEST(EngineAudio_CategoryMixFadeAndDuration) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(Play(fixture, Chain(fixture.allocator, {{LeafKey}})).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    // fade-in 0.1 s(약 9.4 묶음), 지속 0.1 s, fade-out 0.1 s.
    const MixSnapshotRef mix = Mix(fixture.allocator, MixKey, LeafKey, 0.0f, 0.1f, 0.1f, 0.1f);
    REQUIRE(fixture.audio->PushMix(mix));
    bool between = false;
    bool reachedZero = false;
    for (uint32 frame = 0; frame < 24; ++frame) {
        const float peak = Peak(fixture);
        between = between || (peak > 0.01f && peak < DirectLevel - 0.01f);
        reachedZero = reachedZero || peak < 1.0e-4f;
    }

    CHECK(between);
    CHECK(reachedZero);
    REQUIRE(fixture.Frames(24));
    CHECK(Near(Peak(fixture), DirectLevel));
    CHECK(fixture.audio->ControlStats().activeMixes == 0);
}

// base 믹스는 pop·clear로 지워지지 않고, 다른 base로 바꾸면 이전 것이 끝난다.
TEST(EngineAudio_CategoryBaseMix) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(Play(fixture, Chain(fixture.allocator, {{LeafKey}})).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    const MixSnapshotRef base = Mix(fixture.allocator, MixKey, LeafKey, 0.5f);
    REQUIRE(fixture.audio->SetDefaultBaseMix(base));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
    REQUIRE(fixture.audio->PopMix(base));
    fixture.audio->ClearAllMixes();
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
    const MixSnapshotRef other = Mix(fixture.allocator, MixKey + 1, LeafKey, 0.25f);
    REQUIRE(fixture.audio->SetBaseMix(other));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel * 0.25f));
    CHECK(fixture.audio->ControlStats().activeMixes == 1);
}

// passive 믹스: 잎 분류의 재생이 임계 안이면 다음 묶음에 믹스가 걸려 다른 분류를 줄이고, 재생이
// 끝나면 풀린다.
TEST(EngineAudio_CategoryPassiveMix) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const MixSnapshotRef duck = Mix(fixture.allocator, MixKey, OtherKey, 0.5f);
    REQUIRE(Play(fixture, Chain(fixture.allocator, {{OtherKey}})).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel));
    const PlaybackHandle trigger =
        Play(fixture, Chain(fixture.allocator, {{LeafKey}}, duck), PlayOptions{});
    REQUIRE(trigger.IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    // 두 재생의 합: 트리거 0.1 + 줄어든 0.05.
    CHECK(Near(Peak(fixture), DirectLevel * 1.5f));
    CHECK(fixture.audio->ControlStats().activeMixes == 1);
    REQUIRE(fixture.audio->Stop(trigger, 0));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel));
    CHECK(fixture.audio->ControlStats().activeMixes == 0);
}

// always-play: StopLowestPriority(최대 1) 그룹에서 같은 우선순위면 오래된 쪽이 멈추지만, 오래된
// 쪽이 always-play면 P_c + 101이라 새 쪽이 멈춘다.
TEST(EngineAudio_CategoryAlwaysPlayConcurrency) {
    for (const bool always : {false, true}) {
        AudioEngine fixture(DeterministicOptions(true));
        REQUIRE(fixture.initialized);
        const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
        SoundPlayable playable;
        playable.pcm = pcm;
        playable.looping = true;
        playable.virtualizationMode = SoundVirtualizationMode::Disabled;
        playable.concurrencyCount = 1;
        playable.concurrency[0].key = 0x7001;
        playable.concurrency[0].settings.resolutionRule = SoundConcurrencyRule::StopLowestPriority;
        playable.concurrency[0].settings.maxCount = 1;
        auto sound = SoundSourceRef::TryCreate(fixture.allocator, playable);
        REQUIRE(sound);
        PlayOptions oldOptions;
        oldOptions.alwaysPlay = always;
        auto older = fixture.audio->Play2D(sound.Value(), Move(oldOptions));
        auto newer = fixture.audio->Play2D(sound.Value());
        REQUIRE(older && newer);
        REQUIRE(fixture.Frames(StartFrames));
        const PlaybackHandle stopped = always ? newer.Value() : older.Value();
        const PlaybackHandle kept = always ? older.Value() : newer.Value();
        CHECK(fixture.audio->Status(stopped) == PlaybackStatus::Finished);
        CHECK(fixture.audio->Status(kept) != PlaybackStatus::Finished);
    }
}

// 재생 필터: HPF 2 kHz는 상수(DC) 출력을 없앤다. 스테레오 정규화는 2채널 원본을 0.5배 한다.
TEST(EngineAudio_CategoryPlaybackFiltersAndStereo) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    PlayOptions filtered;
    filtered.highPassFrequency = 2000.0f;
    const PlaybackHandle hpf = Play(fixture, {}, Move(filtered));
    REQUIRE(hpf.IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Peak(fixture) < 1.0e-3f);
    REQUIRE(fixture.audio->SetParameter(hpf, PlaybackParameter::HighPassFrequency, 0.0f, 0));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel, 2.0e-3f));
    REQUIRE(fixture.audio->Stop(hpf, 0));
    REQUIRE(fixture.Frames(2));
    PlayOptions bad;
    bad.lowPassFrequency = 30000.0f;
    CHECK(fixture.audio
              ->Play2D(MakeSound(fixture.allocator,
                                 MakeConstantPcm(fixture.allocator, 0.2f, TestRate), true),
                       Move(bad))
              .Error() == ErrorCode::InvalidArgument);

    auto stereo = SoundPcm::TryAllocate(fixture.allocator, 2, TestRate, TestRate);
    REQUIRE(stereo);
    for (uint32 channel = 0; channel < 2; ++channel) {
        for (float& sample : stereo.Value()->MutableChannel(channel)) {
            sample = 0.2f;
        }
    }

    SoundPlayable playable;
    playable.pcm = SharedPtr<const SoundPcm>(Move(stereo.Value()));
    playable.looping = true;
    playable.normalizeStereo = true;
    auto sound = SoundSourceRef::TryCreate(fixture.allocator, playable);
    REQUIRE(sound);
    REQUIRE(fixture.audio->Play2D(sound.Value()));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), 0.1f));
}

// 에셋 사슬: 부모를 따라 루트까지, 키는 객체 키, 순환은 InvalidArgument다. 믹스 MakeRef는 조정
// 분류의 키를 싣고, override·거리 배수 API가 같은 키로 닿는다.
TEST(EngineAudio_CategoryAssetsAndOverride) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterAudioAssetsTypes(registration));
    ObjectRegistry objects(allocator, types, {0x45415544494f3043ull, 1});
    auto rootMade =
        objects.NewObject<SoundCategory>(objects.OwnerAccess(), TypeIdOf<SoundCategory>());
    auto leafMade =
        objects.NewObject<SoundCategory>(objects.OwnerAccess(), TypeIdOf<SoundCategory>());
    REQUIRE(rootMade && leafMade);
    const ObjectHandle rootHandle = rootMade.Value().Handle();
    const ObjectHandle leafHandle = leafMade.Value().Handle();
    {
        auto leafBorrow = objects.Resolve<SoundCategory>(objects.OwnerAccess(), leafHandle);
        REQUIRE(leafBorrow);
        SoundCategory& leaf = *leafBorrow.Value().Get().Value();
        REQUIRE(objects.SetReference(objects.OwnerAccess(), leafHandle, leaf.parent, rootHandle));
        leaf.volume = 0.5f;
        auto chain = leaf.MakeChain(allocator, objects, objects.OwnerAccess());
        REQUIRE(chain);
        CHECK(chain.Value()->depth == 2);
        CHECK(chain.Value()->links[0].key == AudioObjectKey(leafHandle));
        CHECK(chain.Value()->links[1].key == AudioObjectKey(rootHandle));
        CHECK(chain.Value()->links[0].properties.volume == 0.5f);
        leaf.volume = -1.0f;
        CHECK(leaf.MakeChain(allocator, objects, objects.OwnerAccess()).Error() ==
              ErrorCode::InvalidArgument);
        leaf.volume = 0.5f;
    }

    {
        // 루트가 잎을 부모로 가리키면 순환이다.
        auto rootBorrow = objects.Resolve<SoundCategory>(objects.OwnerAccess(), rootHandle);
        REQUIRE(rootBorrow);
        SoundCategory& root = *rootBorrow.Value().Get().Value();
        REQUIRE(objects.SetReference(objects.OwnerAccess(), rootHandle, root.parent, leafHandle));
        CHECK(root.MakeChain(allocator, objects, objects.OwnerAccess()).Error() ==
              ErrorCode::InvalidArgument);
        REQUIRE(
            objects.SetReference(objects.OwnerAccess(), rootHandle, root.parent, ObjectHandle{}));
    }

    // 깊이 초과: 루트 위로 MaxSoundCategoryDepth개를 더 이으면 CapacityExceeded다.
    {
        ObjectHandle top = rootHandle;
        for (uint32 index = 0; index < MaxSoundCategoryDepth; ++index) {
            auto made =
                objects.NewObject<SoundCategory>(objects.OwnerAccess(), TypeIdOf<SoundCategory>());
            REQUIRE(made);
            auto borrow = objects.Resolve<SoundCategory>(objects.OwnerAccess(), top);
            REQUIRE(borrow);
            REQUIRE(objects.SetReference(objects.OwnerAccess(), top,
                                         borrow.Value().Get().Value()->parent,
                                         made.Value().Handle()));
            top = made.Value().Handle();
        }

        auto leafBorrow = objects.Resolve<SoundCategory>(objects.OwnerAccess(), leafHandle);
        REQUIRE(leafBorrow);
        CHECK(leafBorrow.Value()
                  .Get()
                  .Value()
                  ->MakeChain(allocator, objects, objects.OwnerAccess())
                  .Error() == ErrorCode::CapacityExceeded);
        auto rootBorrow = objects.Resolve<SoundCategory>(objects.OwnerAccess(), rootHandle);
        REQUIRE(rootBorrow);
        REQUIRE(objects.SetReference(objects.OwnerAccess(), rootHandle,
                                     rootBorrow.Value().Get().Value()->parent, ObjectHandle{}));
    }

    // override: 활성 믹스의 분류 볼륨을 덮는다(믹스 조정 1인 분류 — 항목 없음 → 새 항목).
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    auto leafBorrow = objects.Resolve<SoundCategory>(objects.OwnerAccess(), leafHandle);
    REQUIRE(leafBorrow);
    const SoundCategory& leaf = *leafBorrow.Value().Get().Value();
    auto chain = leaf.MakeChain(fixture.allocator, objects, objects.OwnerAccess());
    REQUIRE(chain);
    REQUIRE(Play(fixture, chain.Value()).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
    const MixSnapshotRef user = Mix(fixture.allocator, MixKey, OtherKey, 1.0f);
    REQUIRE(fixture.audio->SetMixCategoryOverride(user, leaf, 0.5f, 1.0f, 0.0f, false));
    REQUIRE(fixture.Frames(2));
    // 믹스가 활성이 아니면 저장만 한다.
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
    REQUIRE(fixture.audio->PushMix(user));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel * 0.25f));
    REQUIRE(fixture.audio->ClearMixCategoryOverride(user, leaf, 0.0f));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
}

// 분류 감쇠 거리 배수: 잎 분류에 배수 2를 걸면 감쇠 거리가 두 배로 잰다(감쇠 gain에만).
TEST(EngineAudio_CategoryDistanceScale) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterAudioAssetsTypes(registration));
    ObjectRegistry objects(allocator, types, {0x45415544494f3044ull, 1});
    auto made = objects.NewObject<SoundCategory>(objects.OwnerAccess(), TypeIdOf<SoundCategory>());
    REQUIRE(made);
    auto borrow = objects.Resolve<SoundCategory>(objects.OwnerAccess(), made.Value().Handle());
    REQUIRE(borrow);
    const SoundCategory& category = *borrow.Value().Get().Value();

    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    auto chain = category.MakeChain(fixture.allocator, objects, objects.OwnerAccess());
    REQUIRE(chain);
    const AudioSceneId scene = fixture.audio->RegisterScene();
    SoundAttenuation attenuation;
    attenuation.spatialize = false;
    attenuation.shapeExtents.x = 0.0f;
    attenuation.distance.falloffDistance = 1000.0f;
    const SharedPtr<const SoundPcm> pcm = MakeConstantPcm(fixture.allocator, 0.2f, TestRate);
    PlayOptions options;
    options.category = chain.Value();
    REQUIRE(fixture.audio->PlayAtInScene(scene,
                                         MakeSound(fixture.allocator, pcm, true, 0, 0, attenuation),
                                         Vec3d(250.0, 0.0, 0.0), Move(options)));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.75f));
    REQUIRE(fixture.audio->SetCategoryDistanceScale(category, 2.0f, 0.0f));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
    CHECK(fixture.audio->SetCategoryDistanceScale(category, -1.0f, 0.0f).Error() ==
          ErrorCode::InvalidArgument);
}

// 같은 묶음에서 무한 믹스를 pop한 뒤 push하면 그대로 남는다(UE UpdateSoundMix가 잡힌 fade-out을
// 거둔다).
TEST(EngineAudio_CategoryMixPopPushSameBatch) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(Play(fixture, Chain(fixture.allocator, {{LeafKey}})).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    const MixSnapshotRef mix = Mix(fixture.allocator, MixKey, LeafKey, 0.5f, 0.0f, -1.0f, 0.1f);
    REQUIRE(fixture.audio->PushMix(mix));
    REQUIRE(fixture.Frames(2));
    REQUIRE(fixture.audio->PopMix(mix));
    REQUIRE(fixture.audio->PushMix(mix));
    REQUIRE(fixture.Frames(24));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
    CHECK(fixture.audio->ControlStats().activeMixes == 1);
}

// fade-in 중 pop하면 지금 레벨에서 이어 내려간다(레벨이 튀지 않는다 — UE AD 2853–2856). 조정 0인
// 믹스라 출력은 fade-in 동안 줄고, pop 뒤에는 늘기만 해 원래로 돌아온다.
TEST(EngineAudio_CategoryMixPopDuringFadeIn) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(Play(fixture, Chain(fixture.allocator, {{LeafKey}})).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    const MixSnapshotRef mix = Mix(fixture.allocator, MixKey, LeafKey, 0.0f, 0.2f, -1.0f, 0.2f);
    REQUIRE(fixture.audio->PushMix(mix));
    REQUIRE(fixture.Frames(9));
    const float atPop = Peak(fixture);
    CHECK(atPop > 0.02f && atPop < 0.08f);
    REQUIRE(fixture.audio->PopMix(mix));
    // pop이 닿기 전 묶음과 이미 걸린 gain ramp만큼(묶음 둘 — 묶음마다 약 0.0053) 더 내려간 뒤
    // 늘기만 한다. 0으로 떨어지거나 튀지 않는다.
    constexpr float FadeStepPerBatch = 0.0055f;
    float lowest = atPop;
    float previous = atPop;
    for (uint32 frame = 0; frame < 30; ++frame) {
        const float peak = Peak(fixture);
        lowest = math::Min(lowest, peak);
        if (frame >= 2) {
            CHECK(peak >= previous - 1.0e-4f);
        }

        previous = peak;
    }

    CHECK(lowest >= atPop - 2.0f * FadeStepPerBatch);
    CHECK(Near(previous, DirectLevel));
    CHECK(fixture.audio->ControlStats().activeMixes == 0);
}

// 지연 중 다시 걸어도 지연을 다시 시작하지 않는다(UE는 FadeInStartTime을 그대로 둔다).
TEST(EngineAudio_CategoryMixRetriggerDuringDelay) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(Play(fixture, Chain(fixture.allocator, {{LeafKey}})).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    auto made = SharedPtr<MixSnapshotData>::TryMake(fixture.allocator);
    REQUIRE(made);
    MixSnapshotData& data = *made.Value().Get();
    data.key = MixKey;
    data.initialDelay = 0.05f;
    data.fadeInTime = 0.0f;
    data.fadeOutTime = 0.0f;
    data.adjusterCount = 1;
    data.adjusters[0].categoryKey = LeafKey;
    data.adjusters[0].volume = 0.5f;
    const MixSnapshotRef delayed = made.Value();
    for (uint32 frame = 0; frame < 8; ++frame) {
        REQUIRE(fixture.audio->PushMix(delayed));
        REQUIRE(fixture.Frame());
    }

    CHECK(Near(Peak(fixture), DirectLevel * 0.5f));
}

// 여러 믹스는 곱한다.
TEST(EngineAudio_CategoryMixesMultiply) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    REQUIRE(Play(fixture, Chain(fixture.allocator, {{LeafKey}})).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    REQUIRE(fixture.audio->PushMix(Mix(fixture.allocator, MixKey, LeafKey, 0.5f)));
    REQUIRE(fixture.audio->PushMix(Mix(fixture.allocator, MixKey + 1, LeafKey, 0.5f)));
    REQUIRE(fixture.Frames(2));
    CHECK(Near(Peak(fixture), DirectLevel * 0.25f));
    CHECK(fixture.audio->ControlStats().activeMixes == 2);
}

// passive 대상에서 정지 요청 중인 재생은 뺀다(긴 정지 fade 동안에도 믹스가 풀린다).
TEST(EngineAudio_CategoryPassiveSkipsStopping) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const MixSnapshotRef duck = Mix(fixture.allocator, MixKey, OtherKey, 0.5f);
    const PlaybackHandle trigger = Play(fixture, Chain(fixture.allocator, {{LeafKey}}, duck));
    REQUIRE(trigger.IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(fixture.audio->ControlStats().activeMixes == 1);
    REQUIRE(fixture.audio->Stop(trigger, TestRate));
    REQUIRE(fixture.Frames(3));
    CHECK(fixture.audio->ControlStats().activeMixes == 0);
}

} // namespace audio_category_tests_file
