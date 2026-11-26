#include <Animation/Tests/ClipTestSupport.h>

#include <AnimationTools/AnimationClipImport.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

// 애니메이션 클립 임포트 서비스 시험이다(Docs/Design/MeshAnimationAssets 7.6.2절): 필드
// 채우기(원본 bulk가 출력 bulk로 — 이름 순 정렬, 커브·변환 커브 복사, 이벤트·마커 정렬, 설정·플랫폼
// 비율·참조, playback 비움), 경고 셋(UnknownBoneTrack·EventPropertiesDropped·UnsupportedSetting),
// 검증 실패의 코드·detail(저장 전 — bulk를 넘기지 않음), 재임포트 보존과 replaceSettingsOnReimport.
using namespace rocks;
namespace animation_clip_import_tests_file {

using namespace rocks::clip_test;

namespace {

constexpr AssetId SkeletonAsset{Guid{0x534b454c00000000ull, 31}};
constexpr AssetId BaseClipAsset{Guid{0x434c495000000000ull, 32}};
// 시험 원본의 프레임 수다(30 fps 1초).
constexpr uint32 TestFrames = 30;
// 시험 원본 트랙 셋(spine, root, Tail) 가운데 스켈레톤에 없는 Tail의 명세 번호다.
constexpr uint32 TailSpecTrack = 2;
// 시험 원본의 트랙 수와 트랙마다의 성분(bulk) 수다.
constexpr uint32 TestTracks = 3;
constexpr uint32 ComponentsPerTrack = 3;
constexpr AssetId MissingAsset{Guid{0x4d49535300000000ull, 33}};

// 가짜 임포트 문맥이다(출력은 미리 만든 클립, bulk는 메모리, OpenAsset은 표, 경고 기록).
class FakeContext final : public AssetImportContext {
public:
    FakeContext(ObjectRegistry& registry, ObjectHandle output, Allocator& allocator,
                bool reimport = false) noexcept
        : bulk(allocator), warnings(allocator), registry_(registry),
          access_(registry.OwnerAccess()), output_(output), reimport_(reimport) {
    }

    Span<const AssetImportSource> Sources() const noexcept override {
        return {};
    }

    const void* Options() const noexcept override {
        return nullptr;
    }

    bool IsReimport() const noexcept override {
        return reimport_;
    }

    ObjectRegistry& Registry() noexcept override {
        return registry_;
    }

    const ObjectAccessContext& Access() const noexcept override {
        return access_;
    }

    Result<ObjectHandle> Output(StringView, TypeId) noexcept override {
        return Result<ObjectHandle>::Success(output_);
    }

    Result<BulkDataRef> AddBulk(ObjectHandle target, Array<uint8>&& raw) noexcept override {
        if (target != output_) {
            return Result<BulkDataRef>::Failure(ErrorCode::InvalidArgument);
        }

        ++addedBulks;
        return bulk.Add(Move(raw));
    }

    Result<void> Warn(AssetDiagnosticCode code, uint64 detail) noexcept override {
        return warnings.TryPushBack(Warning{code, detail});
    }

    // 첫 진단만 남긴다(서비스는 실패마다 Fail을 한 번 부른다).
    Result<void> Fail(AssetDiagnosticCode code, ErrorCode error, uint64 detail) noexcept override {
        ++failCount;
        if (failCount == 1) {
            failCode = code;
            failDetail = detail;
        }

        return Result<void>::Failure(error);
    }

    Result<void> AddTag(StringView, StringView) noexcept override {
        return Result<void>::Success();
    }

    Span<const ImportTag> PreviousTags() const noexcept override {
        return {};
    }

