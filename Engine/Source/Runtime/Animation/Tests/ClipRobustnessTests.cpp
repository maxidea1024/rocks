#include "ClipTestSupport.h"

#include <Platform/FloatingPointEnvironment.h>
#include <TestSupport/NFailAllocator.h>
#include <stdio.h>

// 애니메이션 클립의 패키지 왕복·변조·결정성·할당 실패·퍼징
// 시험이다(Docs/Design/MeshAnimationAssets.ko.md 7.5절, 12절 "클립" — 상수 트랙 접기의 bit 보존,
// 커브 왕복, 상한).
using namespace rocks;
namespace clip_robustness_tests_file {

using namespace rocks::clip_test;

constexpr uint64 ClipFuzzSeed = 0x4d4136614b455953ull;
constexpr uint32 ClipFuzzCount = 300;

// 원본·커브·설정·쿠킹 데이터를 모두 채운 클립을 만든다. playbackOut은 저장한 쿠킹 데이터다.
bool FillFullClip(asset_test::AssetEnv& env, AnimationClip& clip, MemoryBulkSource& bulk,
                  ClipSource& source, ClipPlayback& playbackOut) noexcept {
    if (!BuildHumanSource(env.names, source, 30) ||
        !StoreClipSource(source, bulk, env.names, clip.source) ||
        !FillClipSettings(env.names, clip)) {
        return false;
    }

    ClipCurve blink(env.allocator);
    ClipTransformCurve layer(env.allocator);
    layer.name = Intern(env.names, "Layer");
    for (uint32 channel = 0; channel < ClipTransformCurveChannels; ++channel) {
        TangentCurve curve(env.allocator);
        if (!curve.TryAddKey(0.0f, static_cast<float>(channel)) ||
            !layer.channels.TryPushBack(Move(curve))) {
            return false;
        }
    }

    if (!MakeCurve(env.names, "Blink", blink) || !clip.source.curves.TryPushBack(Move(blink)) ||
        !clip.source.transformCurves.TryPushBack(Move(layer))) {
        return false;
    }

    Skeleton skeleton(env.allocator);
    if (!FillHumanSkeleton(env.names, skeleton)) {
        return false;
    }

    ClipBuildInput input;
    input.source = &source;
    input.curves = clip.source.curves.View();
    input.transformCurves = clip.source.transformCurves.View();
    input.skeleton = &skeleton;
    input.targetRate = Rate(60);
    ClipBuildReport report(env.allocator);
    return BuildClipPlayback(input, playbackOut, report) &&
           StoreClipPlayback(playbackOut, bulk, clip.playback);
}

// 저장 → 로드 → 재저장 byte 일치(에디터·쿠킹), 두 번 쓴 byte 일치, 원본 트랙(이름 순 정렬·상수
// 접기)·커브·변환 커브·이벤트·설정·쿠킹 데이터 왕복. 쿠킹 패키지에는 EditorOnly 원본이 없다.
TEST(Clip_PackageRoundTripResaveIdentical) {
    const PackageKind kinds[] = {PackageKind::Editor, PackageKind::Cooked};
    for (PackageKind kind : kinds) {
        asset_test::AssetEnv env;
        REQUIRE(env.Ready());
        REQUIRE(RegisterAnimationTypes(env.modules));
        ObjectHandle package;
        const ObjectHandle handle = MakeClip(env, 0x610, kind, package);
        AnimationClip* clip = asset_test::Pointer<AnimationClip>(*env.source, handle);
        REQUIRE(clip != nullptr);
        MemoryBulkSource bulk(env.allocator);
        ClipSource source(env.allocator);
        ClipPlayback cooked(env.allocator);
        REQUIRE(FillFullClip(env, *clip, bulk, source, cooked));
        REQUIRE(ValidateAnimationClip(*clip, env.allocator));
        // 이름 순(접은 byte): hand_l, pelvis, root, spine, Tail, VB spine_head.
        const StringView order[] = {"hand_l", "pelvis", "root", "spine", "Tail", "VB spine_head"};
        REQUIRE(clip->source.boneTracks.Size() == 6);
        for (uint32 at = 0; at < 6; ++at) {
            CHECK(clip->source.boneTracks[at].bone == Intern(env.names, order[at]));
            CHECK(clip->source.boneTracks[at].scale.rawBytes == 12);
        }

        CHECK(clip->source.boneTracks[2].rotation.rawBytes == 16);
        CHECK(clip->source.boneTracks[3].rotation.rawBytes == 31 * 16);
        LoadedClip first(env.allocator);
        REQUIRE(WriteAndLoad(env, package, kind, bulk, first));
        LoadedClip second(env.allocator);
        REQUIRE(WriteAndLoad(env, package, kind, bulk, second));
        CHECK(asset_test::SameBytes(first.bytes.View(), second.bytes.View()));
        AnimationClip* loaded = asset_test::Pointer<AnimationClip>(*env.target, first.clip);
        Package* loadedPackage = asset_test::Pointer<Package>(*env.target, first.package);
        REQUIRE(loaded != nullptr && loadedPackage != nullptr);
        CHECK(loaded->events.Size() == 2 && loaded->events[1].triggerOnFollower &&
              loaded->events[1].duration == 0.5f);
        CHECK(loaded->syncMarkers.Size() == 2 && loaded->syncMarkers[1].track == 1);
        CHECK(loaded->interpolation == ClipInterpolation::Step && loaded->loop &&
              loaded->rateScale == 1.5f);
        CHECK(loaded->rootMotion.rootLock == RootMotionRootLock::FirstFrame &&
              loaded->additive.basePose == AdditiveBasePose::Frame &&
              loaded->additive.basePoseFrame == 3);
        CHECK(loaded->targetFrameRate.Resolve(Intern(env.names, "Mobile")) == Rate(15));
        ClipPlayback reloaded(env.allocator);
        REQUIRE(LoadClipPlayback(loaded->playback, *loadedPackage, reloaded));
        CHECK(SamePlayback(reloaded, cooked));
        CHECK(loaded->playback.sampleRate == Rate(60) && loaded->playback.sampleCount == 61);
        if (kind == PackageKind::Editor) {
            ClipSource back(env.allocator);
            REQUIRE(LoadClipSource(loaded->source, *loadedPackage, env.names, back));
            CHECK(back.frameCount == 30 && back.frameRate == Rate(30));
            REQUIRE(back.tracks.Size() == 6);
            // root(원본 1번)의 회전은 1키로 접혀 돌아온다. 나머지는 키 그대로.
            CHECK(back.tracks[2].rotation.Size() == 1 && back.tracks[2].scale.Size() == 1);
            CHECK(SameKeys(back.tracks[3].translation, source.tracks[0].translation));
            CHECK(SameKeys(back.tracks[3].rotation, source.tracks[0].rotation));
            REQUIRE(loaded->source.curves.Size() == 1 &&
                    loaded->source.transformCurves.Size() == 1);
            CHECK(SameCurve(loaded->source.curves[0], clip->source.curves[0]));
            CHECK(loaded->source.transformCurves[0].channels.Size() == ClipTransformCurveChannels);
            CHECK(loaded->eventTracks.Size() == 3);
            CHECK(ValidateAnimationClip(*loaded, env.allocator));
        } else {
            CHECK(loaded->source.boneTracks.IsEmpty() && loaded->eventTracks.IsEmpty());
        }

        auto contents =
            ReadPackage(env.allocator, first.bytes.View(), asset_test::LoadOptions(kind).read);
        REQUIRE(contents);
        const Array<uint8> resaved =
            asset_test::ResaveAsRead(env.allocator, *env.target, first.package, contents.Value());
        CHECK(asset_test::SameBytes(first.bytes.View(), resaved.View()));
    }
}

// 손으로 만든 bulk 원본(트랙마다 이동·회전·크기 byte)을 담은 클립을 쓰고 로드해 LoadClipSource
// 진단을 돌려준다.
struct CraftedTrack {
    StringView bone;
    Span<const uint8> translation;
    Span<const uint8> rotation;
    Span<const uint8> scale;
    // 이 성분(0·1·2)의 ref를 비운다. 3이면 비우지 않는다.
    uint32 empty = 3;
};

Result<void> LoadCrafted(asset_test::AssetEnv& env, uint64 id, uint32 frameCount,
                         Span<const CraftedTrack> tracks, ClipDiagnostic& failure) noexcept {
    ObjectHandle package;
    const ObjectHandle handle = MakeClip(env, id, PackageKind::Editor, package);
    AnimationClip* clip = asset_test::Pointer<AnimationClip>(*env.source, handle);
    if (clip == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    MemoryBulkSource bulk(env.allocator);
    clip->source.frameCount = frameCount;
    for (const CraftedTrack& crafted : tracks) {
        ClipBoneTrack track;
        track.bone = Intern(env.names, crafted.bone);
        auto translation = bulk.AddCopy(crafted.translation);
        auto rotation = bulk.AddCopy(crafted.rotation);
        auto scale = bulk.AddCopy(crafted.scale);
        if (!translation || !rotation || !scale) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        track.translation = crafted.empty == 0 ? BulkDataRef{} : translation.Value();
        track.rotation = crafted.empty == 1 ? BulkDataRef{} : rotation.Value();
        track.scale = crafted.empty == 2 ? BulkDataRef{} : scale.Value();
        if (!clip->source.boneTracks.TryPushBack(track)) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }
    }

    LoadedClip loaded(env.allocator);
    if (!WriteAndLoad(env, package, PackageKind::Editor, bulk, loaded)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    ClipSource out(env.allocator);
    return LoadClipSource(asset_test::Pointer<AnimationClip>(*env.target, loaded.clip)->source,
                          *asset_test::Pointer<Package>(*env.target, loaded.package), env.names,
                          out, &failure);
}

// float 값들을 little-endian byte로 만든다.
template <usize N>
Array<uint8> FloatBytes(Allocator& allocator, const float (&values)[N]) noexcept {
    Array<uint8> bytes(allocator);
    for (float value : values) {
        uint8 word[4];
        StoreLittleEndian<uint32>(word, platform::BitCast<uint32>(value));
        (void)bytes.TryAppend(Span<const uint8>(word, 4));
    }

    return bytes;
}

// 원본 로드 변조: 원소 크기·개수, 트랙 순서·중복 이름, 비유한·정규화 안 된 키, 빈 ref, 프레임 상한
// — 모두 CorruptPackage(원래 규칙은 issue)다. 정상 손 원본은 받는다.
TEST(Clip_LoadRejectsSourceBytes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterAnimationTypes(env.modules));
    const float vec[] = {1, 2, 3};
    const float twoVec[] = {1, 2, 3, 4, 5, 6};
    const float quat[] = {0, 0, 0, 1};
    const float stretched[] = {0, 0, 0, 2};
    const float nanVec[] = {1, ClipNaN(), 3};
    const float partial[] = {1, 2};
    const Array<uint8> v = FloatBytes(env.allocator, vec);
    const Array<uint8> v2 = FloatBytes(env.allocator, twoVec);
    const Array<uint8> q = FloatBytes(env.allocator, quat);
    const float twoQuat[] = {0, 0, 0, 1, 0, 0, 0, 1};
    const Array<uint8> q2 = FloatBytes(env.allocator, twoQuat);
    const Array<uint8> qs = FloatBytes(env.allocator, stretched);
    const Array<uint8> vn = FloatBytes(env.allocator, nanVec);
    const Array<uint8> vp = FloatBytes(env.allocator, partial);
    struct Case {
        uint32 frames;
        CraftedTrack tracks[2];
        uint32 count;
        ClipIssue issue;
        uint32 index;
    };

    const Case cases[] = {
        {0, {{"root", v.View(), q.View(), v.View()}}, 1, ClipIssue::None, ClipNoIndex},
        {1, {{"root", v2.View(), q.View(), v.View()}}, 1, ClipIssue::None, ClipNoIndex},
        {2, {{"root", v2.View(), q.View(), v.View()}}, 1, ClipIssue::BulkLength, 0},
        {0, {{"root", vp.View(), q.View(), v.View()}}, 1, ClipIssue::BulkLength, 0},
        {0, {{"root", v.View(), v.View(), v.View()}}, 1, ClipIssue::BulkLength, 0},
        {0, {{"root", v.View(), q.View(), q.View()}}, 1, ClipIssue::BulkLength, 0},
        {0, {{"root", v.View(), qs.View(), v.View()}}, 1, ClipIssue::RotationNotNormalized, 0},
        {0, {{"root", vn.View(), q.View(), v.View()}}, 1, ClipIssue::NonFiniteKey, 0},
        {0,
         {{"spine", v.View(), q.View(), v.View()}, {"root", v.View(), q.View(), v.View()}},
         2,
         ClipIssue::TrackOrder,
         1},
        {0,
         {{"Root", v.View(), q.View(), v.View()}, {"root", v.View(), q.View(), v.View()}},
         2,
         ClipIssue::TrackOrder,
         1},
        {0, {{"root", v.View(), q.View(), v.View(), 0}}, 1, ClipIssue::BulkUnreadable, 0},
        {0, {{"root", v.View(), q.View(), v.View(), 1}}, 1, ClipIssue::BulkUnreadable, 0},
        {0, {{"root", v.View(), q.View(), v.View(), 2}}, 1, ClipIssue::BulkUnreadable, 0},
        {2, {{"root", v.View(), q2.View(), v.View()}}, 1, ClipIssue::BulkLength, 0},
        {ClipMaxFrames + 1, {}, 0, ClipIssue::TooManyFrames, ClipNoIndex},
    };
    uint64 id = 0x620;
    for (const Case& item : cases) {
        ClipDiagnostic failure;
        auto result = LoadCrafted(env, id++, item.frames,
                                  Span<const CraftedTrack>(item.tracks, item.count), failure);
        if (item.issue == ClipIssue::None) {
            CHECK(result);
            continue;
        }

        CHECK(!result && result.Error() != ErrorCode::NotFound);
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage && failure.issue == item.issue &&
              failure.index == item.index);
    }

