#include "AudioEngineTestSupport.h"

#include <Audio/AudioMixerTypes.h>
#include <Audio/Decibels.h>
#include <AudioAssets/AttenuationSettings.generated.h>
#include <AudioAssets/AudioAssetsTypes.h>
#include <AudioAssets/AudioObjectKey.h>
#include <AudioAssets/MixBus.generated.h>
#include <AudioAssets/MixBusSend.generated.h>
#include <AudioAssets/SoundCategory.generated.h>
#include <Core/Math/MathUtility.h>
#include <Base/InitializerList.h>
#include <Config/Tests/ConfigTestSupport.h>
#include <Core/Module/ModuleEntry.h>
#include <EngineAudio/AudioEngineSettings.h>
#include <Object/ObjectRegistry.h>
#include <TestSupport/Test.h>

// AU7f2 버스 시험이다(Docs/Design/AudioSystem 3.45.7–3.45.9·3.45.11절).
// - 에셋: MixBus 사슬(키·선형 값·순환·깊이·범위·생성 번호), send level 평가(Constant 자르기·거리
//   선형), 분류 기본 버스·감쇠 send 스냅숏(Pre는 post로).
// - 엔진(Inline 제어 + Manual 렌더): 기본 버스 순서, send 합치기·재생 단위 send, Master·실행 배수,
//   plan 재시도·버스 수명, 칸 부족 대체, 스냅숏 사이 부모 순환. 출력은 mono 상수 0.2의
//   Direct(0.5배) = 0.1 기준이다.
using namespace rocks;
namespace audio_bus_tests_file {

using namespace rocks::engine_audio_test;

namespace {

constexpr float Tolerance = 1.0e-5f;
constexpr float LevelTolerance = 1.0e-4f;
constexpr uint32 StartFrames = 6;
constexpr float DirectLevel = 0.1f;
constexpr float SourceValue = 0.2f;
constexpr uint32 RetryPlaybacks = 3;

// 시험 버스·분류 키다(객체 없이 사슬을 만든다).
constexpr uint64 SoundBusKey = 0xB001;
constexpr uint64 CategoryBusKey = 0xB002;
constexpr uint64 ParentCategoryBusKey = 0xB003;
constexpr uint64 DefaultBusKey = 0xB004;
constexpr uint64 SendBusKey = 0xB010;
constexpr uint64 ExtraBusKey = 0xB011;
constexpr uint64 DistanceBusKey = 0xB012;
constexpr uint64 MasterAssetKey = 0xB020;
constexpr uint64 ChildBusKey = 0xB030;
constexpr uint64 SwitchBusKey = 0xB031;
constexpr uint64 OverflowBusBase = 0xB100;
constexpr uint64 LeafCategoryKey = 0xC001;
constexpr uint64 RootCategoryKey = 0xC002;

bool Near(float actual, float expected, float tolerance = Tolerance) noexcept {
    return math::Abs(actual - expected) <= tolerance;
}

// 에셋 Registry 시험대다. 객체는 Borrow를 쥔 범위 안에서만 고친다(참조 쓰기 규칙).
struct AssetRig {
    explicit AssetRig(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator),
          registered(static_cast<bool>(RegisterAudioAssetsTypes(registration))),
          objects(allocator, types, {0x4255535445535431ull, 1}) {
    }

    template <class T>
    ObjectHandle Make() noexcept {
        auto made = objects.NewObject<T>(objects.OwnerAccess(), TypeIdOf<T>());
        return made ? made.Value().Handle() : ObjectHandle{};
    }

