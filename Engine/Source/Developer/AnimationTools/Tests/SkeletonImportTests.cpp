#include <Animation/AnimationClip.generated.h>
#include <Animation/Skeleton.generated.h>
#include <AnimationTools/SkeletonImport.h>
#include <AnimationTools/SkeletonMergeApply.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/ObjectRegistryOptions.h>
#include <TestSupport/Test.h>

// 스켈레톤 임포트 서비스 시험이다(Docs/Design/MeshAnimationAssets 6.6절 8): 새 임포트 필드·pose
// 정규화·리타깃 방식 기본, 이름·길이·계층·소켓 실패(코드·detail), 재임포트 병합(기존 본 번호·pose
// 유지, 새 본 추가, 대소문자 이름 바꿈, 사슬 불일치), 편집 보존과 replaceOnReimport, 보존 필드
// 유지, MergeBonesIntoSkeleton 실패 시 불변.
using namespace rocks;
namespace skeleton_import_tests_file {

namespace {

// 가짜 임포트 문맥이다(출력은 미리 만든 객체).
class FakeContext final : public AssetImportContext {
public:
    FakeContext(ObjectRegistry& registry, ObjectHandle output, bool reimport = false) noexcept
        : registry_(registry), access_(registry.OwnerAccess()), output_(output),
          reimport_(reimport) {
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

    Result<BulkDataRef> AddBulk(ObjectHandle, Array<uint8>&&) noexcept override {
        return Result<BulkDataRef>::Failure(ErrorCode::Unsupported);
    }

    Result<void> Warn(AssetDiagnosticCode, uint64) noexcept override {
        return Result<void>::Success();
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

    Result<ObjectHandle> OpenAsset(AssetId) noexcept override {
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

    uint32 failCount = 0;
    AssetDiagnosticCode failCode = AssetDiagnosticCode::None;
    uint64 failDetail = 0;

private:
    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    ObjectHandle output_;
    bool reimport_;
};

struct Scene {
    Scene() noexcept : types(allocator), registration(types, allocator), names(allocator) {
        ready = RegisterAnimationTypes(registration).IsOk() && names.Initialize().IsOk();
        RegistryOptions options;
        options.names = &names;
        auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{0x534b454cull, 1}, options);
        if (made) {
            registry = Move(made.Value());
        }

        ready = ready && registry.Get() != nullptr && MakeSkeleton();
        if (ready) {
            auto clipMade = registry->NewObject<AnimationClip>(registry->OwnerAccess(),
                                                               TypeIdOf<AnimationClip>());
            ready = clipMade.IsOk();
            if (clipMade) {
                clip = clipMade.Value().Handle();
                clipRoot = Move(clipMade.Value());
            }
        }
    }

    ~Scene() noexcept {
        skeletonRoot.Reset();
        clipRoot.Reset();
        registry.Reset();
        CHECK(registration.Unregister());
    }

    bool MakeSkeleton() noexcept {
        auto made = registry->NewObject<Skeleton>(registry->OwnerAccess(), TypeIdOf<Skeleton>());
        if (!made) {
            return false;
        }

        skeleton = made.Value().Handle();
        skeletonRoot = Move(made.Value());
        return true;
    }

    Result<Borrow<Skeleton>> Write() noexcept {
        return registry->ResolveForWrite<Skeleton>(registry->OwnerAccess(), skeleton);
    }

    Name Intern(StringView text) noexcept {
        auto name = names.TryIntern(text);
        return name ? name.Value() : Name{};
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    UniquePtr<ObjectRegistry> registry;
    ObjectHandle skeleton{};
    ObjectHandle clip{};
    RootRef<Skeleton> skeletonRoot;
    RootRef<AnimationClip> clipRoot;
    bool ready = false;
};

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

Transform At(double x, double y, double z) noexcept {
    return Transform(Quatd::Identity(), Vec3d(x, y, z), Vec3d::One());
}

// root → spine → head(head 회전은 길이 2 — 정규화 대상).
constexpr SkeletonImportBone BaseBones[] = {
    {"root", NoParentBone, Transform::Identity()},
    {"spine", 0, Transform(Quatd::Identity(), Vec3d(0.0, 0.0, 10.0), Vec3d::One())},
    {"head", 1, Transform(Quatd(0.0, 0.0, 0.0, 2.0), Vec3d(0.0, 0.0, 20.0), Vec3d::One())},
};

// 새 임포트로 base 스켈레톤을 만든다(소켓 하나, 커브 하나, 가상 본 하나).
bool ImportBase(Scene& scene) noexcept {
    const SkeletonImportSocket sockets[] = {{"Hat", "head", Vec3d(0.0, 0.0, 5.0)}};
    const StringView linked[] = {"head"};
    const SkeletonImportCurve curves[] = {{"Blink", false, true, Span<const StringView>(linked)}};
    const SkeletonImportVirtualBone virtualBones[] = {{"root", "head", "VB root_head"}};
    SkeletonImportSpec spec;
    spec.bones = Span<const SkeletonImportBone>(BaseBones);
    spec.sockets = Span<const SkeletonImportSocket>(sockets);
    spec.curves = Span<const SkeletonImportCurve>(curves);
    spec.virtualBones = Span<const SkeletonImportVirtualBone>(virtualBones);
    FakeContext context(*scene.registry, scene.skeleton);
    return ImportSkeleton(context, scene.skeleton, spec).IsOk();
}

} // namespace

// 새 임포트: 본·pose 정규화·리타깃 방식 기본(Animation)·소켓·커브·가상 본, 보존 필드는 빈다.
TEST(SkeletonImport_NewImportFillsFields) {
    Scene scene;
    REQUIRE(scene.ready);
    {
        // 새 출력에 남아 있던 보존 필드도 비운다.
        auto borrowed = scene.Write();
        REQUIRE(borrowed);
        AnimSlotGroup group(scene.allocator);
        group.name = scene.Intern("Upper");
        REQUIRE(Get(borrowed.Value())->slotGroups.TryPushBack(Move(group)));
        Get(borrowed.Value())->useRetargetModesFromCompatibleSkeleton = true;
    }

    REQUIRE(ImportBase(scene));
    auto borrowed = scene.Write();
    REQUIRE(borrowed);
    Skeleton* skeleton = Get(borrowed.Value());
    REQUIRE(skeleton != nullptr && skeleton->bones.bones.Size() == 3);
    CHECK(skeleton->bones.bones[1].name == scene.Intern("spine") &&
          skeleton->bones.bones[2].parentIndex == 1);
#if ROCKS_WITH_EDITOR_DATA
    CHECK(skeleton->bones.bones[2].exportName.View() == StringView("head"));
#endif
    const Quatd head = skeleton->bones.referencePose[2].rotation;
    CHECK(head.x == 0.0 && head.y == 0.0 && head.z == 0.0 && head.w == 1.0);
    REQUIRE(skeleton->retargetModes.Size() == 3);
    for (BoneTranslationRetarget mode : skeleton->retargetModes) {
        CHECK(mode == BoneTranslationRetarget::Animation);
    }

    REQUIRE(skeleton->sockets.Size() == 1 && skeleton->curves.Size() == 1 &&
            skeleton->virtualBones.Size() == 1);
    CHECK(skeleton->sockets[0].boneName == scene.Intern("head") &&
          skeleton->sockets[0].relativeLocation.z == 5.0);
    CHECK(skeleton->curves[0].isMorphTarget && skeleton->curves[0].linkedBones.Size() == 1);
    CHECK(skeleton->virtualBones[0].name == scene.Intern("VB root_head"));
    CHECK(skeleton->slotGroups.IsEmpty() && !skeleton->useRetargetModesFromCompatibleSkeleton);
}

// 실패의 코드·detail: 리타깃 방식 길이, 잘못된 이름, 계층 위반(두 번째 루트), 소켓 본 없음, 출력
// 타입.
TEST(SkeletonImport_Rejects) {
    Scene scene;
    REQUIRE(scene.ready);
    auto expect = [&](const SkeletonImportSpec& spec, AssetDiagnosticCode code, uint64 detail,
                      SkeletonIssue issue = SkeletonIssue::None,
                      ObjectHandle output = {}) noexcept {
        const ObjectHandle target = output.IsNull() ? scene.skeleton : output;
        FakeContext context(*scene.registry, target);
        SkeletonDiagnostic failure;
        const bool failed = !ImportSkeleton(context, target, spec, &failure);
        return failed && context.failCount == 1 && context.failCode == code &&
               context.failDetail == detail && failure.issue == issue;
    };

    SkeletonImportSpec modes;
    modes.bones = Span<const SkeletonImportBone>(BaseBones);
    const BoneTranslationRetarget two[] = {BoneTranslationRetarget::Animation,
                                           BoneTranslationRetarget::Skeleton};
    modes.retargetModes = Span<const BoneTranslationRetarget>(two);
    CHECK(expect(modes, AssetDiagnosticCode::InvalidSetting, 2));

    const SkeletonImportBone badName[] = {{"root", NoParentBone}, {StringView("\xFF\xFE", 2), 0}};
    SkeletonImportSpec names;
    names.bones = Span<const SkeletonImportBone>(badName);
    CHECK(expect(names, AssetDiagnosticCode::InvalidSetting, 1));

    const SkeletonImportBone twoRoots[] = {{"root", NoParentBone}, {"other", NoParentBone}};
    SkeletonImportSpec roots;
    roots.bones = Span<const SkeletonImportBone>(twoRoots);
    CHECK(expect(roots, AssetDiagnosticCode::InvalidSkeletonData, 1, SkeletonIssue::ParentOrder));

    const SkeletonImportSocket sockets[] = {{"Hat", "head"}, {"Belt", "waist"}};
    SkeletonImportSpec socket;
    socket.bones = Span<const SkeletonImportBone>(BaseBones);
    socket.sockets = Span<const SkeletonImportSocket>(sockets);
    CHECK(expect(socket, AssetDiagnosticCode::InvalidSkeletonData, 1, SkeletonIssue::SocketBone));

    SkeletonImportSpec plain;
    plain.bones = Span<const SkeletonImportBone>(BaseBones);
    CHECK(expect(plain, AssetDiagnosticCode::TypeMismatch, 0, SkeletonIssue::None, scene.clip));
}

// 재임포트 병합: 기존 본 번호·pose 유지, 대소문자 이름 바꿈, 새 본 추가(명세 리타깃 방식), 편집
// 보존(기존 소켓 유지·새 소켓 추가·같은 이름은 그대로), 보존 필드 유지.
TEST(SkeletonImport_ReimportMergesAndPreserves) {
    Scene scene;
    REQUIRE(scene.ready);
    REQUIRE(ImportBase(scene));
    {
        // 사용자 편집: spine 리타깃 방식, 소켓 위치, 슬롯 묶음.
        auto borrowed = scene.Write();
        REQUIRE(borrowed);
        Skeleton* skeleton = Get(borrowed.Value());
        skeleton->retargetModes[1] = BoneTranslationRetarget::Skeleton;
        skeleton->sockets[0].relativeLocation = Vec3d(1.0, 2.0, 3.0);
        AnimSlotGroup group(scene.allocator);
        group.name = scene.Intern("Upper");
        REQUIRE(skeleton->slotGroups.TryPushBack(Move(group)));
    }

    const SkeletonImportBone bones[] = {
        {"root", NoParentBone, At(0.0, 0.0, 99.0)},
        {"Spine", 0, At(0.0, 0.0, 77.0)},
        {"arm", 1, At(5.0, 0.0, 0.0)},
    };
    const BoneTranslationRetarget modes[] = {BoneTranslationRetarget::Animation,
                                             BoneTranslationRetarget::AnimationScaled,
                                             BoneTranslationRetarget::OrientAndScale};
    const SkeletonImportSocket sockets[] = {{"Hat", "head", Vec3d(9.0, 9.0, 9.0)}, {"Hand", "arm"}};
    SkeletonImportSpec spec;
    spec.bones = Span<const SkeletonImportBone>(bones);
    spec.retargetModes = Span<const BoneTranslationRetarget>(modes);
    spec.sockets = Span<const SkeletonImportSocket>(sockets);
    {
        FakeContext context(*scene.registry, scene.skeleton, true);
        REQUIRE(ImportSkeleton(context, scene.skeleton, spec));
    }

    {
        auto borrowed = scene.Write();
        REQUIRE(borrowed);
        Skeleton* skeleton = Get(borrowed.Value());
        REQUIRE(skeleton->bones.bones.Size() == 4 && skeleton->retargetModes.Size() == 4);
        // 기존 본은 번호·pose를 지키고(head도 남는다), 새 arm은 뒤에 명세 pose로 붙는다.
        CHECK(skeleton->bones.bones[3].name == scene.Intern("arm") &&
              skeleton->bones.bones[3].parentIndex == 1);
        CHECK(skeleton->bones.referencePose[1].translation.z == 10.0 &&
              skeleton->bones.referencePose[3].translation.x == 5.0);
#if ROCKS_WITH_EDITOR_DATA
        CHECK(skeleton->bones.bones[1].exportName.View() == StringView("Spine"));
#endif
        CHECK(skeleton->retargetModes[1] == BoneTranslationRetarget::Skeleton &&
              skeleton->retargetModes[3] == BoneTranslationRetarget::OrientAndScale);
        REQUIRE(skeleton->sockets.Size() == 2);
        CHECK(skeleton->sockets[0].relativeLocation.x == 1.0 &&
              skeleton->sockets[1].name == scene.Intern("Hand"));
        CHECK(skeleton->curves.Size() == 1 && skeleton->virtualBones.Size() == 1 &&
              skeleton->slotGroups.Size() == 1);
    }

    // replaceOnReimport: 소켓·커브·가상 본은 명세 값, 명세 본의 리타깃 방식도 명세 값, 본은 남는다.
    spec.replaceOnReimport = true;
    {
        FakeContext context(*scene.registry, scene.skeleton, true);
        REQUIRE(ImportSkeleton(context, scene.skeleton, spec));
    }

    auto borrowed = scene.Write();
    REQUIRE(borrowed);
    Skeleton* skeleton = Get(borrowed.Value());
    CHECK(skeleton->bones.bones.Size() == 4 &&
          skeleton->retargetModes[1] == BoneTranslationRetarget::AnimationScaled);
    REQUIRE(skeleton->sockets.Size() == 2);
    CHECK(skeleton->sockets[0].relativeLocation.x == 9.0);
    CHECK(skeleton->curves.IsEmpty() && skeleton->virtualBones.IsEmpty() &&
          skeleton->slotGroups.Size() == 1);
}

// 재임포트 사슬 불일치: head를 root 아래로 옮긴 명세는 SkeletonMismatch(ParentChainMismatch)이고
// MergeBonesIntoSkeleton은 실패하면 스켈레톤을 바꾸지 않는다.
TEST(SkeletonImport_ReimportChainMismatchAndMergeFailures) {
    Scene scene;
    REQUIRE(scene.ready);
    REQUIRE(ImportBase(scene));
    const SkeletonImportBone moved[] = {{"root", NoParentBone}, {"head", 0}};
    SkeletonImportSpec spec;
    spec.bones = Span<const SkeletonImportBone>(moved);
    {
        FakeContext context(*scene.registry, scene.skeleton, true);
        SkeletonDiagnostic failure;
        CHECK(!ImportSkeleton(context, scene.skeleton, spec, &failure));
        CHECK(context.failCode == AssetDiagnosticCode::SkeletonMismatch &&
              failure.issue == SkeletonIssue::ParentChainMismatch && context.failDetail == 1);
    }

    auto borrowed = scene.Write();
    REQUIRE(borrowed);
    Skeleton* skeleton = Get(borrowed.Value());
    CHECK(skeleton->bones.bones.Size() == 3 && skeleton->retargetModes.Size() == 3);

    // 메시 계층(본 하나 — 이미 있는 root)과 길이가 다른 newModes는 InvalidArgument, 그대로.
    BoneHierarchy mesh(scene.allocator);
    BoneInfo bone(scene.allocator);
    bone.name = scene.Intern("root");
    REQUIRE(mesh.bones.TryPushBack(Move(bone)));
    REQUIRE(mesh.referencePose.TryPushBack(Transform::Identity()));
    BoneMergeResult merged(scene.allocator);
    const BoneTranslationRetarget wrong[] = {BoneTranslationRetarget::Animation,
                                             BoneTranslationRetarget::Animation};
    CHECK(!MergeBonesIntoSkeleton(*skeleton, mesh, Span<const BoneTranslationRetarget>(wrong),
                                  merged));
    // 리타깃 방식 길이가 본 수와 다른 스켈레톤은 RetargetModeCount 진단.
    REQUIRE(skeleton->retargetModes.TryPopBack());
    SkeletonDiagnostic failure;
    CHECK(!MergeBonesIntoSkeleton(*skeleton, mesh, {}, merged, &failure));
    CHECK(failure.code == AssetDiagnosticCode::InvalidSkeletonData &&
          failure.issue == SkeletonIssue::RetargetModeCount);
    CHECK(skeleton->bones.bones.Size() == 3 && merged.meshToMerged.IsEmpty());
}

// 새 임포트 값 복사(명세 리타깃 방식·소켓 회전·크기·forceAlwaysAnimated·커브 표시·maxLod)와 보존
// 필드 셋 비움, 본 0개 거부, 소켓·가상 본·커브 연결 본 이름 실패의 detail.
TEST(SkeletonImport_NewImportCopiesValuesAndRejectsEmpty) {
    Scene scene;
    REQUIRE(scene.ready);
    {
        auto borrowed = scene.Write();
        REQUIRE(borrowed);
        Skeleton* skeleton = Get(borrowed.Value());
        NamedReferencePose pose(scene.allocator);
        pose.name = scene.Intern("Old");
        REQUIRE(skeleton->retargetSources.TryPushBack(Move(pose)));
        REQUIRE(
            skeleton->compatibleSkeletons.TryPushBack(SoftObjectRef<Skeleton>(scene.allocator)));
    }

    const BoneTranslationRetarget modes[] = {BoneTranslationRetarget::Skeleton,
                                             BoneTranslationRetarget::AnimationRelative,
                                             BoneTranslationRetarget::OrientAndScale};
    const SkeletonImportSocket sockets[] = {
        {"Hat", "head", Vec3d(), Rotator<double>(10.0, 20.0, 30.0), Vec3d(2.0, 2.0, 2.0), false}};
    const SkeletonImportCurve curves[] = {{"Glow", true, false, {}, 2}};
    SkeletonImportSpec spec;
    spec.bones = Span<const SkeletonImportBone>(BaseBones);
    spec.retargetModes = Span<const BoneTranslationRetarget>(modes);
    spec.sockets = Span<const SkeletonImportSocket>(sockets);
    spec.curves = Span<const SkeletonImportCurve>(curves);
    {
        FakeContext context(*scene.registry, scene.skeleton);
        REQUIRE(ImportSkeleton(context, scene.skeleton, spec));
    }

    {
        auto borrowed = scene.Write();
        REQUIRE(borrowed);
        Skeleton* skeleton = Get(borrowed.Value());
        CHECK(skeleton->retargetModes[0] == BoneTranslationRetarget::Skeleton &&
              skeleton->retargetModes[2] == BoneTranslationRetarget::OrientAndScale);
        const BoneSocket& hat = skeleton->sockets[0];
        CHECK(hat.relativeRotation.pitch == 10.0 && hat.relativeRotation.roll == 30.0 &&
              hat.relativeScale.y == 2.0 && !hat.forceAlwaysAnimated);
        CHECK(skeleton->curves[0].isMaterial && !skeleton->curves[0].isMorphTarget &&
              skeleton->curves[0].maxLod == 2);
        CHECK(skeleton->retargetSources.IsEmpty() && skeleton->compatibleSkeletons.IsEmpty());
    }

    auto expect = [&](const SkeletonImportSpec& value, AssetDiagnosticCode code, uint64 detail,
                      SkeletonIssue issue = SkeletonIssue::None) noexcept {
        FakeContext context(*scene.registry, scene.skeleton);
        SkeletonDiagnostic failure;
        const bool failed = !ImportSkeleton(context, scene.skeleton, value, &failure);
        return failed && context.failCount == 1 && context.failCode == code &&
               context.failDetail == detail && failure.issue == issue;
    };

    SkeletonImportSpec empty;
    CHECK(
        expect(empty, AssetDiagnosticCode::SkeletonMismatch, 0, SkeletonIssue::EmptyMeshHierarchy));
    const StringView bad = StringView("\xFF\xFE", 2);
    const SkeletonImportSocket badSockets[] = {{"Hat", "head"}, {"Belt", bad}};
    SkeletonImportSpec socketNames;
    socketNames.bones = Span<const SkeletonImportBone>(BaseBones);
    socketNames.sockets = Span<const SkeletonImportSocket>(badSockets);
    CHECK(expect(socketNames, AssetDiagnosticCode::InvalidSetting, 1));
    const SkeletonImportVirtualBone badVirtual[] = {{"root", bad, "VB x"}};
    SkeletonImportSpec virtualNames;
    virtualNames.bones = Span<const SkeletonImportBone>(BaseBones);
    virtualNames.virtualBones = Span<const SkeletonImportVirtualBone>(badVirtual);
    CHECK(expect(virtualNames, AssetDiagnosticCode::InvalidSetting, 0));
    const StringView linked[] = {"head", bad};
    const SkeletonImportCurve badCurves[] = {
        {"Blink"}, {"Smile", false, false, Span<const StringView>(linked)}};
    SkeletonImportSpec curveNames;
    curveNames.bones = Span<const SkeletonImportBone>(BaseBones);
    curveNames.curves = Span<const SkeletonImportCurve>(badCurves);
    CHECK(expect(curveNames, AssetDiagnosticCode::InvalidSetting, 1));
}

// 재임포트 편집 보존 세부: 이름을 바꾼 가상 본(같은 쌍)은 다시 더하지 않고, 같은 이름 커브는 표시만
// OR로 합치며(연결 본·maxLod 편집 유지), 보존 필드 셋을 유지한다. replaceOnReimport이고 명세
// retargetModes가 비면 명세 본은 Animation, 명세에 없는 본(head)은 그대로다.
TEST(SkeletonImport_ReimportVirtualBonesCurvesAndReplaceModes) {
    Scene scene;
    REQUIRE(scene.ready);
    REQUIRE(ImportBase(scene));
    {
        auto borrowed = scene.Write();
        REQUIRE(borrowed);
        Skeleton* skeleton = Get(borrowed.Value());
        skeleton->virtualBones[0].name = scene.Intern("VB headAim");
        skeleton->curves[0].maxLod = 3;
        skeleton->curves[0].isMorphTarget = false;
        skeleton->retargetModes[1] = BoneTranslationRetarget::Skeleton;
        skeleton->retargetModes[2] = BoneTranslationRetarget::AnimationScaled;
        NamedReferencePose pose(scene.allocator);
        pose.name = scene.Intern("Old");
        REQUIRE(skeleton->retargetSources.TryPushBack(Move(pose)));
        REQUIRE(
            skeleton->compatibleSkeletons.TryPushBack(SoftObjectRef<Skeleton>(scene.allocator)));
    }

    const StringView linked[] = {"spine"};
    const SkeletonImportCurve curves[] = {{"Blink", true, true, Span<const StringView>(linked), 7}};
    const SkeletonImportVirtualBone virtualBones[] = {{"root", "head", "VB root_head"}};
    const SkeletonImportBone bones[] = {{"root", NoParentBone}, {"spine", 0}};
    SkeletonImportSpec spec;
    spec.bones = Span<const SkeletonImportBone>(bones);
    spec.curves = Span<const SkeletonImportCurve>(curves);
    spec.virtualBones = Span<const SkeletonImportVirtualBone>(virtualBones);
    {
        FakeContext context(*scene.registry, scene.skeleton, true);
        REQUIRE(ImportSkeleton(context, scene.skeleton, spec));
    }

    {
        auto borrowed = scene.Write();
        REQUIRE(borrowed);
        Skeleton* skeleton = Get(borrowed.Value());
        REQUIRE(skeleton->virtualBones.Size() == 1 && skeleton->curves.Size() == 1);
        CHECK(skeleton->virtualBones[0].name == scene.Intern("VB headAim"));
        const SkeletonCurveInfo& blink = skeleton->curves[0];
        CHECK(blink.isMaterial && blink.isMorphTarget && blink.maxLod == 3 &&
              blink.linkedBones.Size() == 1 && blink.linkedBones[0] == scene.Intern("head"));
        CHECK(skeleton->retargetSources.Size() == 1 && skeleton->compatibleSkeletons.Size() == 1);
        CHECK(skeleton->retargetModes[1] == BoneTranslationRetarget::Skeleton);
    }

    spec.replaceOnReimport = true;
    {
        FakeContext context(*scene.registry, scene.skeleton, true);
        REQUIRE(ImportSkeleton(context, scene.skeleton, spec));
    }

    auto borrowed = scene.Write();
    REQUIRE(borrowed);
    Skeleton* skeleton = Get(borrowed.Value());
    CHECK(skeleton->retargetModes[0] == BoneTranslationRetarget::Animation &&
          skeleton->retargetModes[1] == BoneTranslationRetarget::Animation &&
          skeleton->retargetModes[2] == BoneTranslationRetarget::AnimationScaled);
    CHECK(skeleton->virtualBones[0].name == scene.Intern("VB root_head") &&
          skeleton->curves[0].maxLod == 7 && skeleton->retargetSources.Size() == 1);
}

// 재임포트 실패: 다른 루트(MissingAncestor), 명세 계층 위반(role Mesh, detail = 본), 병합 뒤 검증
// 실패(새 소켓의 본 없음 — detail = 소켓). 병합 실패는 스켈레톤 값(본 이름·pose·리타깃 방식)을
// 바꾸지 않는다.
TEST(SkeletonImport_ReimportFailures) {
    Scene scene;
    REQUIRE(scene.ready);
    REQUIRE(ImportBase(scene));
    auto expect = [&](const SkeletonImportSpec& spec, AssetDiagnosticCode code, uint64 detail,
                      SkeletonIssue issue, BoneHierarchyRole role) noexcept {
        FakeContext context(*scene.registry, scene.skeleton, true);
        SkeletonDiagnostic failure;
        const bool failed = !ImportSkeleton(context, scene.skeleton, spec, &failure);
        return failed && context.failCount == 1 && context.failCode == code &&
               context.failDetail == detail && failure.issue == issue && failure.role == role;
    };

    const SkeletonImportBone otherRoot[] = {{"pelvis", NoParentBone}, {"leg", 0}};
    SkeletonImportSpec ancestor;
    ancestor.bones = Span<const SkeletonImportBone>(otherRoot);
    CHECK(expect(ancestor, AssetDiagnosticCode::SkeletonMismatch, 0, SkeletonIssue::MissingAncestor,
                 BoneHierarchyRole::Mesh));
    const SkeletonImportBone twoRoots[] = {{"root", NoParentBone}, {"spare", NoParentBone}};
    SkeletonImportSpec broken;
    broken.bones = Span<const SkeletonImportBone>(twoRoots);
    CHECK(expect(broken, AssetDiagnosticCode::InvalidSkeletonData, 1, SkeletonIssue::ParentOrder,
                 BoneHierarchyRole::Mesh));
    const SkeletonImportSocket sockets[] = {{"Hat2", "head"}, {"Belt", "waist"}};
    SkeletonImportSpec socket;
    socket.bones = Span<const SkeletonImportBone>(BaseBones);
    socket.sockets = Span<const SkeletonImportSocket>(sockets);
    CHECK(expect(socket, AssetDiagnosticCode::InvalidSkeletonData, 2, SkeletonIssue::SocketBone,
                 BoneHierarchyRole::Skeleton));

    // 사슬 불일치 뒤 값이 그대로다(head 부모를 root로 옮긴 명세).
    {
        auto borrowed = scene.Write();
        REQUIRE(borrowed);
        Get(borrowed.Value())->retargetModes[2] = BoneTranslationRetarget::OrientAndScale;
    }

    const SkeletonImportBone moved[] = {{"root", NoParentBone}, {"head", 0}};
    SkeletonImportSpec chain;
    chain.bones = Span<const SkeletonImportBone>(moved);
    {
        FakeContext context(*scene.registry, scene.skeleton, true);
        CHECK(!ImportSkeleton(context, scene.skeleton, chain));
    }

    auto borrowed = scene.Write();
    REQUIRE(borrowed);
    Skeleton* skeleton = Get(borrowed.Value());
    CHECK(skeleton->bones.bones[2].name == scene.Intern("head") &&
          skeleton->bones.bones[2].parentIndex == 1 &&
          skeleton->bones.referencePose[2].translation.z == 20.0 &&
          skeleton->retargetModes[2] == BoneTranslationRetarget::OrientAndScale);
}

} // namespace skeleton_import_tests_file
