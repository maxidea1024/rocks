#include <Animation/Tests/ClipTestSupport.h>

#include <AnimationTools/AnimationClipCook.h>
#include <AnimationTools/AnimationTypeHooks.h>
#include <AnimationTools/SkeletonMergeApply.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/PackageWriter.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

// 애니메이션 클립 쿠킹 시험이다(Docs/Design/MeshAnimationAssets 7.6.1절): 쿠킹 패키지(EditorOnly
// 원본 없음, playback이 스켈레톤 대조 로드 검증 통과, 원본 이름 순 트랙의 본 번호), 프로파일별 목표
// 비율 재샘플링, 경고(알 수 없는 트랙·변환 커브), 전용 서버 프로파일, 실패(스켈레톤 없음·클립
// 검증·원본 손상·타입)와 에디터 객체 되돌림, 스켈레톤 병합 뒤 재쿠킹, Build hook, 쿠킹 로드 검증
// 음성(본 번호 범위·중복), 처리기 등록.
using namespace rocks;
namespace animation_clip_cook_tests_file {

using namespace rocks::clip_test;

namespace {

constexpr uint64 SkeletonPackageId = 0x7100;
constexpr uint64 ClipPackageId = 0x7101;
// 원본 프레임 수다(30 fps 1초 — FillClipSettings의 이벤트가 길이 안에 들도록).
constexpr uint32 SourceFrames = 30;
// 원본 트랙의 이름 순(hand_l, pelvis, root, spine, Tail, VB spine_head)에서 Tail의 번호다.
constexpr uint32 TailTrack = 4;

struct CookOut {
    explicit CookOut(Allocator& allocator) noexcept : sink(allocator), warnings(allocator) {
    }

    MemoryPackageSink sink;
    AssetDiagnostic failure;
    Array<CookDiagnostic> warnings;
};

// 스켈레톤 패키지와 클립 패키지(사람 원본 — 스켈레톤에 없는 Tail·가상 본 트랙 포함)다.
struct CookScene {
    explicit CookScene(StringView backend = "D3D12") noexcept
        : sourceBulk(env.allocator), profile(env.allocator), entry(env.allocator) {
        ready = env.Ready() && RegisterAnimationTypes(env.modules).IsOk();
        skeleton = ready ? MakeSkeleton(env, SkeletonPackageId) : ObjectHandle{};
        clip =
            ready ? MakeClip(env, ClipPackageId, PackageKind::Editor, clipPackage) : ObjectHandle{};
        ready =
            ready && !skeleton.IsNull() && !clip.IsNull() &&
            FillHumanSkeleton(env.names, *asset_test::Pointer<Skeleton>(*env.source, skeleton)) &&
            FillRetargetModes() && static_cast<bool>(profile.id.TryAssign("Windows"));
        auto parent = env.source->GetOuter(env.source->OwnerAccess(), skeleton);
        skeletonPackage = parent ? parent.Value() : ObjectHandle{};
        if (ready && !backend.IsEmpty()) {
            String name(env.allocator);
            ready = static_cast<bool>(name.TryAssign(backend)) &&
                    static_cast<bool>(profile.graphicsBackends.TryPushBack(Move(name)));
        }

        ClipSource source(env.allocator);
        AnimationClip* value = Clip();
        ready = ready && value != nullptr && BuildHumanSource(env.names, source, SourceFrames) &&
                StoreClipSource(source, sourceBulk, env.names, value->source).IsOk() &&
                FillClipSettings(env.names, *value) && AddBlinkCurve(*value) &&
                env.source->SetReference(env.source->OwnerAccess(), clip, value->skeleton, skeleton)
                    .IsOk();
    }

    // 본마다 리타깃 방식 Animation을 둔다(병합·검증이 본 수와 같은 길이를 요구한다).
    bool FillRetargetModes() noexcept {
        Skeleton* value = asset_test::Pointer<Skeleton>(*env.source, skeleton);
        for (usize bone = 0; value != nullptr && bone < value->bones.bones.Size(); ++bone) {
            if (!value->retargetModes.TryPushBack(BoneTranslationRetarget::Animation)) {
                return false;
            }
        }

        return value != nullptr;
    }

    // 원본 커브 하나(Blink — 쿠킹이 그대로 복사한다)를 더한다.
    bool AddBlinkCurve(AnimationClip& value) noexcept {
        ClipCurve blink(env.allocator);
        return MakeCurve(env.names, "Blink", blink) &&
               value.source.curves.TryPushBack(Move(blink)).IsOk();
    }

