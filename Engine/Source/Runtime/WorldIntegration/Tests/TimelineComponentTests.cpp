#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAssets/FloatCurveAsset.generated.h>
#include <CurveAssets/VectorCurveAsset.generated.h>
#include <Meta/TypeSchema.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/World.h>
#include <WorldIntegration/Components/TimelineColorTrack.generated.h>
#include <WorldIntegration/Components/TimelineComponent.generated.h>
#include <WorldIntegration/Components/TimelineComponent.h>
#include <WorldIntegration/Components/TimelineEventKey.generated.h>
#include <WorldIntegration/Components/TimelineFloatTrack.generated.h>
#include <WorldIntegration/Components/TimelineVectorTrack.generated.h>
#include <WorldIntegration/WorldIntegrationTypes.h>
#include <math.h>
#include <stdio.h>

// 타임라인 컴포넌트 시험이다(Docs/Design/EngineRuntime 32.4절 — UE FTimeline 규칙): 기본값·Tick
// 켜고 끄기, 트랙 평가와 이벤트 순서, 이벤트 키 구간(끝·시작 넓힘), 끝·loop(앞·뒤, 길이 0), 뒤로·
// 배율, 길이 모드·자르기, 시간 배율 무시, 로드 사본(요청 대조·지우기·NotFound), 비유한 delta·큰
// delta loop 상한, 리플렉션과 저장 왕복.
using namespace rocks;
namespace timeline_component_tests_file {

namespace {

constexpr float Tolerance = 1.0e-5f;

bool Near(float value, float expected, float tolerance = Tolerance) noexcept {
    const bool same = fabsf(value - expected) <= tolerance;
    if (!same) {
        printf("  value %.9g, expected %.9g\n", static_cast<double>(value),
               static_cast<double>(expected));
    }

    return same;
}

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

class EmptyActor final : public Actor {
    ROCKS_NATIVE_TYPE(EmptyActor, Actor, 0x6a3e91c4f05b2d78ull, 0xc8d2f6014e9b7a35ull)

public:
    explicit EmptyActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

// OnTick 횟수를 센다(Tick 켜고 끄기 확인 — 32.1절).
class CountingTimeline final : public TimelineComponent {
    ROCKS_NATIVE_TYPE(CountingTimeline, TimelineComponent, 0x27c5e0b94a1d6f83ull,
                      0x9f40b3d8e27c1a56ull)

public:
    explicit CountingTimeline(Allocator& allocator) noexcept : TimelineComponent(allocator) {
    }

    int32 ticks = 0;

protected:
    void OnTick(const TickContext& context) noexcept override {
        ++ticks;
        TimelineComponent::OnTick(context);
    }
};

// 이벤트 기록이다('v' 벡터, 'f' float, 'c' 색, 'k' 키, 'u' updated, 'e' finished).
struct EventLog {
    static constexpr usize Capacity = 64;

    char kinds[Capacity] = {};
    Name names[Capacity] = {};
    float values[Capacity] = {};
    usize count = 0;

    void Add(char kind, Name name = {}, float value = 0.0f) noexcept {
        if (count < Capacity) {
            kinds[count] = kind;
            names[count] = name;
            values[count] = value;
        }

        ++count;
    }

    usize Count(char kind) const noexcept {
        usize total = 0;
        for (usize index = 0; index < count && index < Capacity; ++index) {
            total += kinds[index] == kind ? 1 : 0;
        }

        return total;
    }

    bool Order(StringView expected) const noexcept {
        if (expected.ByteCount() != count) {
            printf("  log count %llu, expected %llu\n", static_cast<unsigned long long>(count),
                   static_cast<unsigned long long>(expected.ByteCount()));
            return false;
        }

        for (usize index = 0; index < count; ++index) {
            if (kinds[index] != expected.Data()[index]) {
                printf("  log[%llu] '%c', expected '%c'\n", static_cast<unsigned long long>(index),
                       kinds[index], expected.Data()[index]);
                return false;
            }
        }

        return true;
    }
};

EventLog eventLog;

void Subscribe(TimelineComponent& timeline) noexcept {
    CHECK(timeline.floatTrackUpdated.TryAddLambda(
        [](Name name, float value) noexcept { eventLog.Add('f', name, value); }));
    CHECK(timeline.vectorTrackUpdated.TryAddLambda([](Name name, Vec3d value) noexcept {
        eventLog.Add('v', name, static_cast<float>(value.x));
    }));
    CHECK(timeline.colorTrackUpdated.TryAddLambda(
        [](Name name, LinearColor value) noexcept { eventLog.Add('c', name, value.r); }));
    CHECK(
        timeline.eventKeyReached.TryAddLambda([](Name name) noexcept { eventLog.Add('k', name); }));
    CHECK(timeline.updated.TryAddLambda([]() noexcept { eventLog.Add('u'); }));
    CHECK(timeline.finished.TryAddLambda([]() noexcept { eventLog.Add('e'); }));
}

struct Fixture {
    Fixture() noexcept : types(allocator), modules(types, allocator), names(allocator) {
        registered = names.Initialize().IsOk() && RegisterWorldIntegrationTypes(modules).IsOk();
        if (registered) {
            auto empty = RegisterNativeClass<EmptyActor>(types);
            auto counting = RegisterNativeClass<CountingTimeline>(types);
            registered = empty.IsOk() && counting.IsOk();
            if (empty) {
                emptyToken = empty.Value();
            }

            if (counting) {
                countingToken = counting.Value();
            }
        }

        WorldDesc desc;
        desc.name = "Timelines";
        desc.context = {0x544c494eull, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.objects.names = &names;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }

        eventLog = EventLog{};
    }