    TypeRegistry types;
    ModuleRegistration registration;
    bool registered = false;
    ObjectRegistry objects;
};

struct BusSpec {
    uint64 key = 0;
    float output = 1.0f;
};

// links[0]이 그 버스인 사슬이다(뒤가 부모).
MixBusRef Bus(Allocator& allocator, std::initializer_list<BusSpec> links) noexcept {
    auto made = SharedPtr<MixBusChain>::TryMake(allocator);
    if (!made) {
        return {};
    }

    MixBusChain& chain = *made.Value().Get();
    for (const BusSpec& spec : links) {
        MixBusLinkValue& link = chain.links[chain.depth++];
        link.key = spec.key;
        link.outputVolume = spec.output;
    }

    chain.serial = NextMixBusSerial();
    return made.Value();
}

MixBusSendValue Send(const MixBusRef& bus, float level) noexcept {
    MixBusSendValue send;
    send.bus = bus;
    send.level = level;
    return send;
}

struct SoundSpec {
    MixBusRef bus;
    bool enableBaseBus = true;
    bool enableBusSends = true;
    uint32 sendCount = 0;
    MixBusSendValue sends[MaxMixBusSends];
};

SoundSourceRef BusSound(AudioEngine& fixture, const SoundSpec& spec) noexcept {
    SoundPlayable playable;
    playable.pcm = MakeConstantPcm(fixture.allocator, SourceValue, TestRate);
    playable.looping = true;
    playable.bus = spec.bus;
    playable.enableBaseBus = spec.enableBaseBus;
    playable.enableBusSends = spec.enableBusSends;
    playable.busSendCount = spec.sendCount;
    for (uint32 index = 0; index < spec.sendCount; ++index) {
        playable.busSends[index] = spec.sends[index];
    }

    auto made = SoundSourceRef::TryCreate(fixture.allocator, playable);
    return made ? made.Value() : SoundSourceRef{};
}

PlaybackHandle PlayBus(AudioEngine& fixture, const SoundSpec& spec,
                       PlayOptions options = {}) noexcept {
    auto played = fixture.audio->Play2D(BusSound(fixture, spec), Move(options));
    return played ? played.Value() : PlaybackHandle{};
}

float Peak(AudioEngine& fixture) noexcept {
    Array<float> chunk(fixture.allocator);
    if (!chunk.TryResize(static_cast<usize>(PumpFrames) * 2)) {
        return -1.0f;
    }

    return CapturePeak(fixture, chunk).left;
}

// 소리를 내고 값을 잰 뒤 멈추고, 제어 쪽이 모두 놓을 때까지 돈다.
float Measure(AudioEngine& fixture, const SoundSpec& spec, PlayOptions options = {}) noexcept {
    const PlaybackHandle handle = PlayBus(fixture, spec, Move(options));
    if (!handle.IsValid() || !fixture.Frames(StartFrames)) {
        return -1.0f;
    }

    const float peak = Peak(fixture);
    (void)fixture.audio->Stop(handle, 0);
    return fixture.RunUntilIdle() ? peak : -1.0f;
}

SoundCategoryChainRef CategoryWithBuses(Allocator& allocator, const MixBusRef& leafBus,
                                        const MixBusRef& rootBus) noexcept {
    auto made = SharedPtr<SoundCategoryChain>::TryMake(allocator);
    if (!made) {
        return {};
    }

    SoundCategoryChain& chain = *made.Value().Get();
    chain.links[0].key = LeafCategoryKey;
    chain.links[0].defaultBus = leafBus;
    chain.links[1].key = RootCategoryKey;
    chain.links[1].defaultBus = rootBus;
    chain.depth = 2;
    return made.Value();
}

} // namespace

// MixBus 사슬: 잎부터 루트까지, 키는 객체 키, dB는 선형, 순환·범위 밖은 InvalidArgument, 깊이
// 초과는 CapacityExceeded, 생성 번호는 늘어난다.
TEST(EngineAudio_BusAssetChain) {
    SystemAllocator allocator;
    AssetRig rig(allocator);
    REQUIRE(rig.registered);
    const ObjectAccessContext& access = rig.objects.OwnerAccess();
    const ObjectHandle rootHandle = rig.Make<MixBus>();
    const ObjectHandle leafHandle = rig.Make<MixBus>();
    REQUIRE(!rootHandle.IsNull() && !leafHandle.IsNull());
    {
        auto leafBorrow = rig.objects.Resolve<MixBus>(access, leafHandle);
        REQUIRE(leafBorrow);
        MixBus& leaf = *leafBorrow.Value().Get().Value();
        REQUIRE(rig.objects.SetReference(access, leafHandle, leaf.parent, rootHandle));
        leaf.outputVolumeDb = -6.0f;
        leaf.muteWhenBackgrounded = true;
        auto first = leaf.MakeRef(allocator, rig.objects, access);
        REQUIRE(first);
        const MixBusChain& chain = *first.Value().Get();
        CHECK(chain.depth == 2);
        CHECK(chain.Key() == AudioObjectKey(leafHandle));
        CHECK(chain.links[1].key == AudioObjectKey(rootHandle));
        CHECK(Near(chain.links[0].outputVolume, audio::DecibelsToLinear(-6.0f)));
        CHECK(Near(chain.links[0].dryLevel, audio::DecibelsToLinear(DefaultMixBusDryLevelDb)));
        CHECK(chain.links[0].muteWhenBackgrounded && !chain.links[1].muteWhenBackgrounded);
        auto second = leaf.MakeRef(allocator, rig.objects, access);
        REQUIRE(second);
        CHECK(second.Value()->serial > chain.serial);
        leaf.outputVolumeDb = 1.0f;
        CHECK(leaf.MakeRef(allocator, rig.objects, access).Error() == ErrorCode::InvalidArgument);
        leaf.outputVolumeDb = 0.0f;
    }

    // 루트가 잎을 부모로 가리키면 순환이다.
    {
        auto rootBorrow = rig.objects.Resolve<MixBus>(access, rootHandle);
        REQUIRE(rootBorrow);
        MixBus& root = *rootBorrow.Value().Get().Value();
        REQUIRE(rig.objects.SetReference(access, rootHandle, root.parent, leafHandle));
        CHECK(root.MakeRef(allocator, rig.objects, access).Error() == ErrorCode::InvalidArgument);
        REQUIRE(rig.objects.SetReference(access, rootHandle, root.parent, ObjectHandle{}));
    }

    // 깊이 초과: 루트 위로 MaxMixBusDepth개를 더 이으면 CapacityExceeded다.
    ObjectHandle top = rootHandle;
    for (uint32 index = 0; index < MaxMixBusDepth; ++index) {
        const ObjectHandle made = rig.Make<MixBus>();
        REQUIRE(!made.IsNull());
        auto borrow = rig.objects.Resolve<MixBus>(access, top);
        REQUIRE(borrow);
        REQUIRE(rig.objects.SetReference(access, top, borrow.Value().Get().Value()->parent, made));
        top = made;
    }

    auto leafBorrow = rig.objects.Resolve<MixBus>(access, leafHandle);
    REQUIRE(leafBorrow);
    CHECK(leafBorrow.Value().Get().Value()->MakeRef(allocator, rig.objects, access).Error() ==
          ErrorCode::CapacityExceeded);
}

// send level(UE UpdateSubmixSendLevels): Constant는 [0, 1](자르기 끄면 믹서 상한), 거리 선형은
// 정규화 거리로 lerp한 뒤 [0, 1], 거리 범위 분모는 1 이상이다.
TEST(EngineAudio_BusSendLevelEvaluation) {
    MixBusSendValue send;
    send.level = 2.0f;
    CHECK(EvaluateMixBusSendLevel(send, 0.0f, audio::MaxVoiceGain) == 1.0f);
    send.disableLevelClamp = true;
    CHECK(EvaluateMixBusSendLevel(send, 0.0f, audio::MaxVoiceGain) == 2.0f);
    send.level = audio::MaxVoiceGain * 2.0f;
    CHECK(EvaluateMixBusSendLevel(send, 0.0f, audio::MaxVoiceGain) == audio::MaxVoiceGain);

    send.levelMethod = MixBusSendLevelMethod::DistanceLinear;
    send.minLevel = 0.2f;
    send.maxLevel = 1.0f;
    send.minDistance = 100.0f;
    send.maxDistance = 500.0f;
    CHECK(Near(EvaluateMixBusSendLevel(send, 0.0f, audio::MaxVoiceGain), 0.2f));
    CHECK(Near(EvaluateMixBusSendLevel(send, 300.0f, audio::MaxVoiceGain), 0.6f));
    CHECK(Near(EvaluateMixBusSendLevel(send, 9000.0f, audio::MaxVoiceGain), 1.0f));
    send.maxLevel = 3.0f;
    CHECK(EvaluateMixBusSendLevel(send, 9000.0f, audio::MaxVoiceGain) == 1.0f);
    // 거리 범위가 0이면 분모는 1이다(UE max(Max − Min, 1)).
    send.maxLevel = 1.0f;
    send.maxDistance = send.minDistance;
    CHECK(Near(EvaluateMixBusSendLevel(send, 100.5f, audio::MaxVoiceGain), 0.2f + 0.8f * 0.5f));
}

// 분류의 기본 버스는 그 분류 칸에만 실리고, 감쇠 send는 Pre여도 post로 평탄화된다.
TEST(EngineAudio_BusAssetSnapshots) {
    SystemAllocator allocator;
    AssetRig rig(allocator);
    REQUIRE(rig.registered);
    const ObjectAccessContext& access = rig.objects.OwnerAccess();
    const ObjectHandle busHandle = rig.Make<MixBus>();
    const ObjectHandle parentHandle = rig.Make<SoundCategory>();
    const ObjectHandle childHandle = rig.Make<SoundCategory>();
    const ObjectHandle settingsHandle = rig.Make<AttenuationSettings>();
    REQUIRE(!busHandle.IsNull() && !parentHandle.IsNull() && !childHandle.IsNull() &&
            !settingsHandle.IsNull());
    {
        auto parentBorrow = rig.objects.Resolve<SoundCategory>(access, parentHandle);
        REQUIRE(parentBorrow);
        SoundCategory& parent = *parentBorrow.Value().Get().Value();
        REQUIRE(rig.objects.SetReference(access, parentHandle, parent.defaultBus, busHandle));
    }

    {
        auto childBorrow = rig.objects.Resolve<SoundCategory>(access, childHandle);
        REQUIRE(childBorrow);
        SoundCategory& child = *childBorrow.Value().Get().Value();
        REQUIRE(rig.objects.SetReference(access, childHandle, child.parent, parentHandle));
        auto chain = child.MakeChain(allocator, rig.objects, access);
        REQUIRE(chain);
        CHECK(!chain.Value()->links[0].defaultBus);
        REQUIRE(chain.Value()->links[1].defaultBus);
        CHECK(chain.Value()->links[1].defaultBus->Key() == AudioObjectKey(busHandle));
    }

    auto settingsBorrow = rig.objects.Resolve<AttenuationSettings>(access, settingsHandle);
    REQUIRE(settingsBorrow);
    AttenuationSettings& settings = *settingsBorrow.Value().Get().Value();
    settings.enableBusSends = true;
    auto index = rig.objects.AddElement(access, settingsHandle, settings.busSends);
    REQUIRE(index);
    MixBusSend& send = settings.busSends[index.Value()];
    REQUIRE(rig.objects.SetElementReference(access, settingsHandle, settings.busSends,
                                            index.Value(), send.bus, busHandle));
    send.stage = MixBusSendStage::PreAttenuation;
    send.lowPassEnabled = true;
    send.lowPassHz = 500.0f;
    auto snapshot = settings.ToSnapshot(allocator, rig.objects, access);
    REQUIRE(snapshot);
    REQUIRE(snapshot.Value().busSendCount == 1);
    CHECK(snapshot.Value().busSends[0].stage == MixBusSendStage::PostAttenuation);
    CHECK(snapshot.Value().busSends[0].lowPassEnabled);
    CHECK(snapshot.Value().busSends[0].bus->Key() == AudioObjectKey(busHandle));
    send.lowPassHz = 5.0f;
    CHECK(settings.ToSnapshot(allocator, rig.objects, access).Error() ==
          ErrorCode::InvalidArgument);
}

// 기본 버스 순서: 사운드 bus > 자기 분류의 defaultBus > BaseDefault > Master. 부모 분류의
// defaultBus는 쓰지 않는다(UE). 쓰지 않게 된 버스는 plan에서 빠진다.
TEST(EngineAudio_BusBaseResolution) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const MixBusRef soundBus = Bus(fixture.allocator, {{SoundBusKey, 0.5f}});
    const MixBusRef categoryBus = Bus(fixture.allocator, {{CategoryBusKey, 0.25f}});
    const MixBusRef parentBus = Bus(fixture.allocator, {{ParentCategoryBusKey, 0.75f}});
    const MixBusRef defaultBus = Bus(fixture.allocator, {{DefaultBusKey, 0.125f}});
    REQUIRE(fixture.audio->SetBaseDefaultBus(defaultBus));
    PlayOptions withCategory;
    withCategory.category = CategoryWithBuses(fixture.allocator, categoryBus, parentBus);
    SoundSpec spec;
    spec.bus = soundBus;
    CHECK(Near(Measure(fixture, spec, Move(withCategory)), DirectLevel * 0.5f, LevelTolerance));
    spec.bus = MixBusRef{};
    withCategory.category = CategoryWithBuses(fixture.allocator, categoryBus, parentBus);
    CHECK(Near(Measure(fixture, spec, Move(withCategory)), DirectLevel * 0.25f, LevelTolerance));
    PlayOptions parentOnly;
    parentOnly.category = CategoryWithBuses(fixture.allocator, MixBusRef{}, parentBus);
    CHECK(Near(Measure(fixture, spec, Move(parentOnly)), DirectLevel * 0.125f, LevelTolerance));
    REQUIRE(fixture.audio->SetBaseDefaultBus(MixBusRef{}));
    CHECK(Near(Measure(fixture, spec), DirectLevel, LevelTolerance));
    REQUIRE(fixture.Frames(2));
    CHECK(fixture.audio->ControlStats().liveBuses == 0);
}