    // 트랙 수 상한은 bulk를 읽기 전에 본다(데이터만으로).
    ClipSourceData many(env.allocator);
    REQUIRE(many.boneTracks.TryResize(MeshMaxBones + 1));
    ObjectHandle package;
    REQUIRE(!(MakeClip(env, 0x6ff, PackageKind::Editor, package) == ObjectHandle{}));
    Package* raw = asset_test::Pointer<Package>(*env.source, package);
    REQUIRE(raw != nullptr);
    ClipSource out(env.allocator);
    ClipDiagnostic failure;
    CHECK(!LoadClipSource(many, *raw, env.names, out, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.issue == ClipIssue::TooManyTracks);
    // 다른 pool의 이름은 그 pool 오류다(진단 코드 없음).
    NamePool other(env.allocator);
    REQUIRE(other.Initialize());
    ClipSourceData foreign(env.allocator);
    ClipBoneTrack track;
    track.bone = other.TryIntern("root").Value();
    REQUIRE(foreign.boneTracks.TryPushBack(track));
    CHECK(!LoadClipSource(foreign, *raw, env.names, out, &failure));
    CHECK(failure.code == AssetDiagnosticCode::None);
}

// 쿠킹 데이터 로드 변조: 원소 수, 짧은 bulk, 비유한 키, trackToBone 길이, sampleCount 0·상한, 빈
// ref, 트랙 수 상한. 저장 쪽 인자 오류는 InvalidArgument다.
TEST(Clip_LoadRejectsPlaybackBytes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterAnimationTypes(env.modules));
    ObjectHandle package;
    const ObjectHandle handle = MakeClip(env, 0x640, PackageKind::Editor, package);
    AnimationClip* clip = asset_test::Pointer<AnimationClip>(*env.source, handle);
    REQUIRE(clip != nullptr);
    MemoryBulkSource bulk(env.allocator);
    auto add = [&](Span<const uint32> words) noexcept {
        Array<uint8> bytes(env.allocator);
        for (uint32 word : words) {
            uint8 raw[4];
            StoreLittleEndian<uint32>(raw, word);
            (void)bytes.TryAppend(Span<const uint8>(raw, 4));
        }

        return bulk.AddCopy(bytes.View()).Value();
    };
    const uint32 one = platform::BitCast<uint32>(1.0f);
    const uint32 nan = 0x7fc00000u;
    const uint32 goodWords[] = {1, 1, 1, 0, 0, 0, 0, 0, 0, one, one, one, one};
    const uint32 badCount[] = {2, 1, 1, 0, 0, 0};
    const uint32 shortHeader[] = {1, 1};
    const uint32 shortData[] = {1, 1, 1, 0};
    const uint32 nonFinite[] = {1, 1, 1, nan, 0, 0, 0, 0, 0, one, one, one, one};
    const uint32 nonFiniteRotation[] = {1, 1, 1, 0, 0, 0, nan, 0, 0, one, one, one, one};
    const uint32 nonFiniteScale[] = {1, 1, 1, 0, 0, 0, 0, 0, 0, one, one, nan, one};
    // 트랙 MeshMaxBones + 1개(트랙마다 원소 수 셋 + 값 10개).
    Array<uint32> manyWords(env.allocator);
    for (uint32 track = 0; track <= MeshMaxBones; ++track) {
        REQUIRE(manyWords.TryAppend(Span<const uint32>(goodWords, 13)));
    }
    // 쿠킹 데이터는 good, 변조 byte는 원본 트랙 ref로 같은 패키지에 담는다(로드한 ref로 읽는다).
    clip->playback.sampleCount = 3;
    clip->playback.tracks = add(goodWords);
    REQUIRE(clip->playback.trackToBone.TryPushBack(4));
    ClipBoneTrack carrier;
    carrier.bone = Intern(env.names, "root");
    carrier.translation = add(badCount);
    carrier.rotation = add(shortHeader);
    carrier.scale = add(shortData);
    ClipBoneTrack carrier2 = carrier;
    carrier2.bone = Intern(env.names, "spine");
    carrier2.translation = add(nonFinite);
    carrier2.rotation = add(nonFiniteRotation);
    carrier2.scale = add(nonFiniteScale);
    ClipBoneTrack carrier3 = carrier;
    carrier3.bone = Intern(env.names, "tail");
    carrier3.translation = add(manyWords.View());
    REQUIRE(clip->source.boneTracks.TryPushBack(carrier) &&
            clip->source.boneTracks.TryPushBack(carrier2) &&
            clip->source.boneTracks.TryPushBack(carrier3));
    LoadedClip loaded(env.allocator);
    REQUIRE(WriteAndLoad(env, package, PackageKind::Editor, bulk, loaded));
    Package* raw = asset_test::Pointer<Package>(*env.target, loaded.package);
    AnimationClip* read = asset_test::Pointer<AnimationClip>(*env.target, loaded.clip);
    REQUIRE(raw != nullptr && read != nullptr);
    ClipPlayback out(env.allocator);
    REQUIRE(LoadClipPlayback(read->playback, *raw, out));
    CHECK(out.tracks.Size() == 1 && out.trackToBone[0] == 4 && out.sampleCount == 3);
    const BulkDataRef goodRef = read->playback.tracks;
    const ClipBoneTrack& carried = read->source.boneTracks[0];
    struct Case {
        BulkDataRef ref;
        uint32 samples;
        uint32 bones;
        ClipIssue issue;
    };

