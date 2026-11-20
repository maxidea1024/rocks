#include "ClipTestSupport.h"

// 애니메이션 클립 시험이다(Docs/Design/MeshAnimationAssets.ko.md 7절·7.5절, 12절 "클립"): 샘플링
// 규칙, 원본·클립 검증, 정렬, 쿠킹 변환(같은 비율의 키 보존·재샘플링·상수 접기·트랙 대응).
using namespace rocks;
namespace clip_tests_file {

using namespace rocks::clip_test;

struct ClipFixture {
    ClipFixture() noexcept : names(allocator) {
        ready = names.Initialize().IsOk();
    }

    SystemAllocator allocator;
    NamePool names;
    bool ready = false;
};

// 키 셋(0, 1, 2 — 30 fps, 2프레임)의 한 트랙 원본이다.
bool ThreeKeySource(ClipFixture& fixture, ClipSource& source) noexcept {
    source.frameRate = Rate(30);
    source.frameCount = 2;
    ClipTrack track(fixture.allocator);
    track.bone = Intern(fixture.names, "root");
    const Quat turn(0.0f, 0.0f, 0.70710677f, 0.70710677f);
    return track.translation.TryPushBack(Vec3(0, 0, 0)).IsOk() &&
           track.translation.TryPushBack(Vec3(10, 0, 0)).IsOk() &&
           track.translation.TryPushBack(Vec3(10, 20, 0)).IsOk() &&
           track.rotation.TryPushBack(Quat(0, 0, 0, 1)).IsOk() &&
           track.rotation.TryPushBack(turn).IsOk() &&
           track.rotation.TryPushBack(Quat(-turn.x, -turn.y, -turn.z, -turn.w)).IsOk() &&
           track.scale.TryPushBack(Vec3(2, 2, 2)).IsOk() &&
           source.tracks.TryPushBack(Move(track)).IsOk();
}

// 7.5절 8: 0 이하·NaN은 키 0, 마지막 키 시간 이상은 마지막 키, 사이는 선형·nlerp, α 문턱으로 앞·뒤
// 키에 붙고, Step은 앞 키, 상수 성분은 그 값이다.
TEST(Clip_SampleSourceRules) {
    ClipFixture fixture;
    REQUIRE(fixture.ready);
    ClipSource source(fixture.allocator);
    REQUIRE(ThreeKeySource(fixture, source));
    const ClipTrack& keys = source.tracks[0];
    auto at = [&](double seconds, ClipInterpolation interpolation = ClipInterpolation::Linear) {
        return SampleClipSource(source, 0, seconds, interpolation);
    };

    auto close = [](const Vec3& a, const Vec3& b) noexcept {
        return math::Abs(a.x - b.x) < 1e-4f && math::Abs(a.y - b.y) < 1e-4f &&
               math::Abs(a.z - b.z) < 1e-4f;
    };
    CHECK(at(-1.0).translation == keys.translation[0]);
    CHECK(at(0.0).translation == keys.translation[0]);
    CHECK(at(ClipNaN()).translation == keys.translation[0]);
    CHECK(at(source.Length()).translation == keys.translation[2]);
    CHECK(at(100.0).rotation == keys.rotation[2]);
    // 반 프레임: 이동은 가운데, 크기는 상수, 회전은 정규화한 nlerp.
    const Transform3f half = at(0.5 / 30.0);
    CHECK(close(half.translation, Vec3(5, 0, 0)) && half.scale == Vec3(2, 2, 2));
    CHECK(math::Abs(half.rotation.z - 0.38268343f) < 1e-6f &&
          math::Abs(half.rotation.w - 0.9238795f) < 1e-6f);
    // 다음 키 회전은 반대 부호(같은 회전)라 최단 경로로 섞는다: 1.5 프레임은 키 1과 같은 회전.
    const Transform3f shortest = at(1.5 / 30.0);
    CHECK(math::Abs(math::Abs(shortest.rotation.w) - keys.rotation[1].w) < 1e-6f);
    CHECK(close(shortest.translation, Vec3(10, 10, 0)));
    // α 문턱(1e-4) 안쪽은 키에 붙는다.
    CHECK(at(0.00005 / 30.0).translation == keys.translation[0]);
    CHECK(at(0.99995 / 30.0).translation == keys.translation[1]);
    CHECK(!(at(0.001 / 30.0).translation == keys.translation[0]));
    // Step은 앞 키다.
    CHECK(at(1.9 / 30.0, ClipInterpolation::Step).translation == keys.translation[1]);
    // 키 하나인 원본은 언제나 키 0이다.
    source.frameCount = 0;
    ClipTrack& single = source.tracks[0];
    REQUIRE(single.translation.TryResize(1) && single.rotation.TryResize(1));
    CHECK(at(0.5).translation == keys.translation[0]);
}

// 쿠킹 데이터 샘플링: 같은 규칙이고, 0 길이 회전 둘을 섞으면 단위 회전이다(정규화 하한).
TEST(Clip_SamplePlaybackRules) {
    ClipFixture fixture;
    REQUIRE(fixture.ready);
    ClipPlayback playback(fixture.allocator);
    playback.sampleRate = Rate(10);
    playback.sampleCount = 3;
    ClipPlaybackTrack track(fixture.allocator);
    REQUIRE(track.translation.TryPushBack(Vec3(0, 0, 0)) &&
            track.translation.TryPushBack(Vec3(1, 0, 0)) &&
            track.translation.TryPushBack(Vec3(2, 0, 0)) &&
            track.rotation.TryPushBack(Quat(0, 0, 0, 0)) &&
            track.rotation.TryPushBack(Quat(0, 0, 0, 0)) &&
            track.rotation.TryPushBack(Quat(0, 0, 0, 0)) && track.scale.TryPushBack(Vec3(1, 1, 1)));
    REQUIRE(playback.tracks.TryPushBack(Move(track)));
    CHECK(playback.Length() == 0.2);
    const Transform3f value = SampleClipPlayback(playback, 0, 0.15, ClipInterpolation::Linear);
    CHECK(value.translation.x > 1.49f && value.translation.x < 1.51f);
    CHECK(value.rotation == Quat(0, 0, 0, 1));
}

// 원본 검증: 규칙마다 하나씩 어기기(비율·프레임·트랙 수, 이름, 원소 수, 비유한, 정규화 안 된 회전,
// 대소문자만 다른 중복). 정규화 허용 경계 안쪽은 받는다.
TEST(Clip_ValidateSourceRules) {
    ClipFixture fixture;
    REQUIRE(fixture.ready);
    auto expect = [&](auto&& mutate, ClipIssue issue, uint32 index, ErrorCode error) noexcept {
        ClipSource source(fixture.allocator);
        REQUIRE(BuildHumanSource(fixture.names, source, 4));
        mutate(source);
        ClipDiagnostic failure;
        auto result = ValidateClipSource(source, fixture.allocator, &failure);
        CHECK(!result && result.Error() == error);
        CHECK(failure.code == AssetDiagnosticCode::InvalidClipData && failure.issue == issue &&
              failure.index == index);
    };

    ClipSource good(fixture.allocator);
    REQUIRE(BuildHumanSource(fixture.names, good, 4));
    CHECK(ValidateClipSource(good, fixture.allocator));
    expect([](ClipSource& s) noexcept { s.frameRate = Rate(1001); }, ClipIssue::FrameRateTooHigh,
           ClipNoIndex, ErrorCode::InvalidArgument);
    expect(
        [&](ClipSource& s) noexcept {
            for (uint32 at = 0; at < MeshMaxBones; ++at) {
                ClipTrack track(fixture.allocator);
                track.bone = s.tracks[0].bone;
                (void)s.tracks.TryPushBack(Move(track));
            }
        },
        ClipIssue::TooManyTracks, ClipNoIndex, ErrorCode::CapacityExceeded);
    expect([](ClipSource& s) noexcept { s.frameCount = ClipMaxFrames + 1; },
           ClipIssue::TooManyFrames, ClipNoIndex, ErrorCode::CapacityExceeded);
    expect([](ClipSource& s) noexcept { s.tracks[2].bone = Name{}; }, ClipIssue::InvalidTrackName,
           2, ErrorCode::InvalidArgument);
    expect([](ClipSource& s) noexcept { (void)s.tracks[0].scale.TryResize(2); },
           ClipIssue::TrackKeyCount, 0, ErrorCode::InvalidArgument);
    expect([](ClipSource& s) noexcept { s.tracks[3].translation[1].y = ClipNaN(); },
           ClipIssue::NonFiniteKey, 3, ErrorCode::InvalidArgument);
    expect([](ClipSource& s) noexcept { s.tracks[1].rotation[0] = Quat(0, 0, 0, 1.2f); },
           ClipIssue::RotationNotNormalized, 1, ErrorCode::InvalidArgument);
    expect([](ClipSource& s) noexcept { s.tracks[5].rotation[0].w = ClipNaN(); },
           ClipIssue::NonFiniteKey, 5, ErrorCode::InvalidArgument);
    expect([](ClipSource& s) noexcept { s.tracks[4].scale[0].z = ClipInfinity(); },
           ClipIssue::NonFiniteKey, 4, ErrorCode::InvalidArgument);
    // 대소문자만 다른 이름은 중복이다(뒤 트랙 번호, related = 앞 트랙).
    ClipSource duplicate(fixture.allocator);
    REQUIRE(BuildHumanSource(fixture.names, duplicate, 4));
    REQUIRE(AddTrack(fixture.names, duplicate, "SPINE", 4));
    ClipDiagnostic failure;
    CHECK(!ValidateClipSource(duplicate, fixture.allocator, &failure));
    CHECK(failure.issue == ClipIssue::DuplicateTrack && failure.index == 6 && failure.related == 0);
    // 정규화 허용 경계 안쪽(|길이² − 1| ≤ 0.01)은 받는다.
    good.tracks[0].rotation[0] = Quat(0, 0, 0, 1.004f);
    CHECK(ValidateClipSource(good, fixture.allocator));
}

// 클립 검증: 커브·이벤트·동기 마커·설정 규칙마다 하나씩 어기기, 정렬 도우미.
TEST(Clip_ValidateClipRules) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterAnimationTypes(env.modules));
    ObjectHandle package;
    const ObjectHandle handle = MakeClip(env, 0x600, PackageKind::Editor, package);
    AnimationClip* clip = asset_test::Pointer<AnimationClip>(*env.source, handle);
    REQUIRE(clip != nullptr);
    clip->source.frameRate = Rate(30);
    clip->source.frameCount = 30;
    REQUIRE(FillClipSettings(env.names, *clip));
    ClipCurve blink(env.allocator);
    REQUIRE(MakeCurve(env.names, "Blink", blink));
    REQUIRE(clip->source.curves.TryPushBack(Move(blink)));
    ClipTransformCurve layer(env.allocator);
    layer.name = Intern(env.names, "Layer");
    for (uint32 channel = 0; channel < ClipTransformCurveChannels; ++channel) {
        REQUIRE(layer.channels.TryPushBack(TangentCurve(env.allocator)));
    }

    REQUIRE(clip->source.transformCurves.TryPushBack(Move(layer)));
    REQUIRE(ValidateAnimationClip(*clip, env.allocator));
    auto expect = [&](auto&& mutate, ClipIssue issue, uint32 index) noexcept {
        AnimationClip* target = asset_test::Pointer<AnimationClip>(*env.source, handle);
        // 바꾸고 보고 되돌린다.
        mutate(*target, true);
        ClipDiagnostic failure;
        auto result = ValidateAnimationClip(*target, env.allocator, &failure);
        CHECK(!result);
        CHECK(failure.code == AssetDiagnosticCode::InvalidClipData && failure.issue == issue &&
              failure.index == index);
        mutate(*target, false);
        CHECK(ValidateAnimationClip(*target, env.allocator));
    };

    expect(
        [](AnimationClip& c, bool on) noexcept { c.source.frameRate = on ? Rate(2000) : Rate(30); },
        ClipIssue::FrameRateTooHigh, ClipNoIndex);
    expect([](AnimationClip& c,
              bool on) noexcept { c.source.frameCount = on ? ClipMaxFrames + 1 : 30; },
           ClipIssue::TooManyFrames, ClipNoIndex);
    expect(
        [&](AnimationClip& c, bool on) noexcept {
            c.source.curves[0].name = on ? Name{} : Intern(env.names, "Blink");
        },
        ClipIssue::InvalidCurveName, 0);
    expect(
        [&](AnimationClip& c, bool on) noexcept {
            c.source.transformCurves[0].name = on ? Name{} : Intern(env.names, "Layer");
        },
        ClipIssue::InvalidCurveName, 0);
    expect(
        [&](AnimationClip& c, bool on) noexcept {
            if (on) {
                ClipCurve copy(env.allocator);
                copy.name = Intern(env.names, "BLINK");
                (void)c.source.curves.TryPushBack(Move(copy));
            } else {
                (void)c.source.curves.TryRemoveAt(1);
            }
        },
        ClipIssue::DuplicateCurve, 1);
    expect(
        [&](AnimationClip& c, bool on) noexcept {
            if (on) {
                ClipTransformCurve copy(env.allocator);
                copy.name = Intern(env.names, "layer");
                (void)c.source.transformCurves.TryPushBack(Move(copy));
            } else {
                (void)c.source.transformCurves.TryRemoveAt(1);
            }
        },
        ClipIssue::DuplicateCurve, 1);
    expect(
        [](AnimationClip& c, bool on) noexcept {
            if (on) {
                (void)c.source.transformCurves[0].channels.TryRemoveAt(8);
            } else {
                (void)c.source.transformCurves[0].channels.TryPushBack(
                    TangentCurve(c.source.transformCurves.GetAllocator()));
            }
        },
        ClipIssue::TransformCurveChannels, 0);
    expect([](AnimationClip& c, bool on) noexcept { c.events[0].time = on ? -0.1f : 0.25f; },
           ClipIssue::EventTime, 0);
    expect([](AnimationClip& c, bool on) noexcept { c.events[0].time = on ? 1.5f : 0.25f; },
           ClipIssue::EventTime, 0);
    expect([](AnimationClip& c, bool on) noexcept { c.events[0].time = on ? ClipNaN() : 0.25f; },
           ClipIssue::EventTime, 0);
    expect([](AnimationClip& c, bool on) noexcept { c.events[1].duration = on ? ClipNaN() : 0.5f; },
           ClipIssue::EventDuration, 1);
    expect([](AnimationClip& c, bool on) noexcept { c.events[1].duration = on ? 0.9f : 0.5f; },
           ClipIssue::EventDuration, 1);
    expect([](AnimationClip& c, bool on) noexcept { c.events[1].duration = on ? -1.0f : 0.5f; },
           ClipIssue::EventDuration, 1);
    expect([](AnimationClip& c, bool on) noexcept { c.events[1].triggerChance = on ? 1.5f : 0.5f; },
           ClipIssue::EventChance, 1);
    expect([](AnimationClip& c,
              bool on) noexcept { c.events[1].triggerChance = on ? ClipNaN() : 0.5f; },
           ClipIssue::EventChance, 1);
    expect([](AnimationClip& c, bool on) noexcept { c.events[0].track = on ? 3 : 1; },
           ClipIssue::EventTrack, 0);
    expect([](AnimationClip& c, bool on) noexcept { c.events[0].track = on ? -1 : 1; },
           ClipIssue::EventTrack, 0);
    // 같은 시간이면 track 순이다(1번이 track 2 → 1로 바뀌면 순서 위반).
    expect([](AnimationClip& c, bool on) noexcept { c.events[1].track = on ? 0 : 2; },
           ClipIssue::EventOrder, 1);
    const Name leftFoot = clip->syncMarkers[0].name;
    expect(
        [&](AnimationClip& c, bool on) noexcept { c.syncMarkers[0].name = on ? Name{} : leftFoot; },
        ClipIssue::InvalidSyncMarker, 0);
    expect([](AnimationClip& c, bool on) noexcept { c.syncMarkers[1].time = on ? 2.0f : 0.6f; },
           ClipIssue::SyncMarkerTime, 1);
    expect([](AnimationClip& c, bool on) noexcept { c.syncMarkers[1].track = on ? 7 : 1; },
           ClipIssue::SyncMarkerTrack, 1);
    expect([](AnimationClip& c, bool on) noexcept { c.syncMarkers[1].time = on ? 0.05f : 0.6f; },
           ClipIssue::SyncMarkerOrder, 1);
    expect([](AnimationClip& c, bool on) noexcept { c.rateScale = on ? ClipInfinity() : 1.5f; },
           ClipIssue::RateScale, ClipNoIndex);
    expect([](AnimationClip& c, bool on) noexcept { c.additive.basePoseFrame = on ? -1 : 3; },
           ClipIssue::BasePoseFrame, ClipNoIndex);
    expect([](AnimationClip& c,
              bool on) noexcept { c.targetFrameRate.defaultRate = on ? Rate(5000) : Rate(30); },
           ClipIssue::TargetFrameRateTooHigh, ClipNoIndex);
    expect(
        [&](AnimationClip& c, bool on) noexcept {
            (void)c.targetFrameRate.overrides.InsertOrAssign(Intern(env.names, "Mobile"),
                                                             on ? Rate(1001) : Rate(15));
        },
        ClipIssue::TargetFrameRateTooHigh, 0);
    // eventTracks가 없으면(쿠킹 데이터) track 범위는 0 이상만 본다.
    AnimationClip* target = asset_test::Pointer<AnimationClip>(*env.source, handle);
    target->eventTracks.Clear();
    target->events[0].track = 40;
    target->events[1].track = 41;
    CHECK(ValidateAnimationClip(*target, env.allocator));
    // 정렬 도우미: 시간·track 순(같으면 원래 순서 유지), 마커는 시간 순.
    ClipEvent late = target->events[0];
    late.time = 0.75f;
    late.name = Intern(env.names, "Late");
    ClipEvent tie = target->events[0];
    tie.name = Intern(env.names, "Tie");
    REQUIRE(target->events.TryInsert(late, 0));
    REQUIRE(target->events.TryPushBack(tie));
    ClipSyncMarker early = target->syncMarkers[0];
    early.time = 0.0f;
    REQUIRE(target->syncMarkers.TryPushBack(early));
    CHECK(!ValidateAnimationClip(*target, env.allocator));
    SortClipEvents(*target);
    SortClipSyncMarkers(*target);
    CHECK(ValidateAnimationClip(*target, env.allocator));
    CHECK(target->events[0].track == 40 && target->events[1].name == tie.name &&
          target->events[2].track == 41 && target->events[3].name == late.name);
    CHECK(target->syncMarkers[0].time == 0.0f);
}