// send 합치기: 기본 버스(Master) + 사운드 send. 재생 단위 send는 같은 버스면 level만, 아니면
// 덧붙인다. 거리 방법 send의 level은 override가 바꾸지 않는다(UE). 사운드 enableBusSends를 끄면
// 재생 단위 send도 꺼진다.
TEST(EngineAudio_BusSendsAndOverrides) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const MixBusRef sendBus = Bus(fixture.allocator, {{SendBusKey, 1.0f}});
    const MixBusRef extraBus = Bus(fixture.allocator, {{ExtraBusKey, 1.0f}});
    const MixBusRef distanceBus = Bus(fixture.allocator, {{DistanceBusKey, 1.0f}});
    SoundSpec spec;
    spec.sendCount = 2;
    spec.sends[0] = Send(sendBus, 0.5f);
    spec.sends[1] = Send(distanceBus, 1.0f);
    spec.sends[1].levelMethod = MixBusSendLevelMethod::DistanceLinear;
    spec.sends[1].minLevel = 0.3f;
    spec.sends[1].maxLevel = 1.0f;
    spec.sends[1].minDistance = 0.0f;
    spec.sends[1].maxDistance = 1000.0f;
    const PlaybackHandle handle = PlayBus(fixture, spec);
    REQUIRE(handle.IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    // 위치 없는 재생은 거리 0이라 거리 send는 minLevel이다.
    CHECK(Near(Peak(fixture), DirectLevel * (1.0f + 0.5f + 0.3f), LevelTolerance));
    CHECK(fixture.audio->ControlStats().liveBuses == 2);
    REQUIRE(fixture.audio->SetBusSend(handle, sendBus, 1.0f));
    REQUIRE(fixture.audio->SetBusSend(handle, distanceBus, 1.0f));
    REQUIRE(fixture.audio->SetBusSend(handle, extraBus, 0.5f));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * (1.0f + 1.0f + 0.3f + 0.5f), LevelTolerance));
    REQUIRE(fixture.audio->Stop(handle, 0));
    REQUIRE(fixture.RunUntilIdle());

    SoundSpec quiet = spec;
    quiet.enableBusSends = false;
    PlayOptions options;
    options.busSendCount = 1;
    options.busSends[0] = PlayBusSend{extraBus, 1.0f};
    CHECK(Near(Measure(fixture, quiet, Move(options)), DirectLevel, LevelTolerance));
    quiet.enableBaseBus = false;
    CHECK(Near(Measure(fixture, quiet), 0.0f, LevelTolerance));
}