    const Case cases[] = {
        {carried.translation, 3, 1, ClipIssue::TrackKeyCount},
        {carried.rotation, 3, 1, ClipIssue::BulkLength},
        {carried.scale, 3, 1, ClipIssue::BulkLength},
        {read->source.boneTracks[1].translation, 3, 1, ClipIssue::NonFiniteKey},
        {read->source.boneTracks[1].rotation, 3, 1, ClipIssue::NonFiniteKey},
        {read->source.boneTracks[1].scale, 3, 1, ClipIssue::NonFiniteKey},
        {read->source.boneTracks[2].translation, 3, 1, ClipIssue::TooManyTracks},
        {goodRef, 3, 2, ClipIssue::TrackBoneLength},
        {goodRef, 0, 1, ClipIssue::NoSamples},
        {goodRef, ClipMaxFrames + 2, 1, ClipIssue::TooManyFrames},
        {BulkDataRef{}, 3, 1, ClipIssue::BulkUnreadable},
    };
    for (const Case& item : cases) {
        ClipPlaybackData data(env.allocator);
        data.sampleCount = item.samples;
        data.tracks = item.ref;
        REQUIRE(data.trackToBone.TryResize(item.bones));
        ClipDiagnostic failure;
        CHECK(!LoadClipPlayback(data, *raw, out, &failure));
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage && failure.issue == item.issue);
        CHECK(out.tracks.Size() == 1);
    }

    const BulkDataRef good = goodRef;
    ClipPlaybackData rate(env.allocator);
    rate.sampleRate = Rate(1001);
    rate.tracks = good;
    rate.sampleCount = 1;
    ClipDiagnostic failure;
    CHECK(!LoadClipPlayback(rate, *raw, out, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.issue == ClipIssue::FrameRateTooHigh);
    // 저장 쪽 인자 오류.
    ClipPlayback bad(env.allocator);
    ClipPlaybackData target(env.allocator);
    REQUIRE(bad.trackToBone.TryPushBack(0));
    CHECK(StoreClipPlayback(bad, bulk, target).Error() == ErrorCode::InvalidArgument);
    REQUIRE(bad.tracks.TryPushBack(ClipPlaybackTrack(env.allocator)));
    CHECK(StoreClipPlayback(bad, bulk, target).Error() == ErrorCode::InvalidArgument);
    bad.sampleCount = 0;
    CHECK(StoreClipPlayback(bad, bulk, target).Error() == ErrorCode::InvalidArgument);
    CHECK(target.sampleCount == 0 && target.trackToBone.IsEmpty());
}