    AnimationClip* Clip() noexcept {
        return asset_test::Pointer<AnimationClip>(*env.source, clip);
    }

    Skeleton* SkeletonValue() noexcept {
        return asset_test::Pointer<Skeleton>(*env.source, skeleton);
    }

    CookContext Context(ObjectHandle package, uint64 id, CookOut& out) noexcept {
        entry.id = asset_test::FixedId(id);
        CookContext context;
        context.allocator = &env.allocator;
        context.registry = env.source.Get();
        context.access = env.source->OwnerAccess();
        context.package = package;
        context.entry = &entry;
        context.profile = &profile;
        context.cookerVersion = 1;
        context.bulkSource = &sourceBulk;
        context.sink = &out.sink;
        context.failure = &out.failure;
        context.warnings = &out.warnings;
        return context;
    }

    Result<void> Cook(const AnimationClipCookProcessor& processor, CookOut& out) noexcept {
        CookContext context = Context(clipPackage, ClipPackageId, out);
        return processor.Cook(context);
    }

    // 스켈레톤(기본 처리기)과 클립을 쿠킹해 target에 함께 로드한다.
    bool CookAndLoad(const AnimationClipCookProcessor& processor, CookOut& out) noexcept {
        CookOut skeletonOut(env.allocator);
        DefaultCookProcessor fallback;
        CookContext skeletonContext = Context(skeletonPackage, SkeletonPackageId, skeletonOut);
        if (!fallback.Cook(skeletonContext) || !Cook(processor, out)) {
            return false;
        }

        Array<uint8> files[2] = {asset_test::Copy(env.allocator, skeletonOut.sink.Bytes()),
                                 asset_test::Copy(env.allocator, out.sink.Bytes())};
        RootRef<Object> roots[2];
        ObjectHandle handles[2];
        if (!asset_test::LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots,
                                 handles)) {
            return false;
        }

        loadedRoots[0] = Move(roots[0]);
        loadedRoots[1] = Move(roots[1]);
        loadedClipPackage = handles[1];
        auto foundSkeleton =
            env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
        auto foundClip =
            env.target->FindObject(env.target->OwnerAccess(), handles[1], StringView());
        loadedSkeleton = foundSkeleton ? foundSkeleton.Value() : ObjectHandle{};
        loadedClip = foundClip ? foundClip.Value() : ObjectHandle{};
        return !loadedSkeleton.IsNull() && !loadedClip.IsNull();
    }

    // 로드한 쿠킹 클립을 스켈레톤과 대조한다.
    Result<void> ValidateLoaded(ClipPlayback& playback, ClipDiagnostic* failure = nullptr,
                                const Skeleton* against = nullptr) noexcept {
        const AnimationClip* value = asset_test::Pointer<AnimationClip>(*env.target, loadedClip);
        const Package* package = asset_test::Pointer<Package>(*env.target, loadedClipPackage);
        const Skeleton* target = against != nullptr
                                     ? against
                                     : asset_test::Pointer<Skeleton>(*env.target, loadedSkeleton);
        if (value == nullptr || package == nullptr || target == nullptr) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        return ValidateCookedAnimationClip(*value, *package, *target, playback, failure);
    }

    bool EditorUntouched() noexcept {
        const AnimationClip* value = Clip();
        return value != nullptr && value->playback.sampleCount == 0 &&
               value->playback.trackToBone.IsEmpty() && value->playback.tracks.IsEmpty();
    }

    asset_test::AssetEnv env;
    MemoryBulkSource sourceBulk;
    TargetProfile profile;
    AssetIndexEntry entry;
    ObjectHandle skeleton{};
    ObjectHandle skeletonPackage{};
    ObjectHandle clip{};
    ObjectHandle clipPackage{};
    RootRef<Object> loadedRoots[2];
    ObjectHandle loadedClipPackage{};
    ObjectHandle loadedClip{};
    ObjectHandle loadedSkeleton{};
    bool ready = false;
};

// 쓰기가 늘 실패하는 출력이다(쓰기 Borrow 뒤 실패의 되돌림 시험).
class FailingSink final : public PackageSink {
public:
    Result<void> Append(Span<const uint8>) noexcept override {
        return Result<void>::Failure(ErrorCode::IoError);
    }