// 플랫폼별 목표 비율: 덮어쓰기 표에 있으면 그 값, 없거나 None이면 기본값(30/1).
TEST(Clip_PlatformFrameRate) {
    ClipFixture fixture;
    REQUIRE(fixture.ready);
    PlatformFrameRate rates(fixture.allocator);
    CHECK(rates.defaultRate == Rate(30));
    REQUIRE(rates.overrides.InsertOrAssign(Intern(fixture.names, "Mobile"), Rate(15)));
    CHECK(rates.Resolve(Intern(fixture.names, "mobile")) == Rate(15));
    CHECK(rates.Resolve(Intern(fixture.names, "Console")) == Rate(30));
    CHECK(rates.Resolve(Name{}) == Rate(30));
}

// 쿠킹 변환(같은 비율): 키를 bit 그대로 옮기고 상수 성분을 접으며, 스켈레톤 원본 본에 맞는 트랙만
// 남는다(가상 본은 경고 없이, 모르는 본은 UnknownBoneTrack). 커브는 복사하고 쓰는 변환 커브 수를
// 센다. 쿠킹 데이터 샘플링은 원본 샘플링과 bit가 같다.
TEST(Clip_BuildSameRateKeepsKeys) {
    ClipFixture fixture;
    REQUIRE(fixture.ready);
    ClipSource source(fixture.allocator);
    REQUIRE(BuildHumanSource(fixture.names, source, 6));
    Skeleton skeleton(fixture.allocator);
    REQUIRE(FillHumanSkeleton(fixture.names, skeleton));
    ClipCurve blink(fixture.allocator);
    REQUIRE(MakeCurve(fixture.names, "Blink", blink));
    ClipTransformCurve layers[2] = {ClipTransformCurve(fixture.allocator),
                                    ClipTransformCurve(fixture.allocator)};
    layers[1].disabled = true;
    ClipBuildInput input;
    input.source = &source;
    input.curves = Span<const ClipCurve>(&blink, 1);
    input.transformCurves = layers;
    input.skeleton = &skeleton;
    input.targetRate = Rate(30);
    ClipPlayback cooked(fixture.allocator);
    ClipBuildReport report(fixture.allocator);
    REQUIRE(BuildClipPlayback(input, cooked, report));
    CHECK(cooked.sampleCount == 7 && cooked.sampleRate == Rate(30));
    // spine(2)·root(0)·pelvis(1)·hand_l(5)만 남는다(원본 순서).
    const uint16 bones[] = {2, 0, 1, 5};
    REQUIRE(cooked.trackToBone.Size() == 4 && cooked.tracks.Size() == 4);
    for (uint32 at = 0; at < 4; ++at) {
        CHECK(cooked.trackToBone[at] == bones[at]);
    }

    REQUIRE(report.warnings.Size() == 1);
    CHECK(report.warnings[0].code == AssetDiagnosticCode::UnknownBoneTrack &&
          report.warnings[0].issue == ClipIssue::UnknownBoneTrack && report.warnings[0].index == 2);
    CHECK(report.skippedTransformCurves == 1);
    REQUIRE(cooked.curves.Size() == 1);
    CHECK(SameCurve(cooked.curves[0], blink));
    // 상수 성분: 크기는 모든 트랙 1키, root의 회전은 1키, 이동은 키 7개 그대로.
    CHECK(cooked.tracks[1].rotation.Size() == 1 && cooked.tracks[0].scale.Size() == 1);
    CHECK(SameKeys(cooked.tracks[0].translation, source.tracks[0].translation));
    CHECK(SameKeys(cooked.tracks[3].rotation, source.tracks[5].rotation));
    const double times[] = {-1.0, 0.0, 0.01, 0.05, 0.1, 0.123, 0.2, 0.21, 10.0};
    for (double seconds : times) {
        const ClipInterpolation modes[] = {ClipInterpolation::Linear, ClipInterpolation::Step};
        for (ClipInterpolation mode : modes) {
            CHECK(SameTransform3f(SampleClipPlayback(cooked, 0, seconds, mode),
                                  SampleClipSource(source, 0, seconds, mode)));
        }
    }

    // 입력 오류: source·skeleton null(진단 없음), 목표 비율 상한.
    ClipBuildInput empty;
    CHECK(BuildClipPlayback(empty, cooked, report).Error() == ErrorCode::InvalidArgument);
    empty.source = &source;
    CHECK(BuildClipPlayback(empty, cooked, report).Error() == ErrorCode::InvalidArgument);
    input.targetRate = Rate(2000);
    ClipDiagnostic failure;
    CHECK(!BuildClipPlayback(input, cooked, report, &failure));
    CHECK(failure.issue == ClipIssue::FrameRateTooHigh && cooked.sampleCount == 7);
}