// 저장 거부: 검증 실패(진단 그대로), 다른 pool의 이름(pool 오류), 실패하면 data는 그대로다.
TEST(Clip_StoreRejectsInvalidSource) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    ClipSource source(env.allocator);
    REQUIRE(BuildHumanSource(env.names, source, 3));
    source.tracks[1].rotation[0] = Quat(0, 0, 0, 3);
    MemoryBulkSource bulk(env.allocator);
    ClipSourceData data(env.allocator);
    ClipDiagnostic failure;
    CHECK(!StoreClipSource(source, bulk, env.names, data, &failure));
    CHECK(failure.issue == ClipIssue::RotationNotNormalized && failure.index == 1);
    CHECK(data.boneTracks.IsEmpty());
    NamePool other(env.allocator);
    REQUIRE(other.Initialize());
    ClipSource foreign(env.allocator);
    REQUIRE(AddTrack(other, foreign, "root", 1));
    CHECK(!StoreClipSource(foreign, bulk, env.names, data, &failure));
    CHECK(failure.code == AssetDiagnosticCode::None && failure.error != ErrorCode::None);
    CHECK(data.boneTracks.IsEmpty());
}

// 저장 순서와 접기: 이름 원문을 ASCII 소문자로 접은 byte 순(앞부분이 같으면 짧은 것 먼저, 숫자는
// 글자보다 앞), 모든 키가 같은 회전은 1키로 접힌다.
TEST(Clip_StoreOrdersAndFolds) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    ClipSource source(env.allocator);
    source.frameCount = 3;
    REQUIRE(AddTrack(env.names, source, "arm_l", 1) && AddTrack(env.names, source, "B2", 2) &&
            AddTrack(env.names, source, "arm", 3) && AddTrack(env.names, source, "b1", 4) &&
            AddTrack(env.names, source, "bA", 5) && AddTrack(env.names, source, "c", 6) &&
            AddTrack(env.names, source, "c_x", 7));
    for (Quat& rotation : source.tracks[2].rotation) {
        rotation = RotationKey(3, 0);
    }

    MemoryBulkSource bulk(env.allocator);
    ClipSourceData data(env.allocator);
    REQUIRE(StoreClipSource(source, bulk, env.names, data));
    const StringView order[] = {"arm", "arm_l", "b1", "B2", "bA", "c", "c_x"};
    REQUIRE(data.boneTracks.Size() == 7);
    for (uint32 at = 0; at < 7; ++at) {
        CHECK(data.boneTracks[at].bone == Intern(env.names, order[at]));
    }

    CHECK(data.boneTracks[0].rotation.rawBytes == 16 && data.boneTracks[1].rotation.rawBytes == 64);
}