    Result<void> WriteAt(uint64, Span<const uint8>) noexcept override {
        return Result<void>::Failure(ErrorCode::IoError);
    }
};

usize CountWarnings(const CookOut& out, AssetDiagnosticCode code) noexcept {
    usize count = 0;
    for (const CookDiagnostic& warning : out.warnings) {
        count += warning.code == code && warning.warning ? 1 : 0;
    }

    return count;
}

} // namespace

// 쿠킹 패키지: EditorOnly 원본 없음, playback(30 fps 기본 비율 — 표본 31, 트랙은 원본 이름 순에서
// 스켈레톤 본만 hand_l(5)·pelvis(1)·root(0)·spine(2))이 스켈레톤 대조 로드 검증을 통과, Tail은
// UnknownBoneTrack 경고(setting = 4), 가상 본 트랙은 경고 없이 빠짐, 에디터 객체 되돌림.
TEST(AnimationClipCook_CookedPackage) {
    CookScene scene;
    REQUIRE(scene.ready);
    AnimationClipCookProcessor processor;
    CookOut out(scene.env.allocator);
    REQUIRE(scene.CookAndLoad(processor, out));
    CHECK(processor.Builds() == 1 && out.warnings.Size() == 1);
    CHECK(CountWarnings(out, AssetDiagnosticCode::UnknownBoneTrack) == 1 &&
          out.warnings[0].setting == TailTrack);
    ClipPlayback playback(scene.env.allocator);
    REQUIRE(scene.ValidateLoaded(playback));
    CHECK(playback.sampleRate == Rate(30) && playback.sampleCount == SourceFrames + 1);
    const uint16 bones[] = {5, 1, 0, 2};
    REQUIRE(playback.trackToBone.Size() == 4);
    for (uint32 at = 0; at < 4; ++at) {
        CHECK(playback.trackToBone[at] == bones[at]);
    }

    // 상수 접기: root(트랙 2)의 회전은 1키, 크기는 모든 트랙 1키, 이동은 표본 수 그대로. 커브는
    // 복사.
    CHECK(playback.tracks[2].rotation.Size() == 1 &&
          playback.tracks[0].translation.Size() == SourceFrames + 1);
    for (const ClipPlaybackTrack& track : playback.tracks) {
        CHECK(track.scale.Size() == 1);
    }

    REQUIRE(playback.curves.Size() == 1);
    CHECK(playback.curves[0].name == Intern(scene.env.names, "Blink"));
    const AnimationClip* loaded =
        asset_test::Pointer<AnimationClip>(*scene.env.target, scene.loadedClip);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->source.boneTracks.IsEmpty() && loaded->events.Size() == 2 && loaded->loop);
    CHECK(scene.EditorUntouched());
}

// 대상 프로파일 목표 비율: Mobile 덮어쓰기 15 fps면 표본 16개로 다시 샘플링한다.
TEST(AnimationClipCook_ProfileTargetRate) {
    CookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.profile.id.TryAssign("Mobile"));
    AnimationClipCookProcessor processor;
    CookOut out(scene.env.allocator);
    REQUIRE(scene.CookAndLoad(processor, out));
    ClipPlayback playback(scene.env.allocator);
    REQUIRE(scene.ValidateLoaded(playback));
    CHECK(playback.sampleRate == Rate(15) && playback.sampleCount == SourceFrames / 2 + 1);
}

// 경고: 사용 중 변환 커브는 UnsupportedSetting(setting = 수 — disabled는 세지 않는다). 전용 서버
// 프로파일(backend 없음)도 playback을 쓴다.
TEST(AnimationClipCook_WarningsAndServer) {
    CookScene scene("");
    REQUIRE(scene.ready);
    AnimationClip* clip = scene.Clip();
    REQUIRE(clip != nullptr);
    ClipTransformCurve used(scene.env.allocator);
    used.name = Intern(scene.env.names, "Layer");
    ClipTransformCurve disabled(scene.env.allocator);
    disabled.name = Intern(scene.env.names, "Off");
    disabled.disabled = true;
    for (uint32 channel = 0; channel < ClipTransformCurveChannels; ++channel) {
        REQUIRE(used.channels.TryPushBack(TangentCurve(scene.env.allocator)));
        REQUIRE(disabled.channels.TryPushBack(TangentCurve(scene.env.allocator)));
    }

    REQUIRE(clip->source.transformCurves.TryPushBack(Move(used)));
    REQUIRE(clip->source.transformCurves.TryPushBack(Move(disabled)));
    AnimationClipCookProcessor processor;
    CookOut out(scene.env.allocator);
    REQUIRE(scene.CookAndLoad(processor, out));
    REQUIRE(CountWarnings(out, AssetDiagnosticCode::UnsupportedSetting) == 1);
    CHECK(out.warnings[out.warnings.Size() - 1].setting == 1);
    ClipPlayback playback(scene.env.allocator);
    REQUIRE(scene.ValidateLoaded(playback));
    CHECK(playback.trackToBone.Size() == 4);
}