    Result<ObjectHandle> OpenAsset(AssetId asset) noexcept override {
        for (usize index = 0; index < count; ++index) {
            if (ids[index] == asset) {
                return Result<ObjectHandle>::Success(handles[index]);
            }
        }

        (void)Fail(AssetDiagnosticCode::MissingDependency, ErrorCode::NotFound, 0);
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    Result<ObjectHandle> EditAsset(AssetId) noexcept override {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    usize GroupSize() const noexcept override {
        return 1;
    }

    usize GroupIndex() const noexcept override {
        return 0;
    }

    AssetId MemberAsset(usize) const noexcept override {
        return {};
    }

    void Add(AssetId id, ObjectHandle handle) noexcept {
        ids[count] = id;
        handles[count] = handle;
        ++count;
    }

    uint64 WarningDetail(AssetDiagnosticCode code) const noexcept {
        for (const Warning& warning : warnings) {
            if (warning.code == code) {
                return warning.detail;
            }
        }

        return NumericLimits<uint64>::Max();
    }

    struct Warning {
        AssetDiagnosticCode code = AssetDiagnosticCode::None;
        uint64 detail = 0;
    };

    MemoryBulkSource bulk;
    Array<Warning> warnings;
    uint32 addedBulks = 0;
    uint32 failCount = 0;
    AssetDiagnosticCode failCode = AssetDiagnosticCode::None;
    uint64 failDetail = 0;
    AssetId ids[4];
    ObjectHandle handles[4];
    usize count = 0;

private:
    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    ObjectHandle output_;
    bool reimport_;
};

// 키 데이터(트랙 셋 — spine, root(회전 상수), Tail)와 명세다.
struct SpecData {
    explicit SpecData(Allocator& allocator) noexcept
        : blink(allocator),
          channels{TangentCurve(allocator), TangentCurve(allocator), TangentCurve(allocator),
                   TangentCurve(allocator), TangentCurve(allocator), TangentCurve(allocator),
                   TangentCurve(allocator), TangentCurve(allocator), TangentCurve(allocator)} {
        const StringView names[TestTracks] = {"spine", "root", "Tail"};
        for (uint32 track = 0; track < TestTracks; ++track) {
            for (uint32 key = 0; key <= TestFrames; ++key) {
                translations[track][key] = TranslationKey(track, key);
                rotations[track][key] = RotationKey(track, key);
            }

            scales[track] = Vec3(1.0f, 1.0f, 1.0f);
            const usize rotationCount = track == 1 ? 1 : TestFrames + 1;
            tracks[track] =
                ClipImportTrack{names[track], Span<const Vec3>(translations[track], TestFrames + 1),
                                Span<const Quat>(rotations[track], rotationCount),
                                Span<const Vec3>(&scales[track], 1)};
        }

        (void)blink.TryAddKey(0.0f, 1.0f);
        (void)blink.TryAddKey(1.0f, 0.0f);
        curves[0] = ClipImportCurve{"Blink", true, false, false, &blink};
        transform[0] = ClipImportTransformCurve{"Layer", true, false, true,
                                                Span<const TangentCurve>(channels)};
        events[0] = ClipImportEvent{0.75f, 0.0f, 1, "Late", "Notify", 1.0f, true, false, 0};
        events[1] = ClipImportEvent{0.25f, 0.5f, 0, "Early", "NotifyState", 0.5f, false, true, 2};
        markers[0] = ClipImportSyncMarker{"Right", 0.6f, 0};
        markers[1] = ClipImportSyncMarker{"Left", 0.1f, 0};
        rates[0] = ClipImportRate{"Mobile", FrameRate::TryCreate(15, 1).Value()};
        spec.frameRate = FrameRate::TryCreate(30, 1).Value();
        spec.frameCount = TestFrames;
        spec.tracks = tracks;
        spec.curves = curves;
        spec.transformCurves = transform;
        spec.events = events;
        spec.syncMarkers = markers;
        spec.eventTracks = eventTracks;
        spec.interpolation = ClipInterpolation::Step;
        spec.rateScale = 1.5f;
        spec.loop = true;
        spec.rootMotion.enable = true;
        spec.additive.type = AdditiveType::LocalSpace;
        spec.additive.basePose = AdditiveBasePose::Frame;
        spec.additive.basePoseClip = BaseClipAsset;
        spec.additive.basePoseFrame = 2;
        spec.targetFrameRatePerPlatform = rates;
        spec.retargetSource = "TPose";
        spec.skeleton = SkeletonAsset;
        spec.attributeCount = 3;
    }

    Vec3 translations[TestTracks][TestFrames + 1];
    Quat rotations[TestTracks][TestFrames + 1];
    Vec3 scales[TestTracks];
    ClipImportTrack tracks[TestTracks];
    TangentCurve blink;
    TangentCurve channels[ClipTransformCurveChannels];
    ClipImportCurve curves[1];
    ClipImportTransformCurve transform[1];
    ClipImportEvent events[2];
    ClipImportSyncMarker markers[2];
    StringView eventTracks[2] = {"Feet", "Sound"};
    ClipImportRate rates[1];
    AnimationClipImportSpec spec;
};

struct ImportScene {
    ImportScene() noexcept {
        ready = env.Ready() && RegisterAnimationTypes(env.modules).IsOk();
        ObjectHandle package{};
        clip = ready ? MakeClip(env, 0x7300, PackageKind::Editor, package) : ObjectHandle{};
        baseClip = ready ? MakeClip(env, 0x7301, PackageKind::Editor, package) : ObjectHandle{};
        skeleton = ready ? MakeSkeleton(env, 0x7302) : ObjectHandle{};
        ready = ready && !clip.IsNull() && !baseClip.IsNull() && !skeleton.IsNull() &&
                FillHumanSkeleton(env.names, *asset_test::Pointer<Skeleton>(*env.source, skeleton));
    }

    void AddReferences(FakeContext& context) noexcept {
        context.Add(SkeletonAsset, skeleton);
        context.Add(BaseClipAsset, baseClip);
    }

    AnimationClip* Clip() noexcept {
        return asset_test::Pointer<AnimationClip>(*env.source, clip);
    }

    Result<void> Import(const AnimationClipImportSpec& spec, FakeContext& context,
                        ClipDiagnostic* failure = nullptr) noexcept {
        AddReferences(context);
        return ImportAnimationClip(context, clip, spec, failure);
    }

    asset_test::AssetEnv env;
    ObjectHandle clip{};
    ObjectHandle baseClip{};
    ObjectHandle skeleton{};
    bool ready = false;
};

} // namespace

// 필드 채우기: 원본 bulk(트랙 이름 순 — root, spine, Tail)가 출력 bulk에서 같은 원본으로 로드되고,
// 커브· 변환 커브 복사, 이벤트·마커 정렬, 설정·플랫폼 비율·참조, playback 비움, 경고 셋.
TEST(AnimationClipImport_FillsFields) {
    ImportScene scene;
    REQUIRE(scene.ready);
    SpecData data(scene.env.allocator);
    FakeContext context(*scene.env.source, scene.clip, scene.env.allocator);
    REQUIRE(scene.Import(data.spec, context));
    CHECK(context.failCount == 0 && context.addedBulks == TestTracks * ComponentsPerTrack);
    CHECK(context.WarningDetail(AssetDiagnosticCode::UnknownBoneTrack) == TailSpecTrack &&
          context.WarningDetail(AssetDiagnosticCode::EventPropertiesDropped) == 2 &&
          context.WarningDetail(AssetDiagnosticCode::UnsupportedSetting) == 3);
    const AnimationClip* clip = scene.Clip();
    REQUIRE(clip != nullptr);
    CHECK(clip->source.frameCount == TestFrames && clip->source.boneTracks.Size() == 3 &&
          clip->source.boneTracks[0].bone == Intern(scene.env.names, "root"));
    ClipSource loaded(scene.env.allocator);
    REQUIRE(LoadClipSource(clip->source, context.bulk, scene.env.names, loaded));
    CHECK(loaded.tracks[0].rotation.Size() == 1 &&
          loaded.tracks[1].translation.Size() == TestFrames + 1 &&
          loaded.tracks[1].translation[3] == TranslationKey(0, 3));
    REQUIRE(clip->source.curves.Size() == 1 && clip->source.transformCurves.Size() == 1);
    CHECK(clip->source.curves[0].curve.KeyCount() == 2 &&
          clip->source.transformCurves[0].disabled &&
          clip->source.transformCurves[0].channels.Size() == ClipTransformCurveChannels);
    REQUIRE(clip->events.Size() == 2 && clip->syncMarkers.Size() == 2);
    CHECK(clip->events[0].name == Intern(scene.env.names, "Early") &&
          clip->events[0].duration == 0.5f && !clip->events[0].triggerOnDedicatedServer &&
          clip->syncMarkers[0].name == Intern(scene.env.names, "Left"));
    CHECK(clip->eventTracks.Size() == 2 && clip->interpolation == ClipInterpolation::Step &&
          clip->rateScale == 1.5f && clip->loop && clip->rootMotion.enable &&
          clip->additive.basePoseFrame == 2 &&
          clip->additive.basePoseClip.Handle() == scene.baseClip &&
          clip->skeleton.Handle() == scene.skeleton &&
          clip->retargetSource == Intern(scene.env.names, "TPose"));
    CHECK(clip->targetFrameRate.Resolve(Intern(scene.env.names, "Mobile")) == Rate(15) &&
          clip->targetFrameRate.Resolve(Name{}) == Rate(30) && clip->playback.sampleCount == 0);
}

// 검증 실패(코드·detail, 저장 전 — bulk를 넘기지 않는다): output이 클립이 아님, 스켈레톤 id 없음,
// 원본 위반(회전 비정규 — detail = 트랙), 커브 null, 빈·겹친 플랫폼 키, 스켈레톤·기준 클립 타입,
// 이벤트가 길이 밖(클립 검증).
TEST(AnimationClipImport_Rejects) {
    ImportScene scene;
    REQUIRE(scene.ready);
    const auto rejects = [&](const AnimationClipImportSpec& spec, AssetDiagnosticCode code,
                             uint64 detail, ObjectHandle output = {}) noexcept {
        FakeContext context(*scene.env.source, output.IsNull() ? scene.clip : output,
                            scene.env.allocator);
        scene.AddReferences(context);
        const bool failed =
            !ImportAnimationClip(context, output.IsNull() ? scene.clip : output, spec);
        return failed && context.failCode == code && context.failDetail == detail &&
               context.failCount == 1 && context.addedBulks == 0;
    };
    SpecData data(scene.env.allocator);
    CHECK(rejects(data.spec, AssetDiagnosticCode::TypeMismatch, 0, scene.skeleton));
    AnimationClipImportSpec noSkeleton = data.spec;
    noSkeleton.skeleton = AssetId{};
    CHECK(rejects(noSkeleton, AssetDiagnosticCode::InvalidSetting, 0));

    SpecData bent(scene.env.allocator);
    bent.rotations[2][4] = Quat(2.0f, 0.0f, 0.0f, 0.0f);
    CHECK(rejects(bent.spec, AssetDiagnosticCode::InvalidClipData, 2));

    SpecData nullCurve(scene.env.allocator);
    nullCurve.curves[0].curve = nullptr;
    CHECK(rejects(nullCurve.spec, AssetDiagnosticCode::InvalidSetting, 0));

    SpecData keys(scene.env.allocator);
    const ClipImportRate twice[2] = {{"Mobile", Rate(15)}, {"Mobile", Rate(10)}};
    keys.spec.targetFrameRatePerPlatform = twice;
    CHECK(rejects(keys.spec, AssetDiagnosticCode::InvalidSetting, 1));
    const ClipImportRate empty[1] = {{"", Rate(15)}};
    keys.spec.targetFrameRatePerPlatform = empty;
    CHECK(rejects(keys.spec, AssetDiagnosticCode::InvalidSetting, 0));

    AnimationClipImportSpec wrongSkeleton = data.spec;
    wrongSkeleton.skeleton = BaseClipAsset;
    CHECK(rejects(wrongSkeleton, AssetDiagnosticCode::TypeMismatch, 0));
    AnimationClipImportSpec wrongBase = data.spec;
    wrongBase.additive.basePoseClip = SkeletonAsset;
    CHECK(rejects(wrongBase, AssetDiagnosticCode::TypeMismatch, 1));

    SpecData late(scene.env.allocator);
    late.events[0].time = 5.0f;
    CHECK(rejects(late.spec, AssetDiagnosticCode::InvalidClipData, 1));

    // 없는 참조는 문맥의 MissingDependency다.
    AnimationClipImportSpec missingSkeleton = data.spec;
    missingSkeleton.skeleton = MissingAsset;
    CHECK(rejects(missingSkeleton, AssetDiagnosticCode::MissingDependency, 0));
    AnimationClipImportSpec missingBase = data.spec;
    missingBase.additive.basePoseClip = MissingAsset;
    CHECK(rejects(missingBase, AssetDiagnosticCode::MissingDependency, 0));

    // 변환 커브 채널이 9개가 아니면 클립 검증이 TransformCurveChannels로 거부한다(failure 세부).
    SpecData channels(scene.env.allocator);
    channels.transform[0].channels = Span<const TangentCurve>(channels.channels, 2);
    {
        FakeContext context(*scene.env.source, scene.clip, scene.env.allocator);
        ClipDiagnostic failure;
        CHECK(!scene.Import(channels.spec, context, &failure));
        CHECK(context.failCode == AssetDiagnosticCode::InvalidClipData &&
              failure.issue == ClipIssue::TransformCurveChannels && context.addedBulks == 0);
    }

    // 플랫폼 키가 None으로 접히는 문자열("None")이면 InvalidSetting이다.
    SpecData none(scene.env.allocator);
    const ClipImportRate noneKey[1] = {{"None", Rate(15)}};
    none.spec.targetFrameRatePerPlatform = noneKey;
    CHECK(rejects(none.spec, AssetDiagnosticCode::InvalidSetting, 0));
}

// 새 임포트는 옛 값을 남기지 않는다(채워진 클립에 재임포트가 아닌 임포트 — 이벤트·마커·이벤트 트랙·
// 커브·기준 클립·플랫폼 덮어쓰기 없음).
TEST(AnimationClipImport_NewImportReplaces) {
    ImportScene scene;
    REQUIRE(scene.ready);
    SpecData full(scene.env.allocator);
    {
        FakeContext context(*scene.env.source, scene.clip, scene.env.allocator);
        REQUIRE(scene.Import(full.spec, context));
    }

    SpecData bare(scene.env.allocator);
    bare.spec.curves = {};
    bare.spec.transformCurves = {};
    bare.spec.events = {};
    bare.spec.syncMarkers = {};
    bare.spec.eventTracks = {};
    bare.spec.additive = ClipImportAdditive{};
    bare.spec.targetFrameRatePerPlatform = {};
    {
        FakeContext context(*scene.env.source, scene.clip, scene.env.allocator);
        REQUIRE(scene.Import(bare.spec, context));
    }

    const AnimationClip* clip = scene.Clip();
    REQUIRE(clip != nullptr);
    CHECK(clip->events.IsEmpty() && clip->syncMarkers.IsEmpty() && clip->eventTracks.IsEmpty() &&
          clip->source.curves.IsEmpty() && clip->source.transformCurves.IsEmpty() &&
          clip->additive.basePoseClip.Handle().IsNull() &&
          clip->targetFrameRate.overrides.IsEmpty());
}

// 재임포트: 이벤트·마커·이벤트 트랙·재생 설정은 보존하고 원본·커브는 새 값이다. 보존한 이벤트가 새
// 길이를 넘으면 InvalidClipData로 실패하고, replaceSettingsOnReimport면 명세 값이다.
TEST(AnimationClipImport_ReimportPreservesEdits) {
    ImportScene scene;
    REQUIRE(scene.ready);
    SpecData data(scene.env.allocator);
    {
        FakeContext context(*scene.env.source, scene.clip, scene.env.allocator);
        REQUIRE(scene.Import(data.spec, context));
    }

    // 사용자 편집.
    AnimationClip* clip = scene.Clip();
    REQUIRE(clip != nullptr);
    clip->rateScale = 2.5f;
    clip->loop = false;
    clip->events[1].triggerChance = 0.25f;
    // 편집기에서 만든 커브(명세에 없는 이름)는 재임포트에서 남는다.
    ClipCurve authored(scene.env.allocator);
    REQUIRE(MakeCurve(scene.env.names, "Authored", authored));
    REQUIRE(clip->source.curves.TryPushBack(Move(authored)));

    // 명세 설정을 바꾸고 기준 클립을 없는 id로 둬도(보존 모드는 열지 않는다) 성공한다.
    SpecData next(scene.env.allocator);
    next.spec.rateScale = 0.5f;
    next.spec.interpolation = ClipInterpolation::Linear;
    next.spec.retargetSource = "Other";
    next.spec.additive.basePoseClip = MissingAsset;
    next.spec.targetFrameRatePerPlatform = {};
    next.spec.syncMarkers = {};
    next.spec.eventTracks = {};
    next.translations[0][5] = Vec3(99.0f, 0.0f, 0.0f);
    {
        FakeContext context(*scene.env.source, scene.clip, scene.env.allocator, true);
        REQUIRE(scene.Import(next.spec, context));
        ClipSource loaded(scene.env.allocator);
        REQUIRE(LoadClipSource(clip->source, context.bulk, scene.env.names, loaded));
        CHECK(loaded.tracks[1].translation[5] == Vec3(99.0f, 0.0f, 0.0f));
    }

    CHECK(
        clip->rateScale == 2.5f && !clip->loop && clip->events[1].triggerChance == 0.25f &&
        clip->source.boneTracks.Size() == TestTracks &&
        clip->interpolation == ClipInterpolation::Step && clip->syncMarkers.Size() == 2 &&
        clip->eventTracks.Size() == 2 && clip->retargetSource == Intern(scene.env.names, "TPose") &&
        clip->additive.basePoseClip.Handle() == scene.baseClip &&
        clip->targetFrameRate.overrides.Count() == 1 && clip->skeleton.Handle() == scene.skeleton);
    // 커브: 명세 Blink와 편집기 Authored.
    REQUIRE(clip->source.curves.Size() == 2);
    CHECK(clip->source.curves[0].name == Intern(scene.env.names, "Blink") &&
          clip->source.curves[1].name == Intern(scene.env.names, "Authored"));

    // 짧은 원본(10프레임): 보존한 0.75초 이벤트가 길이 밖이다.
    SpecData shorter(scene.env.allocator);
    shorter.spec.frameCount = 10;
    for (ClipImportTrack& track : shorter.tracks) {
        track.translation = Span<const Vec3>(track.translation.Data(), 11);
        if (track.rotation.Size() != 1) {
            track.rotation = Span<const Quat>(track.rotation.Data(), 11);
        }
    }

    {
        FakeContext context(*scene.env.source, scene.clip, scene.env.allocator, true);
        CHECK(!scene.Import(shorter.spec, context) &&
              context.failCode == AssetDiagnosticCode::InvalidClipData);
    }

    // 명세 값으로 바꾸면(이벤트도 짧은 원본에 맞춘다) 성공한다.
    shorter.events[0].time = 0.2f;
    shorter.events[1].time = 0.1f;
    shorter.events[1].duration = 0.1f;
    shorter.markers[0].time = 0.3f;
    shorter.spec.replaceSettingsOnReimport = true;
    {
        FakeContext context(*scene.env.source, scene.clip, scene.env.allocator, true);
        REQUIRE(scene.Import(shorter.spec, context));
    }

    clip = scene.Clip();
    REQUIRE(clip != nullptr);
    CHECK(clip->rateScale == 1.5f && clip->loop && clip->source.frameCount == 10 &&
          clip->events[1].triggerChance == 1.0f);
}

} // namespace animation_clip_import_tests_file