// 실행 배수는 버스가 생기기 전에 걸어도 남는다. Master 에셋은 Master 값을 정하고, 그 키를 가리키는
// 사운드 bus는 Master로 간다(새 칸을 쓰지 않는다).
TEST(EngineAudio_BusMasterAndModifiers) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    const MixBusRef sendBus = Bus(fixture.allocator, {{SendBusKey, 1.0f}});
    const MixBusRef master = Bus(fixture.allocator, {{MasterAssetKey, 0.5f}});
    REQUIRE(fixture.audio->SetBusOutputVolume(sendBus, 0.5f));
    CHECK(fixture.audio->SetBusOutputVolume(sendBus, -1.0f).Error() == ErrorCode::InvalidArgument);
    SoundSpec spec;
    spec.bus = sendBus;
    const PlaybackHandle handle = PlayBus(fixture, spec);
    REQUIRE(handle.IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f, LevelTolerance));
    REQUIRE(fixture.audio->SetMasterBus(master));
    CHECK(fixture.audio->SetMasterBus(Bus(fixture.allocator, {{ExtraBusKey}, {SendBusKey}}))
              .Error() == ErrorCode::InvalidArgument);
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.25f, LevelTolerance));
    REQUIRE(fixture.audio->Stop(handle, 0));
    REQUIRE(fixture.RunUntilIdle());
    SoundSpec toMaster;
    toMaster.bus = master;
    const PlaybackHandle second = PlayBus(fixture, toMaster);
    REQUIRE(second.IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(fixture.audio->ControlStats().liveBuses == 0);
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f, LevelTolerance));
}