// 로드 할당 실패: 원본·쿠킹 데이터 로드는 몇 번째 할당이 실패해도 그 오류이고 out은 그대로이며,
// 끝까지 간 로드는 기준과 같다.
TEST(Clip_LoadAllocationFailures) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterAnimationTypes(env.modules));
    ObjectHandle package;
    const ObjectHandle handle = MakeClip(env, 0x670, PackageKind::Editor, package);
    AnimationClip* clip = asset_test::Pointer<AnimationClip>(*env.source, handle);
    REQUIRE(clip != nullptr);
    MemoryBulkSource bulk(env.allocator);
    ClipSource source(env.allocator);
    ClipPlayback cooked(env.allocator);
    REQUIRE(FillFullClip(env, *clip, bulk, source, cooked));
    LoadedClip loaded(env.allocator);
    REQUIRE(WriteAndLoad(env, package, PackageKind::Editor, bulk, loaded));
    AnimationClip* read = asset_test::Pointer<AnimationClip>(*env.target, loaded.clip);
    Package* raw = asset_test::Pointer<Package>(*env.target, loaded.package);
    REQUIRE(read != nullptr && raw != nullptr);
    ClipSource expected(env.allocator);
    REQUIRE(LoadClipSource(read->source, *raw, env.names, expected));
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        ClipSource out(failing);
        failing.FailOnAttempt(attempt);
        auto result = LoadClipSource(read->source, *raw, env.names, out);
        if (!result) {
            CHECK(result.Error() == ErrorCode::OutOfMemory && out.tracks.IsEmpty());
            ++failures;
            continue;
        }

        REQUIRE(out.tracks.Size() == expected.tracks.Size());
        for (usize at = 0; at < out.tracks.Size(); ++at) {
            CHECK(SameTrack(out.tracks[at], expected.tracks[at]));
        }

        if (failing.Attempts() < attempt) {
            break;
        }
    }

    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        ClipPlayback out(failing);
        failing.FailOnAttempt(attempt);
        auto result = LoadClipPlayback(read->playback, *raw, out);
        if (!result) {
            CHECK(result.Error() == ErrorCode::OutOfMemory && out.tracks.IsEmpty());
            ++failures;
            continue;
        }

        CHECK(SamePlayback(out, cooked));
        if (failing.Attempts() < attempt) {
            break;
        }
    }

    CHECK(failures > 20);
}