    ~Fixture() {
        world.Reset();
        if (registered) {
            CHECK(types.Unregister(countingToken).IsOk());
            CHECK(types.Unregister(emptyToken).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
    }

    Name Of(StringView text) noexcept {
        auto name = names.TryIntern(text);
        return name ? name.Value() : Name{};
    }

    // ActorType은 저장 시험에서 리플렉션 Actor다(native 시험 Actor는 그래프에 저장할 수 없다).
    template <class T = TimelineComponent, class ActorType = EmptyActor>
    ObjectHandle AddTimeline(ObjectHandle level = {}) noexcept {
        SpawnParams params;
        params.level = level;
        auto actor = world->SpawnActor<ActorType>(params);
        if (!actor) {
            return {};
        }

        auto added = world->AddComponent<T>(actor.Value());
        return added ? added.Value() : ObjectHandle{};
    }

    template <class Fn>
    bool With(ObjectHandle timeline, Fn&& fn) noexcept {
        auto done =
            world->With<TimelineComponent>(timeline, [&](TimelineComponent& value) noexcept {
                fn(value);
                return true;
            });
        return done && done.Value();
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    NamePool names;
    TypeRegistrationToken emptyToken{};
    TypeRegistrationToken countingToken{};
    bool registered = false;
    UniquePtr<World> world;
};

// 선형 (0, 0) → (1, 10) 커브다.
TangentCurve Ramp(Allocator& allocator, float end = 1.0f, float value = 10.0f) noexcept {
    TangentCurve curve(allocator);
    CHECK(curve.TryAddKey(0.0f, 0.0f) && curve.TryAddKey(end, value));
    return curve;
}

} // namespace

// 32.2절 기본값(UE FTimeline 생성자)과 32.1절 Tick: 시작 때 꺼져 있고, Play가 켜고, 끝나 멈추면
// 끈다.
TEST(Timeline_DefaultsAndTickEnable) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    const ObjectHandle timeline = fixture.AddTimeline<CountingTimeline>();
    REQUIRE(!timeline.IsNull());
    REQUIRE(world.BeginPlay());
    CHECK(fixture.With(timeline, [](TimelineComponent& value) noexcept {
        CHECK(value.GetTimelineLengthMode() == TimelineLengthMode::LastKeyFrame);
        CHECK(value.GetLengthSetting() == TimelineComponent::DefaultLength);
        CHECK(value.GetPlayRate() == 1.0f && value.GetPlaybackPosition() == 0.0f);
        CHECK(!value.IsPlaying() && !value.IsReversing() && !value.IsLooping());
        CHECK(!value.GetIgnoreTimeDilation() && value.GetTimelineLength() == 0.0f);
        CHECK(value.GetTickSettings().tick &&
              value.GetTickSettings().phase == TickPhase::PrePhysics);
    }));
    auto ticks = [&]() noexcept {
        auto count = world.With<CountingTimeline>(
            timeline, [](CountingTimeline& value) noexcept { return value.ticks; });
        return count ? count.Value() : -1;
    };
    REQUIRE(world.Tick(0.1));
    CHECK(ticks() == 0);
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        CHECK(value.TryAddFloatTrack(fixture.Of("Fade"), Ramp(fixture.allocator, 0.15f)));
        value.Play();
    }));
    REQUIRE(world.Tick(0.1));
    CHECK(ticks() == 1);
    REQUIRE(world.Tick(0.1)); // 0.2 > 0.15 → 끝, Tick 끔
    CHECK(ticks() == 2);
    REQUIRE(world.Tick(0.1));
    CHECK(ticks() == 2);
    CHECK(fixture.With(timeline, [](TimelineComponent& value) noexcept {
        CHECK(!value.IsPlaying() && Near(value.GetPlaybackPosition(), 0.15f));
    }));
}

// 32.3절 SetPlaybackPosition: 벡터 → float → 색 → 키 → updated 순서, 트랙 값, 구독 없는 종류는
// 평가하지 않는다.
TEST(Timeline_TrackValuesAndOrder) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle timeline = fixture.AddTimeline();
    REQUIRE(!timeline.IsNull());
    Allocator& allocator = fixture.allocator;
    const Name fade = fixture.Of("Fade");
    const Name move = fixture.Of("Move");
    const Name tint = fixture.Of("Tint");
    const Name beat = fixture.Of("Beat");
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        CHECK(value.TryAddFloatTrack(fade, Ramp(allocator)));
        VectorCurveValue vector(allocator);
        CHECK(vector.x.TryAddKey(0.0f, 0.0f) && vector.x.TryAddKey(1.0f, 4.0f));
        CHECK(value.TryAddVectorTrack(move, vector));
        ColorCurveValue color(allocator);
        CHECK(color.r.TryAddKey(0.0f, 0.2f) && color.r.TryAddKey(1.0f, 0.6f));
        CHECK(value.TryAddColorTrack(tint, color));
        CHECK(value.TryAddEventKey(0.25f, beat));
        CHECK(value.TryAddEventKey(0.75f, beat));
        CHECK(value.TryAddEventKey(NAN, beat).Error() == ErrorCode::InvalidArgument);
        CHECK(value.TryAddEventKey(INFINITY, beat).Error() == ErrorCode::InvalidArgument);
        // 구독 전: 평가하지 않으므로 아무 기록도 없다.
        value.SetPlaybackPosition(0.1f, true);
        CHECK(eventLog.count == 0);
        Subscribe(value);
        value.SetPlaybackPosition(0.5f, true);
        CHECK(eventLog.Order("vfcku"));
        CHECK(Near(eventLog.values[0], 2.0f) && eventLog.names[0] == move);
        CHECK(Near(eventLog.values[1], 5.0f) && eventLog.names[1] == fade);
        CHECK(Near(eventLog.values[2], 0.4f) && eventLog.names[2] == tint);
        CHECK(eventLog.names[3] == beat);
        // 이벤트 없이·update 없이.
        eventLog = EventLog{};
        value.SetPlaybackPosition(0.9f, false, false);
        CHECK(eventLog.Order("vfc"));
        CHECK(value.GetTimelineLength() == 1.0f);
    }));
}