// 한 묶음에 새 버스 셋을 여는 재생 셋: 미채택 plan이 둘이 되면 셋째는 다음 묶음에 시작한다. 모두
// 시작하고, 끝나면 버스가 빠진다.
TEST(EngineAudio_BusPlanRetryAndLifetime) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    PlaybackHandle handles[RetryPlaybacks] = {};
    const uint64 keys[RetryPlaybacks] = {SoundBusKey, CategoryBusKey, DefaultBusKey};
    for (uint32 index = 0; index < RetryPlaybacks; ++index) {
        SoundSpec spec;
        spec.bus = Bus(fixture.allocator, {{keys[index], 1.0f}});
        handles[index] = PlayBus(fixture, spec);
        REQUIRE(handles[index].IsValid());
    }

    REQUIRE(fixture.engine.RunFrame());
    CHECK(fixture.audio->ControlStats().busPlanRetries >= 1);
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(fixture.audio->ControlStats().liveBuses == RetryPlaybacks);
    CHECK(Near(Peak(fixture), DirectLevel * static_cast<float>(RetryPlaybacks), LevelTolerance));
    for (const PlaybackHandle handle : handles) {
        REQUIRE(fixture.audio->Stop(handle, 0));
    }

    REQUIRE(fixture.RunUntilIdle());
    REQUIRE(fixture.Frames(2));
    CHECK(fixture.audio->ControlStats().liveBuses == 0);
}