// 실패: 스켈레톤 없음 MissingDependency, 클립 검증 위반 InvalidClipData, 원본 bulk 손상
// CorruptPackage(detail = 트랙), 클립이 아닌 패키지 TypeMismatch. 에디터 객체는 그대로다.
TEST(AnimationClipCook_Failures) {
    AnimationClipCookProcessor processor;
    {
        CookScene scene;
        REQUIRE(scene.ready);
        REQUIRE(scene.env.source->SetReference(scene.env.source->OwnerAccess(), scene.clip,
                                               scene.Clip()->skeleton, ObjectHandle{}));
        CookOut out(scene.env.allocator);
        CHECK(!scene.Cook(processor, out) &&
              out.failure.code == AssetDiagnosticCode::MissingDependency);
        CHECK(scene.EditorUntouched());
    }

    {
        CookScene scene;
        REQUIRE(scene.ready);
        scene.Clip()->rateScale = ClipNaN();
        CookOut out(scene.env.allocator);
        CHECK(!scene.Cook(processor, out) &&
              out.failure.code == AssetDiagnosticCode::InvalidClipData);
        CHECK(scene.EditorUntouched());
    }

    {
        CookScene scene;
        REQUIRE(scene.ready);
        scene.Clip()->source.boneTracks[1].rotation = BulkDataRef{};
        CookOut out(scene.env.allocator);
        CHECK(!scene.Cook(processor, out) &&
              out.failure.code == AssetDiagnosticCode::CorruptPackage && out.failure.detail == 1);
        CHECK(scene.EditorUntouched());
    }

    {
        CookScene scene;
        REQUIRE(scene.ready);
        CookOut out(scene.env.allocator);
        CookContext context = scene.Context(scene.skeletonPackage, SkeletonPackageId, out);
        CHECK(!processor.Cook(context) && out.failure.code == AssetDiagnosticCode::TypeMismatch);
    }

    CHECK(processor.Builds() == 0);

    // 쓰기 Borrow 뒤 실패(출력 쓰기 오류)도 에디터 객체의 playback을 비운다.
    {
        CookScene scene;
        REQUIRE(scene.ready);
        CookOut out(scene.env.allocator);
        FailingSink failing;
        CookContext context = scene.Context(scene.clipPackage, ClipPackageId, out);
        context.sink = &failing;
        CHECK(processor.Cook(context).Error() == ErrorCode::IoError);
        CHECK(scene.EditorUntouched() && processor.Builds() == 1);
    }
}