// 결정성: 같은 입력이면 쿠킹 결과·저장 byte가 같고, 호출 전 FTZ를 켜도 같다.
TEST(Clip_DeterministicBuild) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    ClipSource source(env.allocator);
    REQUIRE(BuildHumanSource(env.names, source, 30));
    // 비정규 값 이동 키를 섞는다.
    source.tracks[0].translation[3].x = 1.0e-40f;
    Skeleton skeleton(env.allocator);
    REQUIRE(FillHumanSkeleton(env.names, skeleton));
    const FrameRate targets[] = {Rate(30), Rate(60), Rate(10), Rate(90)};
    for (FrameRate target : targets) {
        ClipBuildInput input;
        input.source = &source;
        input.skeleton = &skeleton;
        input.targetRate = target;
        ClipPlayback first(env.allocator);
        ClipPlayback second(env.allocator);
        ClipPlayback flushed(env.allocator);
        ClipBuildReport report(env.allocator);
        REQUIRE(BuildClipPlayback(input, first, report));
        REQUIRE(BuildClipPlayback(input, second, report));
        const uint64 original = platform::detail::ReadFloatingPointControl();
        platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit());
        const bool built = BuildClipPlayback(input, flushed, report).IsOk();
        platform::detail::WriteFloatingPointControl(original);
        REQUIRE(built);
        CHECK(SamePlayback(first, second) && SamePlayback(first, flushed));
        MemoryBulkSource a(env.allocator);
        MemoryBulkSource b(env.allocator);
        ClipPlaybackData da(env.allocator);
        ClipPlaybackData db(env.allocator);
        REQUIRE(StoreClipPlayback(first, a, da) && StoreClipPlayback(flushed, b, db));
        CHECK(da.tracks.sha256 == db.tracks.sha256);
    }
}