// 32.3절 이벤트 키 구간: 앞으로 [old, new)(끝에서 1e-4 넓힘), 뒤로 (new, old](0에서 넓힘), 같은
// 시간 키 여럿은 모두.
TEST(Timeline_EventKeyIntervals) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle timeline = fixture.AddTimeline();
    REQUIRE(!timeline.IsNull());
    const Name a = fixture.Of("A");
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        value.SetTimelineLengthMode(TimelineLengthMode::TimelineLength);
        value.SetTimelineLength(1.0f);
        CHECK(value.TryAddEventKey(0.0f, a) && value.TryAddEventKey(0.5f, a) &&
              value.TryAddEventKey(0.5f, a) && value.TryAddEventKey(1.0f, a));
        Subscribe(value);
        value.SetPlaybackPosition(1.0f, true, false); // [0, 1 + ε) — 넷 모두
        CHECK(eventLog.Count('k') == 4);
        eventLog = EventLog{};
        value.Reverse();
        value.SetPlaybackPosition(0.0f, true, false); // (0 − ε, 1] — 넷 모두
        CHECK(eventLog.Count('k') == 4);
        eventLog = EventLog{};
        value.Play();
        value.SetTimelineLength(2.0f);
        value.SetPlaybackPosition(0.5f, false, false);
        value.SetPlaybackPosition(1.0f, true, false); // [0.5, 1) — 0.5 둘, 끝 넓힘 없음
        CHECK(eventLog.Count('k') == 2);
        eventLog = EventLog{};
        value.Reverse();
        value.SetPlaybackPosition(0.5f, true, false); // (0.5, 1] — 1 하나
        CHECK(eventLog.Count('k') == 1);
    }));
}