// 칸 부족: Master와 버스 하나뿐이면 둘째 버스는 Master로 간다(Warning·계수).
TEST(EngineAudio_BusCapacityFallback) {
    test::config::ConfigFixture settings(test::config::DevelopmentRules());
    config::ConfigContext& context = settings.Context();
    REQUIRE(RegisterEngineAudioSettings(context));
    REQUIRE(
        context.AddAssignment(config::ConfigLayer::Session, "test", "audio.mixer.maxbuses", "2"));
    REQUIRE(context.Sync());
    REQUIRE(context.CompleteStartup());
    AudioEngine fixture(DeterministicOptions(true), &context);
    REQUIRE(fixture.initialized);
    SoundSpec first;
    first.bus = Bus(fixture.allocator, {{SoundBusKey, 0.5f}});
    SoundSpec second;
    second.bus = Bus(fixture.allocator, {{CategoryBusKey, 0.5f}});
    REQUIRE(PlayBus(fixture, first).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    REQUIRE(PlayBus(fixture, second).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(fixture.audio->ControlStats().busCapacityFallbacks >= 1);
    CHECK(fixture.audio->ControlStats().liveBuses == 1);
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f + DirectLevel, LevelTolerance));
}

// 더 새 스냅숏이 부모를 바꾼다: X(부모 Y) 뒤에 더 새 Y(부모 X, X는 루트)가 오면 X가 루트가 되고 Y가
// X 밑으로 간다. 사슬마다 조상 전체를 싣고 루트부터 등록하므로 순환이 생기지 않는다.
TEST(EngineAudio_BusNewerSnapshotReparents) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    SoundSpec first;
    first.bus = Bus(fixture.allocator, {{SoundBusKey, 1.0f}, {CategoryBusKey, 1.0f}});
    SoundSpec second;
    second.bus = Bus(fixture.allocator, {{CategoryBusKey, 1.0f}, {SoundBusKey, 1.0f}});
    REQUIRE(PlayBus(fixture, first).IsValid());
    REQUIRE(fixture.Frames(2));
    REQUIRE(PlayBus(fixture, second).IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(fixture.audio->ControlStats().busParentCycles == 0);
    CHECK(fixture.audio->ControlStats().liveBuses == 2);
    CHECK(Near(Peak(fixture), DirectLevel * 2.0f, LevelTolerance));
}