// 스켈레톤 병합 뒤 재쿠킹(6.7.2절 3이 옮겨 온 시험): 스켈레톤에 Tail 본을
// 더하면(MergeBonesIntoSkeleton — 스킨 메시 임포트가 하는 병합) 같은 클립의 쿠킹 결과가 Tail 트랙을
// 그 본 번호로 담고 경고가 없다. Build hook은 클립의 스켈레톤을 Build 의존으로 선언한다(쿠커가 이
// 의존으로 클립 키를 바꿔 다시 쿠킹한다 — 키 계산은 AssetCooker A4c 시험이 본다).
TEST(AnimationClipCook_RecookAfterSkeletonMerge) {
    CookScene scene;
    REQUIRE(scene.ready);
    AnimationClipCookProcessor processor;
    CookOut before(scene.env.allocator);
    REQUIRE(scene.Cook(processor, before));
    CHECK(CountWarnings(before, AssetDiagnosticCode::UnknownBoneTrack) == 1);

    // 메시 본: root → Tail.
    BoneHierarchy mesh(scene.env.allocator);
    const BoneSpec tail[] = {{"root", NoParentBone}, {"Tail", 0}};
    REQUIRE(BuildHierarchy(scene.env.names, tail, mesh));
    Skeleton* skeleton = scene.SkeletonValue();
    REQUIRE(skeleton != nullptr);
    BoneMergeResult merged(scene.env.allocator);
    REQUIRE(MergeBonesIntoSkeleton(*skeleton, mesh, {}, merged));
    REQUIRE(merged.addedBones == 1);
    const uint16 tailBone = static_cast<uint16>(skeleton->bones.bones.Size() - 1);

    CookOut after(scene.env.allocator);
    REQUIRE(scene.CookAndLoad(processor, after));
    CHECK(CountWarnings(after, AssetDiagnosticCode::UnknownBoneTrack) == 0);
    ClipPlayback playback(scene.env.allocator);
    REQUIRE(scene.ValidateLoaded(playback));
    REQUIRE(playback.trackToBone.Size() == 5);
    CHECK(playback.trackToBone[TailTrack] == tailBone);

    // Build hook: 클립 → 스켈레톤 패키지 id, 스켈레톤이 비면 의존 없음.
    AssetTypeHookTable table(scene.env.allocator, scene.env.types);
    Array<AssetTypeHookRegistration> tokens(scene.env.allocator);
    REQUIRE(RegisterAnimationTypeHooks(nullptr, table, tokens));
    {
        AssetDependencySink sink(scene.env.allocator);
        REQUIRE(table.CollectBuildDependencies(*scene.env.source, scene.env.source->OwnerAccess(),
                                               scene.clip, ClipType(),
                                               asset_test::FixedId(ClipPackageId), sink));
        REQUIRE(sink.Dependencies().Size() == 1);
        CHECK(sink.Dependencies()[0].asset == asset_test::FixedId(SkeletonPackageId) &&
              sink.Dependencies()[0].kind == PackageDependencyKind::Build);
    }

    REQUIRE(scene.env.source->SetReference(scene.env.source->OwnerAccess(), scene.clip,
                                           scene.Clip()->skeleton, ObjectHandle{}));
    AssetDependencySink empty(scene.env.allocator);
    REQUIRE(table.CollectBuildDependencies(*scene.env.source, scene.env.source->OwnerAccess(),
                                           scene.clip, ClipType(),
                                           asset_test::FixedId(ClipPackageId), empty));
    CHECK(empty.Dependencies().IsEmpty());
    CHECK(RegisterAnimationTypeHooks(nullptr, table, tokens).Error() == ErrorCode::AlreadyExists);
    tokens.Clear();
}

// 쿠킹 로드 검증 음성: 본이 모자란 스켈레톤과 대조하면 TrackBoneIndex(index = 트랙), 같은 본 둘은
// DuplicateTrackBone(related = 앞 트랙) — 모두 CorruptPackage.
TEST(AnimationClipCook_CookedValidationRejects) {
    CookScene scene;
    REQUIRE(scene.ready);
    AnimationClipCookProcessor processor;
    CookOut out(scene.env.allocator);
    REQUIRE(scene.CookAndLoad(processor, out));
    Skeleton small(scene.env.allocator);
    const BoneSpec two[] = {{"root", NoParentBone}, {"pelvis", 0}};
    REQUIRE(BuildHierarchy(scene.env.names, two, small.bones));
    ClipPlayback playback(scene.env.allocator);
    ClipDiagnostic failure;
    CHECK(!scene.ValidateLoaded(playback, &failure, &small));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.issue == ClipIssue::TrackBoneIndex && failure.index == 0);

    AnimationClip* loaded = asset_test::Pointer<AnimationClip>(*scene.env.target, scene.loadedClip);
    REQUIRE(loaded != nullptr);
    loaded->playback.trackToBone[2] = loaded->playback.trackToBone[0];
    failure = ClipDiagnostic{};
    CHECK(!scene.ValidateLoaded(playback, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.issue == ClipIssue::DuplicateTrackBone && failure.index == 2 &&
          failure.related == 0);
}

// 처리기 등록과 정체성.
TEST(AnimationClipCook_Registration) {
    CookScene scene;
    REQUIRE(scene.ready);
    DefaultCookProcessor fallback;
    CookProcessorTable table(scene.env.allocator, scene.env.types, fallback);
    AnimationClipCookProcessor processor;
    CHECK(RegisterAnimationClipCookProcessors(nullptr, table).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(RegisterAnimationClipCookProcessors(&processor, table));
    auto found = table.Find(TypeIdOf<AnimationClip>());
    REQUIRE(found);
    CHECK(found.Value() == &processor &&
          processor.Identity().processor == AnimationClipCookProcessorId.processor);
}

} // namespace animation_clip_cook_tests_file