// 32.3절 끝·loop: 끝에서 멈추고 finished 한 번(Stop이면 없음), loop 앞으로·뒤로 끝 키와 남은 위치,
// 0·끝 점프는 이벤트·update 없음, 길이 0 loop.
TEST(Timeline_EndAndLoop) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle timeline = fixture.AddTimeline();
    REQUIRE(!timeline.IsNull());
    const Name end = fixture.Of("End");
    const Name start = fixture.Of("Start");
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        value.SetTimelineLengthMode(TimelineLengthMode::TimelineLength);
        value.SetTimelineLength(1.0f);
        CHECK(value.TryAddEventKey(1.0f, end) && value.TryAddEventKey(0.0f, start));
        Subscribe(value);
        value.Play();
        value.TickTimeline(0.6f);
        value.TickTimeline(0.6f);
        CHECK(!value.IsPlaying() && value.GetPlaybackPosition() == 1.0f);
        CHECK(eventLog.Count('e') == 1 && eventLog.Count('k') == 2); // 0(첫 Tick)·끝
        // Stop: finished 없음.
        eventLog = EventLog{};
        value.PlayFromStart();
        value.TickTimeline(0.3f);
        value.Stop();
        value.TickTimeline(0.3f);
        CHECK(eventLog.Count('e') == 0 && Near(value.GetPlaybackPosition(), 0.3f));
        // loop 앞으로: 0.9 + 0.3 → 끝 키, 0 점프(이벤트 없음), 남은 0.2 구간 [0, 0.2) — 시작 키.
        eventLog = EventLog{};
        value.SetLooping(true);
        value.SetPlaybackPosition(0.9f, false, false);
        value.Play();
        eventLog = EventLog{};
        value.TickTimeline(0.3f);
        CHECK(value.IsPlaying() && Near(value.GetPlaybackPosition(), 0.2f, 1.0e-4f));
        CHECK(eventLog.Order("kku") && eventLog.names[0] == end && eventLog.names[1] == start);
        // loop 뒤로: 0.1 − 0.3 → 시작 키, 끝 점프, 남은 0.8.
        eventLog = EventLog{};
        value.SetPlaybackPosition(0.1f, false, false);
        value.Reverse();
        eventLog = EventLog{};
        value.TickTimeline(0.3f);
        CHECK(Near(value.GetPlaybackPosition(), 0.8f, 1.0e-4f));
        CHECK(eventLog.Order("kku") && eventLog.names[0] == start && eventLog.names[1] == end);
        // 길이 0 loop: 앞으로·뒤로 모두 위치 0.
        value.SetTimelineLength(0.0f);
        value.Play();
        value.TickTimeline(0.5f);
        CHECK(value.GetPlaybackPosition() == 0.0f && value.IsPlaying());
        value.Reverse();
        value.TickTimeline(0.5f);
        CHECK(value.GetPlaybackPosition() == 0.0f && value.IsPlaying());
    }));
}

// 32.3절 뒤로·배율: ReverseFromEnd·PlayFromStart, 음수 배율은 뒤로, 배율 0은 멈추지 않고 그대로,
// GetScaledTimelineLength.
TEST(Timeline_ReverseAndRate) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle timeline = fixture.AddTimeline();
    REQUIRE(!timeline.IsNull());
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        value.SetTimelineLengthMode(TimelineLengthMode::TimelineLength);
        value.SetTimelineLength(2.0f);
        value.ReverseFromEnd();
        CHECK(value.IsReversing() && value.GetPlaybackPosition() == 2.0f);
        value.TickTimeline(0.5f);
        CHECK(Near(value.GetPlaybackPosition(), 1.5f));
        value.PlayFromStart();
        CHECK(!value.IsReversing() && value.GetPlaybackPosition() == 0.0f);
        value.SetPlayRate(-2.0f);
        value.SetPlaybackPosition(1.0f, false);
        value.TickTimeline(0.25f);
        CHECK(Near(value.GetPlaybackPosition(), 0.5f));
        value.SetPlayRate(0.0f);
        value.TickTimeline(1.0f);
        CHECK(Near(value.GetPlaybackPosition(), 0.5f) && value.IsPlaying());
        CHECK(value.GetScaledTimelineLength() == 0.0f);
        value.SetPlayRate(4.0f);
        CHECK(Near(value.GetScaledTimelineLength(), 0.5f));
    }));
}

// 32.3절 길이: LastKeyFrame(이벤트 키·트랙 커브 마지막 키), SetTimelineLength가 위치를 자름,
// SetNewTime 자르기(LengthMode와 무관하게 length).
TEST(Timeline_LengthModes) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle timeline = fixture.AddTimeline();
    REQUIRE(!timeline.IsNull());
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        CHECK(value.TryAddFloatTrack(fixture.Of("A"), Ramp(fixture.allocator, 2.0f)));
        CHECK(value.TryAddFloatTrack(fixture.Of("Empty"), TangentCurve(fixture.allocator)));
        CHECK(value.GetTimelineLength() == 2.0f);
        VectorCurveValue vector(fixture.allocator);
        CHECK(vector.z.TryAddKey(2.5f, 1.0f));
        CHECK(value.TryAddVectorTrack(fixture.Of("V"), vector));
        CHECK(value.GetTimelineLength() == 2.5f);
        ColorCurveValue color(fixture.allocator);
        CHECK(color.a.TryAddKey(2.75f, 1.0f));
        CHECK(value.TryAddColorTrack(fixture.Of("C"), color));
        CHECK(value.GetTimelineLength() == 2.75f);
        CHECK(value.TryAddEventKey(3.0f, fixture.Of("Late")));
        CHECK(value.GetTimelineLength() == 3.0f);
        value.SetTimelineLengthMode(TimelineLengthMode::TimelineLength);
        CHECK(value.GetTimelineLength() == TimelineComponent::DefaultLength);
        value.SetPlaybackPosition(0.8f, false);
        value.SetTimelineLength(0.5f);
        CHECK(value.GetPlaybackPosition() == 0.5f - TimelineComponent::EventEdgeEpsilon);
        value.SetNewTime(-1.0f);
        CHECK(value.GetPlaybackPosition() == 0.0f);
        value.SetNewTime(10.0f);
        CHECK(value.GetPlaybackPosition() == 0.5f);
        value.SetNewTime(NAN);
        CHECK(value.GetPlaybackPosition() == 0.5f); // UE Clamp: NaN → 위 끝
    }));
}