// Master 교체: Live 버스 K를 Master로 바꾸면 그 자식 C가 바로 Master 밑으로 옮겨 K 값이 두 번
// 곱해지지 않고 K 칸이 빠진다. 되돌리면 C는 새 K 칸 밑으로 돌아간다.
TEST(EngineAudio_BusMasterSwitchMovesChildren) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    SoundSpec spec;
    spec.bus = Bus(fixture.allocator, {{ChildBusKey, 1.0f}, {SwitchBusKey, 0.5f}});
    const PlaybackHandle handle = PlayBus(fixture, spec);
    REQUIRE(handle.IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f, LevelTolerance));
    CHECK(fixture.audio->ControlStats().liveBuses == 2);

    REQUIRE(fixture.audio->SetMasterBus(Bus(fixture.allocator, {{SwitchBusKey, 0.5f}})));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f, LevelTolerance));
    CHECK(fixture.audio->ControlStats().liveBuses == 1);

    REQUIRE(fixture.audio->SetMasterBus(MixBusRef{}));
    REQUIRE(fixture.Frames(StartFrames));
    CHECK(Near(Peak(fixture), DirectLevel * 0.5f, LevelTolerance));
    CHECK(fixture.audio->ControlStats().liveBuses == 2);
}

// send 수 초과: 기본 버스 + send 8개면 마지막 send를 버리고 재생마다 한 번 센다. 끝난 재생의
// SetBusSend는 아무 일도 하지 않는다(Success).
TEST(EngineAudio_BusSendOverflowAndEndedPlayback) {
    AudioEngine fixture(DeterministicOptions(true));
    REQUIRE(fixture.initialized);
    constexpr float SendLevel = 0.1f;
    SoundSpec spec;
    spec.sendCount = MaxMixBusSends;
    for (uint32 index = 0; index < MaxMixBusSends; ++index) {
        spec.sends[index] =
            Send(Bus(fixture.allocator, {{OverflowBusBase + index, 1.0f}}), SendLevel);
    }

    const PlaybackHandle handle = PlayBus(fixture, spec);
    REQUIRE(handle.IsValid());
    REQUIRE(fixture.Frames(StartFrames));
    const float kept = static_cast<float>(audio::MaxVoiceSends - 1) * SendLevel;
    CHECK(Near(Peak(fixture), DirectLevel * (1.0f + kept), LevelTolerance));
    CHECK(fixture.audio->ControlStats().droppedBusSends == 1);
    CHECK(fixture.audio->ControlStats().liveBuses == audio::MaxVoiceSends - 1);
    REQUIRE(fixture.audio->Stop(handle, 0));
    REQUIRE(fixture.RunUntilIdle());
    CHECK(fixture.audio->SetBusSend(handle, spec.sends[0].bus, 1.0f).IsOk());
    CHECK(fixture.audio->SetBusSend(handle, MixBusRef{}, 1.0f).Error() ==
          ErrorCode::InvalidArgument);
}

} // namespace audio_bus_tests_file