// 할당 실패 주입: 몇 번째 할당이 실패해도 그 오류이고 결과는 그대로이며, 끝까지 간 호출은 기준과
// 같다(쿠킹 변환·원본 저장·원본 검증·클립 검증·쿠킹 저장·트랙 찾기).
TEST(Clip_AllocationFailures) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    REQUIRE(RegisterAnimationTypes(env.modules));
    ClipSource source(env.allocator);
    REQUIRE(BuildHumanSource(env.names, source, 8));
    Skeleton skeleton(env.allocator);
    REQUIRE(FillHumanSkeleton(env.names, skeleton));
    ClipCurve blink(env.allocator);
    REQUIRE(MakeCurve(env.names, "Blink", blink));
    const FrameRate targets[] = {Rate(30), Rate(60)};
    for (FrameRate target : targets) {
        ClipBuildInput input;
        input.source = &source;
        input.curves = Span<const ClipCurve>(&blink, 1);
        input.skeleton = &skeleton;
        input.targetRate = target;
        ClipPlayback expected(env.allocator);
        ClipBuildReport expectedReport(env.allocator);
        REQUIRE(BuildClipPlayback(input, expected, expectedReport));
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(env.allocator);
            ClipPlayback built(failing);
            ClipBuildReport report(failing);
            failing.FailOnAttempt(attempt);
            auto result = BuildClipPlayback(input, built, report);
            if (!result) {
                CHECK(result.Error() == ErrorCode::OutOfMemory && built.tracks.IsEmpty() &&
                      report.warnings.IsEmpty());
                ++failures;
                continue;
            }

            CHECK(SamePlayback(built, expected) && report.warnings.Size() == 1);
            if (failing.Attempts() < attempt) {
                break;
            }
        }

        CHECK(failures > 10);
        MemoryBulkSource reference(env.allocator);
        ClipPlaybackData referenceData(env.allocator);
        REQUIRE(StoreClipPlayback(expected, reference, referenceData));
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(env.allocator);
            MemoryBulkSource bulk(failing);
            ClipPlaybackData data(failing);
            failing.FailOnAttempt(attempt);
            auto result = StoreClipPlayback(expected, bulk, data);
            if (!result) {
                CHECK(result.Error() == ErrorCode::OutOfMemory && data.sampleCount == 0);
                continue;
            }

            CHECK(data.tracks.sha256 == referenceData.tracks.sha256);
            if (failing.Attempts() < attempt) {
                break;
            }
        }
    }

    MemoryBulkSource reference(env.allocator);
    ClipSourceData referenceData(env.allocator);
    REQUIRE(StoreClipSource(source, reference, env.names, referenceData));
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        MemoryBulkSource bulk(failing);
        ClipSourceData data(failing);
        failing.FailOnAttempt(attempt);
        auto result = StoreClipSource(source, bulk, env.names, data);
        if (!result) {
            CHECK(result.Error() == ErrorCode::OutOfMemory && data.boneTracks.IsEmpty());
            continue;
        }

        REQUIRE(data.boneTracks.Size() == referenceData.boneTracks.Size());
        for (usize at = 0; at < data.boneTracks.Size(); ++at) {
            CHECK(data.boneTracks[at].translation.sha256 ==
                  referenceData.boneTracks[at].translation.sha256);
        }

        if (failing.Attempts() < attempt) {
            break;
        }
    }

    ObjectHandle package;
    const ObjectHandle handle = MakeClip(env, 0x660, PackageKind::Editor, package);
    AnimationClip* clip = asset_test::Pointer<AnimationClip>(*env.source, handle);
    REQUIRE(clip != nullptr);
    clip->source.frameCount = 30;
    REQUIRE(FillClipSettings(env.names, *clip));
    ClipCurve again(env.allocator);
    REQUIRE(MakeCurve(env.names, "Blink", again));
    REQUIRE(clip->source.curves.TryPushBack(Move(again)));
    ClipTransformCurve layer(env.allocator);
    layer.name = Intern(env.names, "Layer");
    for (uint32 channel = 0; channel < ClipTransformCurveChannels; ++channel) {
        REQUIRE(layer.channels.TryPushBack(TangentCurve(env.allocator)));
    }

    REQUIRE(clip->source.transformCurves.TryPushBack(Move(layer)));
    uint32 validateFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        failing.FailOnAttempt(attempt);
        ClipDiagnostic failure;
        auto clipResult = ValidateAnimationClip(*clip, failing, &failure);
        auto sourceResult = ValidateClipSource(source, failing);
        const bool any = !clipResult || !sourceResult;
        if (any) {
            ++validateFailures;
            CHECK((!clipResult && clipResult.Error() == ErrorCode::OutOfMemory) ||
                  (!sourceResult && sourceResult.Error() == ErrorCode::OutOfMemory));
        }

        if (failing.Attempts() < attempt) {
            break;
        }
    }

    CHECK(validateFailures >= 3);
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(env.allocator);
        Array<ClipDiagnostic> warnings(failing);
        failing.FailOnAttempt(attempt);
        auto result = FindUnknownBoneTracks(source, skeleton, warnings);
        if (!result) {
            CHECK(result.Error() == ErrorCode::OutOfMemory);
            continue;
        }

        CHECK(warnings.Size() == 1);
        break;
    }

    printf("Clip_AllocationFailures: %u validate failures\n", validateFailures);
}