// 32.3절 시간 배율: World 시간 배율 0.5에서 기본은 배율 적용 delta, ignoreTimeDilation이면
// unscaled.
TEST(Timeline_IgnoreTimeDilation) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    const ObjectHandle scaled = fixture.AddTimeline();
    const ObjectHandle unscaled = fixture.AddTimeline();
    REQUIRE(!scaled.IsNull() && !unscaled.IsNull());
    REQUIRE(world.BeginPlay());
    REQUIRE(world.Clock().SetTimeScale(0.5));
    for (ObjectHandle handle : {scaled, unscaled}) {
        CHECK(fixture.With(handle, [&](TimelineComponent& value) noexcept {
            value.SetTimelineLengthMode(TimelineLengthMode::TimelineLength);
            value.SetIgnoreTimeDilation(handle == unscaled);
            value.Play();
        }));
    }

    REQUIRE(world.Tick(0.1));
    CHECK(fixture.With(scaled, [](TimelineComponent& value) noexcept {
        CHECK(Near(value.GetPlaybackPosition(), 0.05f, 1.0e-4f));
    }));
    CHECK(fixture.With(unscaled, [](TimelineComponent& value) noexcept {
        CHECK(Near(value.GetPlaybackPosition(), 0.1f, 1.0e-4f));
    }));
}

// 32.2절 로드 사본: 에셋 트랙은 사본 전에 평가하지 않고, ApplyLoaded* 뒤 에셋 커브(색은 조정 적용),
// 요청 참조가 다르면 InvalidArgument(값 유지), SetFloatTrackCurveAsset이 사본을 지우고, 없는 이름·
// None은 NotFound다.
TEST(Timeline_LoadedCurveCopies) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle timeline = fixture.AddTimeline();
    REQUIRE(!timeline.IsNull());
    Allocator& allocator = fixture.allocator;
    const AssetId curveId{Guid{0x544c4346ull, 1}};
    const AssetId otherId{Guid{0x544c4346ull, 2}};
    SoftObjectRef<FloatCurveAsset> softFloat(allocator);
    REQUIRE(softFloat.TryAssign(curveId, {}));
    SoftObjectRef<FloatCurveAsset> otherFloat(allocator);
    REQUIRE(otherFloat.TryAssign(otherId, {}));
    SoftObjectRef<ColorCurveAsset> softColor(allocator);
    REQUIRE(softColor.TryAssign(curveId, "Tint"));
    SoftObjectRef<VectorCurveAsset> softVector(allocator);
    REQUIRE(softVector.TryAssign(curveId, "Move"));
    // 에셋 Registry 쪽 커브 객체다(로드 시스템이 준다고 본다 — 값으로만 복사한다).
    FloatCurveAsset floatAsset(allocator);
    REQUIRE(floatAsset.curve.TryAddKey(0.0f, 100.0f) && floatAsset.curve.TryAddKey(4.0f, 200.0f));
    ColorCurveAsset colorAsset(allocator);
    REQUIRE(colorAsset.r.TryAddKey(0.0f, 0.5f) && colorAsset.g.TryAddKey(0.0f, 0.25f));
    colorAsset.adjustBrightness = 0.5f;
    VectorCurveAsset vectorAsset(allocator);
    REQUIRE(vectorAsset.x.TryAddKey(0.0f, 7.0f));
    const Name fade = fixture.Of("Fade");
    const Name tint = fixture.Of("Tint");
    const Name move = fixture.Of("Move");
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        CHECK(value.TryAddFloatTrack(fade, Ramp(allocator)));
        CHECK(value.TryAddColorTrack(tint, ColorCurveValue(allocator)));
        CHECK(value.TryAddVectorTrack(move, VectorCurveValue(allocator)));
        CHECK(value.SetFloatTrackCurveAsset(fade, softFloat));
        CHECK(value.SetColorTrackCurveAsset(tint, softColor));
        CHECK(value.SetVectorTrackCurveAsset(move, softVector));
        CHECK(value.SetFloatTrackCurveAsset(fixture.Of("Missing"), softFloat).Error() ==
              ErrorCode::NotFound);
        CHECK(value.SetFloatTrackCurveAsset(Name{}, softFloat).Error() == ErrorCode::NotFound);
        CHECK(!value.HasFloatCurve(0) && !value.HasColorCurve(0) && !value.HasVectorCurve(0));
        CHECK(value.GetTimelineLength() == 0.0f); // 커브 없는 트랙은 0
        Subscribe(value);
        value.SetPlaybackPosition(1.0f, false, false);
        CHECK(eventLog.count == 0);
        // 요청 참조 불일치·null·범위 밖: InvalidArgument.
        CHECK(value.ApplyLoadedFloatCurve(0, otherFloat, floatAsset).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(value.ApplyLoadedFloatCurve(0, SoftObjectRef<FloatCurveAsset>(allocator), floatAsset)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(value.ApplyLoadedFloatCurve(5, softFloat, floatAsset).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(!value.HasFloatCurve(0));
        CHECK(value.ApplyLoadedFloatCurve(0, softFloat, floatAsset));
        CHECK(value.ApplyLoadedColorCurve(0, softColor, colorAsset));
        CHECK(value.ApplyLoadedVectorCurve(0, softVector, vectorAsset));
        CHECK(value.HasFloatCurve(0) && value.GetTimelineLength() == 4.0f);
        eventLog = EventLog{};
        value.SetPlaybackPosition(2.0f, false, false);
        CHECK(eventLog.Order("vfc"));
        CHECK(Near(eventLog.values[0], 7.0f) && Near(eventLog.values[1], 150.0f));
        CHECK(Near(eventLog.values[2], colorAsset.Evaluate(2.0f).r)); // 조정 적용(명도 0.5)
        CHECK(fabsf(eventLog.values[2] - 0.5f) > 1.0e-3f);
        // 같은 에셋을 다시 주면 사본을 지우지 않는다(30.3절 SetStaticMesh와 같다).
        CHECK(value.SetFloatTrackCurveAsset(fade, softFloat) && value.HasFloatCurve(0));
        // 벡터·색도 이름 없음 NotFound, 요청 불일치·범위 밖 InvalidArgument.
        CHECK(value.SetVectorTrackCurveAsset(fixture.Of("Missing"), softVector).Error() ==
              ErrorCode::NotFound);
        CHECK(value.SetColorTrackCurveAsset(Name{}, softColor).Error() == ErrorCode::NotFound);
        SoftObjectRef<VectorCurveAsset> wrongVector(allocator);
        REQUIRE(wrongVector.TryAssign(otherId, {}));
        CHECK(value.ApplyLoadedVectorCurve(0, wrongVector, vectorAsset).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(value.ApplyLoadedColorCurve(3, softColor, colorAsset).Error() ==
              ErrorCode::InvalidArgument);
        // 에셋을 바꾸면 사본을 지운다. ClearLoaded*도 지운다.
        CHECK(value.SetFloatTrackCurveAsset(fade, otherFloat));
        CHECK(!value.HasFloatCurve(0));
        value.ClearLoadedColorCurve(0);
        value.ClearLoadedColorCurve(9);
        CHECK(!value.HasColorCurve(0) && value.HasVectorCurve(0));
        value.ClearLoadedVectorCurve(0);
        value.ClearLoadedFloatCurve(0);
        CHECK(!value.HasVectorCurve(0) && !value.HasFloatCurve(0));
        // null 에셋이면 인라인 커브로 돌아간다(32.6절 5).
        CHECK(value.SetFloatTrackCurveAsset(fade, SoftObjectRef<FloatCurveAsset>(allocator)));
        CHECK(value.HasFloatCurve(0) && value.FloatTracks()[0].curveAsset.IsNull());
    }));
}