// 쿠킹 재샘플링(30 → 60 fps): 표본 = 프레임 × 2 + 1, 짝수 표본은 원본 키와 bit가 같고 홀수 표본은
// 두 키 사이 값이다. Step은 앞 키다. 정수가 아닌 표본 수(30 → 20 fps에 4프레임)와 상한 초과는
// InvalidClipData(IncompatibleTargetFrameRate)다. NTSC 비율(30000/1001 → 60000/1001)도 정수 배다.
TEST(Clip_BuildResamples) {
    ClipFixture fixture;
    REQUIRE(fixture.ready);
    ClipSource source(fixture.allocator);
    REQUIRE(BuildHumanSource(fixture.names, source, 6));
    Skeleton skeleton(fixture.allocator);
    REQUIRE(FillHumanSkeleton(fixture.names, skeleton));
    ClipBuildInput input;
    input.source = &source;
    input.skeleton = &skeleton;
    input.targetRate = Rate(60);
    ClipPlayback cooked(fixture.allocator);
    ClipBuildReport report(fixture.allocator);
    REQUIRE(BuildClipPlayback(input, cooked, report));
    REQUIRE(cooked.sampleCount == 13);
    const ClipPlaybackTrack& spine = cooked.tracks[0];
    REQUIRE(spine.translation.Size() == 13);
    for (uint32 sample = 0; sample < 13; sample += 2) {
        CHECK(platform::MemoryEquals(&spine.translation[sample],
                                     &source.tracks[0].translation[sample / 2], sizeof(Vec3)));
    }

    const Vec3 middle = spine.translation[1];
    CHECK(math::Abs(middle.x - 3.25f) < 1e-5f);
    input.interpolation = ClipInterpolation::Step;
    REQUIRE(BuildClipPlayback(input, cooked, report));
    CHECK(platform::MemoryEquals(&cooked.tracks[0].translation[1], &source.tracks[0].translation[0],
                                 sizeof(Vec3)));
    // 정수가 아닌 표본 수.
    source.frameCount = 4;
    for (ClipTrack& track : source.tracks) {
        REQUIRE(track.translation.TryResize(5));
        if (track.rotation.Size() != 1) {
            REQUIRE(track.rotation.TryResize(5));
        }

        REQUIRE(track.scale.TryResize(5));
    }

    input.targetRate = Rate(20);
    ClipDiagnostic failure;
    CHECK(!BuildClipPlayback(input, cooked, report, &failure));
    CHECK(failure.code == AssetDiagnosticCode::InvalidClipData &&
          failure.issue == ClipIssue::IncompatibleTargetFrameRate);
    // 3프레임이면 20 fps로 2프레임이다.
    source.frameCount = 3;
    for (ClipTrack& track : source.tracks) {
        REQUIRE(track.translation.TryResize(4) && track.scale.TryResize(4));
        if (track.rotation.Size() != 1) {
            REQUIRE(track.rotation.TryResize(4));
        }
    }

    REQUIRE(BuildClipPlayback(input, cooked, report));
    CHECK(cooked.sampleCount == 3);
    // NTSC.
    source.frameRate = Rate(30000, 1001);
    input.targetRate = Rate(60000, 1001);
    REQUIRE(BuildClipPlayback(input, cooked, report));
    CHECK(cooked.sampleCount == 7);
    // 상한: 트랙 없는 원본 2^20 프레임을 2배로.
    ClipSource huge(fixture.allocator);
    huge.frameCount = ClipMaxFrames;
    input.source = &huge;
    input.targetRate = Rate(60);
    CHECK(!BuildClipPlayback(input, cooked, report, &failure));
    CHECK(failure.issue == ClipIssue::IncompatibleTargetFrameRate);
    // 프레임 0인 원본은 어느 비율로든 표본 하나다.
    ClipSource still(fixture.allocator);
    input.source = &still;
    input.targetRate = Rate(24);
    REQUIRE(BuildClipPlayback(input, cooked, report));
    CHECK(cooked.sampleCount == 1);
}