// 고정 seed 퍼징: 무작위 원본(트랙 이름·키 수·값, 가끔 규칙 위반). 검증 통과분은 쿠킹 변환 불변식
// (표본 수 식, 트랙 대응, 성분 길이, 같은 비율이면 키 bit 보존)과 두 번 변환한 결과 일치를 지킨다.
TEST(Clip_FuzzSources) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    Skeleton skeleton(env.allocator);
    REQUIRE(FillHumanSkeleton(env.names, skeleton));
    RandomStream random(ClipFuzzSeed);
    const StringView names[] = {"root", "pelvis", "spine", "head", "Tail", "VB spine_head", "ROOT"};
    const FrameRate rates[] = {Rate(30), Rate(24), Rate(60), Rate(30000, 1001)};
    uint32 valid = 0;
    uint32 invalid = 0;
    uint32 incompatible = 0;
    for (uint32 iteration = 0; iteration < ClipFuzzCount; ++iteration) {
        ClipSource source(env.allocator);
        source.frameRate = rates[random.NextUInt32Below(4)];
        source.frameCount = random.NextUInt32Below(12);
        const uint32 tracks = random.NextUInt32Below(5);
        for (uint32 at = 0; at < tracks; ++at) {
            ClipTrack track(env.allocator);
            track.bone = Intern(env.names, names[random.NextUInt32Below(7)]);
            const uint32 keys = source.KeyCount();
            const uint32 counts[3] = {random.NextUInt32Below(2) == 0 ? 1 : keys,
                                      random.NextUInt32Below(2) == 0 ? 1 : keys,
                                      random.NextUInt32Below(2) == 0 ? 1 : keys};
            for (uint32 key = 0; key < counts[0]; ++key) {
                REQUIRE(track.translation.TryPushBack(
                    Vec3(random.NextFloatInRange(-100.0f, 100.0f), 0.0f, 1.0f)));
            }

            for (uint32 key = 0; key < counts[1]; ++key) {
                REQUIRE(track.rotation.TryPushBack(RotationKey(at, random.NextUInt32Below(40))));
            }

            for (uint32 key = 0; key < counts[2]; ++key) {
                REQUIRE(track.scale.TryPushBack(Vec3(1.0f, 2.0f, 1.0f)));
            }

            if (random.NextUInt32Below(20) == 0) {
                (void)track.scale.TryPushBack(Vec3(1.0f, 1.0f, 1.0f));
            }

            REQUIRE(source.tracks.TryPushBack(Move(track)));
        }

        if (!ValidateClipSource(source, env.allocator)) {
            ++invalid;
            continue;
        }

        ++valid;
        ClipBuildInput input;
        input.source = &source;
        input.skeleton = &skeleton;
        input.targetRate = rates[random.NextUInt32Below(4)];
        input.interpolation =
            random.NextUInt32Below(2) == 0 ? ClipInterpolation::Linear : ClipInterpolation::Step;
        ClipPlayback cooked(env.allocator);
        ClipBuildReport report(env.allocator);
        ClipDiagnostic failure;
        if (!BuildClipPlayback(input, cooked, report, &failure)) {
            CHECK(failure.issue == ClipIssue::IncompatibleTargetFrameRate);
            ++incompatible;
            continue;
        }

        CHECK(cooked.tracks.Size() == cooked.trackToBone.Size());
        CHECK(cooked.tracks.Size() + report.warnings.Size() <= source.tracks.Size());
        for (const ClipPlaybackTrack& track : cooked.tracks) {
            CHECK(track.translation.Size() == 1 || track.translation.Size() == cooked.sampleCount);
            CHECK(track.rotation.Size() == 1 || track.rotation.Size() == cooked.sampleCount);
            CHECK(track.scale.Size() == 1 || track.scale.Size() == cooked.sampleCount);
        }

        if (input.targetRate == source.frameRate) {
            CHECK(cooked.sampleCount == source.KeyCount());
        }

        ClipPlayback again(env.allocator);
        REQUIRE(BuildClipPlayback(input, again, report));
        CHECK(SamePlayback(cooked, again));
    }

    printf("Clip_FuzzSources: %u valid, %u invalid, %u incompatible\n", valid, invalid,
           incompatible);
    CHECK(valid > 100 && invalid > 20 && incompatible > 10);
}

} // namespace clip_robustness_tests_file