// 32.6절 3: 비유한 delta·배율은 그 Tick을 건너뛰고, 길이에 비해 큰 delta의 loop는 반복 상한에서
// 정확한 나머지로 줄여 끝난다(위치는 [0, 길이]).
TEST(Timeline_NonFiniteAndHugeDelta) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle timeline = fixture.AddTimeline();
    REQUIRE(!timeline.IsNull());
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        value.SetTimelineLengthMode(TimelineLengthMode::TimelineLength);
        value.SetTimelineLength(1.0f);
        value.Play();
        value.TickTimeline(0.25f);
        value.TickTimeline(NAN);
        value.TickTimeline(INFINITY);
        CHECK(Near(value.GetPlaybackPosition(), 0.25f) && value.IsPlaying());
        value.SetPlayRate(NAN);
        value.TickTimeline(0.25f);
        CHECK(Near(value.GetPlaybackPosition(), 0.25f));
        value.SetPlayRate(1.0f);
        value.SetLooping(true);
        value.SetTimelineLength(0.001f);
        value.TickTimeline(1.0e5f);
        const float position = value.GetPlaybackPosition();
        CHECK(position >= 0.0f && position <= 0.001f);
        value.Reverse();
        value.TickTimeline(1.0e5f);
        const float back = value.GetPlaybackPosition();
        CHECK(back >= 0.0f && back <= 0.001f);
    }));
}