// 진단 이름표: 모든 ClipIssue가 고유한 이름이고 열거 밖은 "Unknown"이다. 새 코드는 4.6절 이름표에
// 있고 ToAssetDiagnostic은 위치를 detail로 옮긴다.
TEST(Clip_DiagnosticNames) {
    constexpr uint32 last = static_cast<uint32>(ClipIssue::UnknownBoneTrack);
    for (uint32 index = 0; index <= last; ++index) {
        const StringView name(ClipIssueName(static_cast<ClipIssue>(index)));
        CHECK(name != StringView("Unknown"));
        for (uint32 other = 0; other < index; ++other) {
            CHECK(name != StringView(ClipIssueName(static_cast<ClipIssue>(other))));
        }
    }

    CHECK(StringView(ClipIssueName(static_cast<ClipIssue>(last + 1))) == StringView("Unknown"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::InvalidClipData)) ==
          StringView("InvalidClipData"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::UnknownBoneTrack)) ==
          StringView("UnknownBoneTrack"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::EventPropertiesDropped)) ==
          StringView("EventPropertiesDropped"));
    ClipDiagnostic diagnostic;
    diagnostic.code = AssetDiagnosticCode::InvalidClipData;
    diagnostic.error = ErrorCode::InvalidArgument;
    CHECK(ToAssetDiagnostic(diagnostic, AssetId{}).detail == 0);
    diagnostic.index = 4;
    const AssetDiagnostic converted = ToAssetDiagnostic(diagnostic, asset_test::FixedId(9));
    CHECK(converted.detail == 4 && converted.code == AssetDiagnosticCode::InvalidClipData &&
          converted.asset == asset_test::FixedId(9));
}

} // namespace clip_tests_file