// 리플렉션 형태와 저장 왕복: 설정·트랙·키·에셋 참조는 저장되고 로드 사본은 저장되지 않는다.
TEST(Timeline_ReflectionAndSaveRoundTrip) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    {
        auto lease = fixture.types.Acquire(ObjectTypeIdOf<TimelineComponent>());
        REQUIRE(lease);
        const TypeSchema& schema = lease.Value().Schema();
        CHECK(HasFlag(schema.flags, TypeFlags::WorldOnly));
        CHECK(schema.baseType == ObjectTypeIdOf<Component>());
        usize events = 0;
        for (const FieldSchema& field : schema.fields) {
            events += field.kind == FieldKind::Event ? 1 : 0;
        }

        CHECK(events == 6);
    }

    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto level = world.CreateLevel("Timelines");
    REQUIRE(level);
    const ObjectHandle timeline = fixture.AddTimeline<TimelineComponent, Actor>(level.Value());
    REQUIRE(!timeline.IsNull());
    Allocator& allocator = fixture.allocator;
    const AssetId curveId{Guid{0x544c4346ull, 3}};
    SoftObjectRef<FloatCurveAsset> soft(allocator);
    REQUIRE(soft.TryAssign(curveId, {}));
    FloatCurveAsset asset(allocator);
    REQUIRE(asset.curve.TryAddKey(0.0f, 1.0f));
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        value.SetTimelineLengthMode(TimelineLengthMode::TimelineLength);
        value.SetTimelineLength(3.0f);
        value.SetLooping(true);
        value.SetPlayRate(2.0f);
        CHECK(value.TryAddFloatTrack(fixture.Of("Fade"), Ramp(allocator)));
        CHECK(value.TryAddFloatTrack(fixture.Of("Loaded"), TangentCurve(allocator)));
        CHECK(value.SetFloatTrackCurveAsset(fixture.Of("Loaded"), soft));
        CHECK(value.ApplyLoadedFloatCurve(1, soft, asset));
        CHECK(value.TryAddEventKey(1.5f, fixture.Of("Mid")));
        VectorCurveValue vector(allocator);
        CHECK(vector.y.TryAddKey(0.0f, 3.0f));
        CHECK(value.TryAddVectorTrack(fixture.Of("Move"), vector));
        ColorCurveValue color(allocator);
        CHECK(color.g.TryAddKey(0.0f, 0.5f));
        color.adjustHue = 30.0f;
        CHECK(value.TryAddColorTrack(fixture.Of("Tint"), color));
        SoftObjectRef<ColorCurveAsset> tintAsset(allocator);
        CHECK(tintAsset.TryAssign(curveId, "Tint.Curve"));
        CHECK(value.SetColorTrackCurveAsset(fixture.Of("Tint"), tintAsset));
        value.SetIgnoreTimeDilation(true);
        value.Reverse();
        value.SetPlaybackPosition(0.75f, false, false);
    }));
    Array<uint8> saved(allocator);
    {
        auto bytes = SaveObjectGraph(allocator, world.Objects(), world.Access(), level.Value());
        REQUIRE(bytes);
        saved = Move(bytes.Value());
    }

    RegistryOptions options;
    options.names = &fixture.names;
    ObjectRegistry restored(allocator, fixture.types, {0x544c5253ull, 2}, options);
    const auto access = restored.OwnerAccess();
    auto root = LoadObjectGraph(allocator, restored, fixture.types, access, saved.View());
    REQUIRE(root);
    ObjectHandle restoredTimeline;
    {
        auto borrowed = restored.Resolve<Level>(access, root.Value().Handle());
        REQUIRE(borrowed);
        Level* pointer = Get(borrowed.Value());
        REQUIRE(pointer != nullptr && pointer->Actors().Size() == 1);
        auto actor = restored.Resolve<Actor>(access, pointer->Actors()[0]);
        REQUIRE(actor);
        Actor* actorPointer = Get(actor.Value());
        REQUIRE(actorPointer != nullptr && actorPointer->Components().Size() >= 1);
        restoredTimeline = actorPointer->Components()[actorPointer->Components().Size() - 1];
    }

    auto borrowed = restored.Resolve<TimelineComponent>(access, restoredTimeline);
    REQUIRE(borrowed);
    TimelineComponent* value = Get(borrowed.Value());
    REQUIRE(value != nullptr);
    CHECK(value->GetTimelineLengthMode() == TimelineLengthMode::TimelineLength);
    CHECK(value->GetLengthSetting() == 3.0f && value->IsLooping() && value->GetPlayRate() == 2.0f);
    REQUIRE(value->FloatTracks().Size() == 2 && value->EventKeys().Size() == 1);
    CHECK(value->FloatTracks()[0].trackName == fixture.Of("Fade"));
    CHECK(value->FloatTracks()[0].curve.KeyCount() == 2);
    CHECK(value->FloatTracks()[1].curveAsset.asset == curveId);
    CHECK(!value->HasFloatCurve(1)); // 로드 사본은 저장하지 않는다
    CHECK(value->EventKeys()[0].time == 1.5f &&
          value->EventKeys()[0].eventName == fixture.Of("Mid"));
    CHECK(value->IsPlaying() && value->IsReversing() && value->GetIgnoreTimeDilation());
    CHECK(value->GetPlaybackPosition() == 0.75f);
    REQUIRE(value->VectorTracks().Size() == 1 && value->ColorTracks().Size() == 1);
    CHECK(value->VectorTracks()[0].curve.y.KeyCount() == 1);
    CHECK(value->ColorTracks()[0].curve.adjustHue == 30.0f);
    CHECK(value->ColorTracks()[0].curveAsset.objectPath.View() == StringView("Tint.Curve"));
}

// 32.3절 OnStart: 재생 중으로 저장된 상태(playing 참·Tick 꺼짐)로 시작하면 Tick이 켜진다.
TEST(Timeline_StartRestoresTick) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    const ObjectHandle timeline = fixture.AddTimeline<CountingTimeline>();
    REQUIRE(!timeline.IsNull());
    CHECK(fixture.With(timeline, [](TimelineComponent& value) noexcept {
        value.SetTimelineLengthMode(TimelineLengthMode::TimelineLength);
        value.Play();
        CHECK(value.SetTickEnabled(false)); // 로드한 객체처럼 Tick 꺼짐 + playing
    }));
    REQUIRE(world.BeginPlay());
    REQUIRE(world.Tick(0.1));
    CHECK(fixture.With(timeline, [](TimelineComponent& value) noexcept {
        CHECK(Near(value.GetPlaybackPosition(), 0.1f, 1.0e-4f));
    }));
}

// 재진입: finished 구독자가 Play를 부르면 재생이 이어지고 Tick이 켜진 채다. 구독자가 발행 중
// 트랙·키를 더해도 안전하다(번호로 다시 읽는다).
TEST(Timeline_Reentrancy) {
    Fixture fixture;
    REQUIRE(fixture.registered && fixture.world.Get() != nullptr);
    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    const ObjectHandle timeline = fixture.AddTimeline<CountingTimeline>();
    REQUIRE(!timeline.IsNull());
    REQUIRE(world.BeginPlay());
    Allocator& allocator = fixture.allocator;
    const Name grow = fixture.Of("Grow");
    static TimelineComponent* self = nullptr;
    static int32 restarts = 0;
    restarts = 0;
    CHECK(fixture.With(timeline, [&](TimelineComponent& value) noexcept {
        self = &value;
        value.SetTimelineLengthMode(TimelineLengthMode::TimelineLength);
        value.SetTimelineLength(0.15f);
        CHECK(value.TryAddFloatTrack(grow, Ramp(allocator)));
        CHECK(value.finished.TryAddLambda([]() noexcept {
            if (++restarts == 1) {
                self->PlayFromStart();
            }
        }));
        CHECK(value.floatTrackUpdated.TryAddLambda([](Name name, float) noexcept {
            if (self->FloatTracks().Size() < 8) {
                (void)self->TryAddFloatTrack(
                    name, TangentCurve(self->FloatTracks()[0].curve.GetAllocator()));
                (void)self->TryAddEventKey(0.01f, name);
            }
        }));
        value.Play();
    }));
    auto ticks = [&]() noexcept {
        auto count = world.With<CountingTimeline>(
            timeline, [](CountingTimeline& value) noexcept { return value.ticks; });
        return count ? count.Value() : -1;
    };
    REQUIRE(world.Tick(0.1));
    REQUIRE(world.Tick(0.1)); // 끝 → finished → PlayFromStart(재생 이어짐)
    CHECK(restarts == 1);
    REQUIRE(world.Tick(0.1));
    CHECK(ticks() == 3);
    CHECK(fixture.With(timeline, [](TimelineComponent& value) noexcept {
        CHECK(value.IsPlaying() && value.FloatTracks().Size() == 8);
    }));
    self = nullptr;
}

// 트랙 값 struct TryCopyFrom: 이름·커브·에셋 참조를 깊게 복사한다(자기 자신은 그대로).
TEST(Timeline_TrackCopies) {
    SystemAllocator allocator;
    const AssetId curveId{Guid{0x544c4346ull, 4}};
    TimelineFloatTrack source(allocator);
    REQUIRE(source.curve.TryAddKey(0.0f, 1.0f) && source.curve.TryAddKey(1.0f, 2.0f));
    REQUIRE(source.curveAsset.TryAssign(curveId, "Float"));
    TimelineFloatTrack copy(allocator);
    REQUIRE(copy.TryCopyFrom(source));
    CHECK(copy.curve.KeyCount() == 2 && copy.curveAsset == source.curveAsset);
    REQUIRE(copy.TryCopyFrom(copy));
    CHECK(copy.curve.KeyCount() == 2);
    TimelineVectorTrack vector(allocator);
    REQUIRE(vector.curve.z.TryAddKey(0.0f, 5.0f));
    TimelineVectorTrack vectorCopy(allocator);
    REQUIRE(vectorCopy.TryCopyFrom(vector));
    CHECK(vectorCopy.curve.z.KeyCount() == 1);
    TimelineColorTrack color(allocator);
    REQUIRE(color.curve.b.TryAddKey(0.0f, 0.25f));
    color.curve.adjustSaturation = 0.5f;
    REQUIRE(color.curveAsset.TryAssign(curveId, "Color"));
    TimelineColorTrack colorCopy(allocator);
    REQUIRE(colorCopy.TryCopyFrom(color));
    CHECK(colorCopy.curve.b.KeyCount() == 1 && colorCopy.curve.adjustSaturation == 0.5f);
    CHECK(colorCopy.curveAsset == color.curveAsset);
}

} // namespace timeline_component_tests_file
