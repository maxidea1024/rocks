#include "SkinnedMeshTestSupport.h"

#include <Animation/Skeleton.generated.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Material/Material.generated.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.generated.h>
#include <MeshTools/SkeletalMeshImport.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/ObjectRegistryOptions.h>
#include <TestSupport/Test.h>

// 스킨 메시 임포트 서비스 시험이다(Docs/Design/MeshAnimationAssets 6.7.3절): 필드 채우기(원본
// bulk가 출력 bulk로, 메시 본·슬롯·소켓·가중치 프로필·LOD 설정·플랫폼 값, renderData 비움),
// 스켈레톤 세 경우(보조 출력 — 메시 본으로 만듦, 기존 스켈레톤 — 본이 다 있으면 참조만·모자라면
// EditAsset으로 더함, 묶음 구성원 — 모자라면 MissingSkeletonBones), 병합 충돌, 검증 실패의
// 코드·detail(저장 전 — bulk를 넘기지 않음), 재임포트 보존과 replaceSettingsOnReimport.
using namespace rocks;
namespace skeletal_mesh_import_tests_file {

using namespace rocks::skinned_mesh_test;

namespace {

constexpr AssetId MaterialAsset{Guid{0x4d41540000000000ull, 11}};
constexpr AssetId SkeletonAsset{Guid{0x534b454c00000000ull, 12}};
// 표에 있는 머티리얼 id를 스켈레톤으로 잘못 준다(TypeMismatch).
constexpr AssetId WrongSkeleton = MaterialAsset;
// 시험 메시 본 수다(사슬 ba → bb, 정점은 모두 bb).
constexpr uint32 TestBones = 2;

// 가짜 임포트 문맥이다: 주 출력은 미리 만든 메시, 보조 출력 "Skeleton"은 미리 만든 스켈레톤, bulk는
// 메모리, OpenAsset·EditAsset은 표(EditAsset은 부른 id를 기록한다), 묶음 구성원 표.
class FakeContext final : public AssetImportContext {
public:
    FakeContext(ObjectRegistry& registry, ObjectHandle output, ObjectHandle skeleton,
                Allocator& allocator, bool reimport = false) noexcept
        : bulk(allocator), registry_(registry), access_(registry.OwnerAccess()), output_(output),
          skeleton_(skeleton), reimport_(reimport) {
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

    Result<ObjectHandle> Output(StringView key, TypeId type) noexcept override {
        if (key.IsEmpty() && type == TypeIdOf<SkeletalMesh>()) {
            return Result<ObjectHandle>::Success(output_);
        }

        if (key == StringView("Skeleton") && type == TypeIdOf<Skeleton>()) {
            ++skeletonOutputs;
            return Result<ObjectHandle>::Success(skeleton_);
        }

        return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    }

    Result<BulkDataRef> AddBulk(ObjectHandle target, Array<uint8>&& raw) noexcept override {
        if (target != output_) {
            return Result<BulkDataRef>::Failure(ErrorCode::InvalidArgument);
        }

        ++addedBulks;
        return bulk.Add(Move(raw));
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

    Result<ObjectHandle> OpenAsset(AssetId asset) noexcept override {
        for (usize index = 0; index < count; ++index) {
            if (ids[index] == asset) {
                return Result<ObjectHandle>::Success(handles[index]);
            }
        }

        (void)Fail(AssetDiagnosticCode::MissingDependency, ErrorCode::NotFound, 0);
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    Result<ObjectHandle> EditAsset(AssetId asset) noexcept override {
        ++edits;
        lastEdited = asset;
        return OpenAsset(asset);
    }

    usize GroupSize() const noexcept override {
        return members;
    }

    usize GroupIndex() const noexcept override {
        return 0;
    }

    AssetId MemberAsset(usize index) const noexcept override {
        return index == 1 && members > 1 ? memberSkeleton : AssetId{};
    }

    void Add(AssetId id, ObjectHandle handle) noexcept {
        ids[count] = id;
        handles[count] = handle;
        ++count;
    }

    MemoryBulkSource bulk;
    uint32 addedBulks = 0;
    uint32 failCount = 0;
    uint32 edits = 0;
    uint32 skeletonOutputs = 0;
    AssetId lastEdited{};
    AssetDiagnosticCode failCode = AssetDiagnosticCode::None;
    uint64 failDetail = 0;
    AssetId ids[4];
    ObjectHandle handles[4];
    usize count = 0;
    // 묶음 크기와 구성원 1의 id다(구성원 스켈레톤 시험).
    usize members = 1;
    AssetId memberSkeleton{};

private:
    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    ObjectHandle output_;
    ObjectHandle skeleton_;
    bool reimport_;
};

struct ImportScene {
    ImportScene() noexcept : types(allocator), registration(types, allocator), names(allocator) {
        ready = RegisterMeshTypes(registration).IsOk() && names.Initialize().IsOk();
        RegistryOptions options;
        options.names = &names;
        auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{0x534b4d49ull, 1}, options);
        if (made) {
            registry = Move(made.Value());
        }

        ready = ready && registry.Get() != nullptr && Make<SkeletalMesh>(mesh, meshRoot) &&
                Make<Skeleton>(auxiliary, auxiliaryRoot) &&
                Make<Skeleton>(existing, existingRoot) && Make<Material>(material, materialRoot);
        ready = ready && mesh_source_test::BuildQuad(source) &&
                RigidWeights(source, 1) != MeshNoAttribute;
    }

    ~ImportScene() noexcept {
        meshRoot.Reset();
        auxiliaryRoot.Reset();
        existingRoot.Reset();
        materialRoot.Reset();
        registry.Reset();
        CHECK(registration.Unregister());
    }

    template <class T, class R>
    bool Make(ObjectHandle& handle, R& root) noexcept {
        auto made = registry->NewObject<T>(registry->OwnerAccess(), TypeIdOf<T>());
        if (!made) {
            return false;
        }

        handle = made.Value().Handle();
        root = Move(made.Value());
        return true;
    }

    template <class T>
    T* Get(ObjectHandle handle) noexcept {
        auto borrowed = registry->ResolveForWrite<T>(registry->OwnerAccess(), handle);
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    SkeletalMesh* Mesh() noexcept {
        return Get<SkeletalMesh>(mesh);
    }

    // 기존 스켈레톤에 본 count개 사슬(ba, bb, …)을 둔다.
    bool SetExisting(uint32 count) noexcept {
        Skeleton* skeleton = Get<Skeleton>(existing);
        if (skeleton == nullptr) {
            return false;
        }

        skeleton->bones.bones.Clear();
        skeleton->bones.referencePose.Clear();
        skeleton->retargetModes.Clear();
        if (!ChainBones(names, count, skeleton->bones)) {
            return false;
        }

        for (uint32 bone = 0; bone < count; ++bone) {
            if (!skeleton->retargetModes.TryPushBack(BoneTranslationRetarget::Animation)) {
                return false;
            }
        }

        return true;
    }

    usize ExistingBones() noexcept {
        Skeleton* skeleton = Get<Skeleton>(existing);
        return skeleton != nullptr ? skeleton->bones.bones.Size() : 0;
    }

    void AddReferences(FakeContext& context) noexcept {
        context.Add(MaterialAsset, material);
        context.Add(SkeletonAsset, existing);
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
    MeshSource source{allocator};
    ObjectHandle mesh{};
    ObjectHandle auxiliary{};
    ObjectHandle existing{};
    ObjectHandle material{};
    RootRef<SkeletalMesh> meshRoot;
    RootRef<Skeleton> auxiliaryRoot;
    RootRef<Skeleton> existingRoot;
    RootRef<Material> materialRoot;
    bool ready = false;
};

// 시험 메시 본(사슬 ba → bb — ChainBones·LetterName과 같은 이름·pose)이다.
struct TestBoneList {
    TestBoneList() noexcept {
        for (uint32 bone = 0; bone < TestBones; ++bone) {
            bones[bone].name = bone == 0 ? StringView("ba") : StringView("bb");
            bones[bone].parentIndex = bone == 0 ? NoParentBone : static_cast<int32>(bone - 1);
            bones[bone].referencePose = animation_test::PoseFor(bone);
        }
    }

    SkeletonImportBone bones[TestBones];
};

SkeletalMeshImportSpec Basic(const SkeletalMeshImportLod& lod, const TestBoneList& list) noexcept {
    SkeletalMeshImportSpec spec;
    spec.lods = Span<const SkeletalMeshImportLod>(&lod, 1);
    spec.bones = Span<const SkeletonImportBone>(list.bones);
    return spec;
}

} // namespace

// 필드 채우기(스켈레톤 null — 보조 출력을 메시 본으로 만든다): 원본 bulk가 출력 bulk로
// 옮겨지고(출력 bulk에서 Load가 같은 메시), 메시 본·스켈레톤 참조·슬롯·소켓·가중치 프로필·LOD
// 설정·플랫폼 값이 채워지고 renderData는 빈다.
TEST(SkeletalMeshImport_FillsFields) {
    ImportScene scene;
    REQUIRE(scene.ready);
    FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
    scene.AddReferences(context);
    TestBoneList list;
    const int32 map[] = {SkeletalMeshNoMaterialRemap, 0};
    const StringView removes[] = {"bb"};
    const StaticMeshImportFloat screens[] = {{"Android", 0.5f}};
    SkeletalMeshImportLod lod;
    lod.source = &scene.source;
    lod.lodHysteresis = 0.2f;
    lod.lodMaterialMap = map;
    lod.bonesToRemove = removes;
    lod.screenSizePerPlatform = screens;
    lod.build.useHighPrecisionSkinWeights = true;
    const StaticMeshImportSlot slots[] = {{"Body", "BodyImported", MaterialAsset, {}, {}}};
    SkeletonImportSocket socket;
    socket.name = "Hand";
    socket.boneName = "bb";
    socket.relativeLocation = Vec3d(1.0, 2.0, 3.0);
    const SkeletonImportSocket sockets[] = {socket};
    const SkeletalMeshImportBool defaults[] = {{"Android", true}};
    SkeletalMeshImportProfile profile;
    profile.name = "Cloth";
    profile.defaultProfilePerPlatform = defaults;
    profile.defaultProfileFromLod = 1;
    const SkeletalMeshImportProfile profiles[] = {profile};
    const StaticMeshImportInt minLods[] = {{"Android", 1}};
    SkeletalMeshImportSpec spec = Basic(lod, list);
    spec.slots = slots;
    spec.sockets = sockets;
    spec.skinWeightProfiles = profiles;
    spec.minLod = 0;
    spec.minLodPerPlatform = minLods;
    spec.positiveBoundsExtension = Vec3d(0.0, 0.0, 5.0);
    REQUIRE(ImportSkeletalMesh(context, scene.mesh, spec));
    CHECK(context.failCount == 0 && context.skeletonOutputs == 1 && context.edits == 0 &&
          context.addedBulks > 0);
    const SkeletalMesh* mesh = scene.Mesh();
    REQUIRE(mesh != nullptr);
    CHECK(mesh->skeleton.Handle() == scene.auxiliary && mesh->bones.bones.Size() == TestBones &&
          mesh->bones.bones[1].parentIndex == 0);
    const Skeleton* skeleton = scene.Get<Skeleton>(scene.auxiliary);
    REQUIRE(skeleton != nullptr);
    CHECK(skeleton->bones.bones.Size() == TestBones &&
          skeleton->bones.bones[1].name == mesh->bones.bones[1].name);
    REQUIRE(mesh->sourceLods.Size() == 1);
    const SkeletalMeshSourceLod& source = mesh->sourceLods[0];
    CHECK(source.hasSource && !source.hasBakedSource && source.lodHysteresis == 0.2f &&
          source.lodMaterialMap.Size() == 2 && source.lodMaterialMap[1] == 0 &&
          source.bonesToRemove.Size() == 1 && source.bonesToRemove[0] == scene.Intern("bb") &&
          source.build.useHighPrecisionSkinWeights &&
          source.screenSize.defaultValue == SkeletalMeshSourceLod::DefaultScreenSize &&
          source.screenSize.perPlatform.Count() == 1);
    MeshSource loaded(scene.allocator);
    CHECK(loaded.Load(source.source, context.bulk, scene.names));
    CHECK(loaded.Count(MeshElement::Vertex) == 4);
    REQUIRE(mesh->materialSlots.Size() == 1);
    CHECK(mesh->materialSlots[0].slotName == scene.Intern("Body") &&
          mesh->materialSlots[0].material.Handle() == scene.material);
    REQUIRE(mesh->sockets.Size() == 1);
    CHECK(mesh->sockets[0].boneName == scene.Intern("bb") &&
          mesh->sockets[0].relativeLocation.z == 3.0);
    REQUIRE(mesh->skinWeightProfiles.Size() == 1);
    CHECK(mesh->skinWeightProfiles[0].name == scene.Intern("Cloth") &&
          !mesh->skinWeightProfiles[0].defaultProfile.defaultValue &&
          mesh->skinWeightProfiles[0].defaultProfile.perPlatform.Count() == 1 &&
          mesh->skinWeightProfiles[0].defaultProfileFromLod.defaultValue == 1);
    CHECK(mesh->minLod.perPlatform.Count() == 1 && mesh->positiveBoundsExtension.z == 5.0 &&
          mesh->renderData.lods.IsEmpty());
}

// 기존 스켈레톤: 메시 본이 모두 있으면 참조만(EditAsset 없음), 모자라면 EditAsset으로 고쳐 본을
// 더한다(기존 본 번호 유지). 보조 출력은 만들지 않는다.
TEST(SkeletalMeshImport_ExistingSkeleton) {
    ImportScene scene;
    REQUIRE(scene.ready);
    TestBoneList list;
    SkeletalMeshImportLod lod;
    lod.source = &scene.source;
    SkeletalMeshImportSpec spec = Basic(lod, list);
    spec.skeleton = SkeletonAsset;
    {
        REQUIRE(scene.SetExisting(TestBones));
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, spec));
        CHECK(context.edits == 0 && context.skeletonOutputs == 0 &&
              scene.Mesh()->skeleton.Handle() == scene.existing &&
              scene.ExistingBones() == TestBones);
    }

    {
        REQUIRE(scene.SetExisting(1));
        Skeleton* before = scene.Get<Skeleton>(scene.existing);
        REQUIRE(before != nullptr);
        before->sockets.Clear();
        BoneSocket kept;
        kept.name = scene.Intern("Kept");
        kept.boneName = scene.Intern("ba");
        REQUIRE(before->sockets.TryPushBack(kept));
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, spec));
        CHECK(context.edits == 1 && context.lastEdited == SkeletonAsset &&
              scene.ExistingBones() == TestBones &&
              scene.Mesh()->skeleton.Handle() == scene.existing);
        const Skeleton* after = scene.Get<Skeleton>(scene.existing);
        REQUIRE(after != nullptr);
        CHECK(after->sockets.Size() == 1 && after->sockets[0].name == scene.Intern("Kept") &&
              after->retargetModes.Size() == TestBones &&
              after->retargetModes[1] == BoneTranslationRetarget::Animation);
    }

    // 없는 스켈레톤 id는 문맥의 MissingDependency다.
    {
        SkeletalMeshImportSpec missing = spec;
        missing.skeleton = AssetId{Guid{0x534b454c00000000ull, 99}};
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
        scene.AddReferences(context);
        CHECK(ImportSkeletalMesh(context, scene.mesh, missing).Error() == ErrorCode::NotFound);
        CHECK(context.failCode == AssetDiagnosticCode::MissingDependency &&
              context.addedBulks == 0);
    }
}

// 병합 뒤 스켈레톤 검증: 기존 스켈레톤의 가상 본 이름과 같은 메시 본을 더하면 본 계층 병합은
// 통과하지만 ValidateSkeleton이 InvalidSkeletonData(DuplicateVirtualBone)로 거부한다.
TEST(SkeletalMeshImport_MergeValidatesSkeleton) {
    ImportScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.SetExisting(1));
    {
        Skeleton* skeleton = scene.Get<Skeleton>(scene.existing);
        REQUIRE(skeleton != nullptr);
        VirtualBone virtualBone;
        virtualBone.sourceBone = scene.Intern("ba");
        virtualBone.targetBone = scene.Intern("ba");
        virtualBone.name = scene.Intern("VB bb");
        REQUIRE(skeleton->virtualBones.TryPushBack(virtualBone));
    }

    TestBoneList list;
    list.bones[1].name = "VB bb";
    SkeletalMeshImportLod lod;
    lod.source = &scene.source;
    SkeletalMeshImportSpec spec = Basic(lod, list);
    spec.skeleton = SkeletonAsset;
    FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
    scene.AddReferences(context);
    SkeletonDiagnostic failure;
    CHECK(!ImportSkeletalMesh(context, scene.mesh, spec, nullptr, &failure));
    CHECK(context.failCode == AssetDiagnosticCode::InvalidSkeletonData &&
          failure.issue == SkeletonIssue::DuplicateVirtualBone && context.edits == 1);
}

// 새 임포트는 옛 값을 남기지 않는다(채워진 메시에 새 임포트 — 슬롯·소켓·프로필·섹션 정보 없음).
// 섹션 정보: 채우기, 재임포트에서 명세가 비면 옛 값 유지.
TEST(SkeletalMeshImport_NewImportReplacesAndSections) {
    ImportScene scene;
    REQUIRE(scene.ready);
    TestBoneList list;
    SkeletalMeshImportLod lod;
    lod.source = &scene.source;
    const StaticMeshImportSlot slots[] = {{"Body", "Body", MaterialAsset, {}, {}}};
    SkeletonImportSocket hand;
    hand.name = "Hand";
    hand.boneName = "bb";
    const SkeletonImportSocket sockets[] = {hand};
    SkeletalMeshImportProfile cloth;
    cloth.name = "Cloth";
    const SkeletalMeshImportProfile profiles[] = {cloth};
    MeshSectionInfoEntry entry;
    entry.info.castShadow = false;
    const MeshSectionInfoEntry sections[] = {entry};
    SkeletalMeshImportSpec full = Basic(lod, list);
    full.slots = slots;
    full.sockets = sockets;
    full.skinWeightProfiles = profiles;
    full.sectionInfo = sections;
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, full));
    }

    CHECK(scene.Mesh()->sectionInfo.Size() == 1 && !scene.Mesh()->sectionInfo[0].info.castShadow);
    SkeletalMeshImportSpec bare = Basic(lod, list);
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator, true);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, bare));
    }

    CHECK(scene.Mesh()->sectionInfo.Size() == 1);
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, bare));
    }

    const SkeletalMesh* mesh = scene.Mesh();
    REQUIRE(mesh != nullptr);
    CHECK(mesh->materialSlots.Size() == 0 && mesh->sockets.IsEmpty() &&
          mesh->skinWeightProfiles.IsEmpty() && mesh->sectionInfo.IsEmpty());
}

// 재임포트의 본 변화: 보조 스켈레톤은 새 본을 병합해 더하고, 본이 빠진 메시는 그 본의 옛 소켓을
// 버린다(UE 소켓 복원과 같다 — 남은 본의 소켓은 둔다).
TEST(SkeletalMeshImport_ReimportBoneChanges) {
    ImportScene scene;
    REQUIRE(scene.ready);
    TestBoneList list;
    SkeletalMeshImportLod lod;
    lod.source = &scene.source;
    SkeletonImportSocket hand;
    hand.name = "Hand";
    hand.boneName = "bb";
    SkeletonImportSocket root;
    root.name = "Root";
    root.boneName = "ba";
    const SkeletonImportSocket sockets[] = {hand, root};
    SkeletalMeshImportSpec spec = Basic(lod, list);
    spec.sockets = sockets;
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, spec));
    }

    // 본 셋(ba → bb → bc): 보조 스켈레톤이 bc를 더한다.
    SkeletonImportBone three[3] = {list.bones[0], list.bones[1], list.bones[1]};
    three[2].name = "bc";
    three[2].parentIndex = 1;
    SkeletalMeshImportSpec more = Basic(lod, list);
    more.bones = three;
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator, true);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, more));
    }

    const Skeleton* skeleton = scene.Get<Skeleton>(scene.auxiliary);
    REQUIRE(skeleton != nullptr);
    CHECK(skeleton->bones.bones.Size() == 3 && scene.Mesh()->bones.bones.Size() == 3 &&
          scene.Mesh()->sockets.Size() == 2);

    // 본 하나(ba)뿐인 메시(정점은 본 0): Hand(bb) 소켓은 버리고 Root는 둔다.
    MeshSource rigid(scene.allocator);
    REQUIRE(mesh_source_test::BuildQuad(rigid) && RigidWeights(rigid, 0) != MeshNoAttribute);
    SkeletalMeshImportLod single;
    single.source = &rigid;
    SkeletalMeshImportSpec fewer = Basic(single, list);
    fewer.bones = Span<const SkeletonImportBone>(list.bones, 1);
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator, true);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, fewer));
    }

    const SkeletalMesh* mesh = scene.Mesh();
    REQUIRE(mesh != nullptr && mesh->sockets.Size() == 1);
    CHECK(mesh->sockets[0].name == scene.Intern("Root") && mesh->bones.bones.Size() == 1);
}

// 묶음 구성원 스켈레톤은 고치지 않는다: 본이 모자라면 SkeletonMismatch(MissingSkeletonBones,
// detail = 첫 모자란 메시 본). 본이 다 있으면 참조한다.
TEST(SkeletalMeshImport_MemberSkeleton) {
    ImportScene scene;
    REQUIRE(scene.ready);
    TestBoneList list;
    SkeletalMeshImportLod lod;
    lod.source = &scene.source;
    SkeletalMeshImportSpec spec = Basic(lod, list);
    spec.skeleton = SkeletonAsset;
    REQUIRE(scene.SetExisting(1));
    FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
    scene.AddReferences(context);
    context.members = 2;
    context.memberSkeleton = SkeletonAsset;
    SkeletonDiagnostic failure;
    CHECK(ImportSkeletalMesh(context, scene.mesh, spec, nullptr, &failure).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(context.failCode == AssetDiagnosticCode::SkeletonMismatch && context.failDetail == 1 &&
          failure.issue == SkeletonIssue::MissingSkeletonBones && context.edits == 0 &&
          scene.ExistingBones() == 1 && context.addedBulks == 0);
    REQUIRE(scene.SetExisting(TestBones));
    FakeContext complete(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
    scene.AddReferences(complete);
    complete.members = 2;
    complete.memberSkeleton = SkeletonAsset;
    REQUIRE(ImportSkeletalMesh(complete, scene.mesh, spec));
    CHECK(complete.edits == 0 && scene.Mesh()->skeleton.Handle() == scene.existing);
}

// 병합 충돌(같은 이름 본의 부모 사슬이 다름)은 SkeletonMismatch(ParentChainMismatch)이고 스켈레톤을
// 고치지 않는다.
TEST(SkeletalMeshImport_MergeConflict) {
    ImportScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.SetExisting(1));
    {
        Skeleton* skeleton = scene.Get<Skeleton>(scene.existing);
        REQUIRE(skeleton != nullptr);
        // 스켈레톤: ba → x → bb (메시는 ba → bb).
        REQUIRE(AddBone(scene.names, skeleton->bones, 0, Transform::Identity()));
        skeleton->bones.bones[1].name = scene.Intern("x");
        REQUIRE(AddBone(scene.names, skeleton->bones, 1, Transform::Identity()));
        skeleton->bones.bones[2].name = scene.Intern("bb");
        REQUIRE(skeleton->retargetModes.TryPushBack(BoneTranslationRetarget::Animation) &&
                skeleton->retargetModes.TryPushBack(BoneTranslationRetarget::Animation));
    }

    TestBoneList list;
    SkeletalMeshImportLod lod;
    lod.source = &scene.source;
    SkeletalMeshImportSpec spec = Basic(lod, list);
    spec.skeleton = SkeletonAsset;
    FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
    scene.AddReferences(context);
    SkeletonDiagnostic failure;
    CHECK(!ImportSkeletalMesh(context, scene.mesh, spec, nullptr, &failure));
    CHECK(context.failCode == AssetDiagnosticCode::SkeletonMismatch &&
          failure.issue == SkeletonIssue::ParentChainMismatch && context.edits == 0 &&
          scene.ExistingBones() == 3);
}

// 검증 실패(코드·detail, 저장 전 — bulk를 넘기지 않는다): LOD 0개·9개, 원본 없음, 메시 본 0개
// (EmptyMeshHierarchy)·계층 위반(InvalidSkeletonData), 원본 영향 번호 ≥ 본 수(원본 검증), 머티리얼
// 맵 −2, 소켓 본이 메시 본에 없음, 프로필 이름 겹침, 스켈레톤이 Skeleton이 아님(TypeMismatch,
// detail = 슬롯 수 × 2), output이 SkeletalMesh가 아님.
TEST(SkeletalMeshImport_Rejects) {
    ImportScene scene;
    REQUIRE(scene.ready);
    TestBoneList list;
    SkeletalMeshImportLod lod;
    lod.source = &scene.source;
    const auto rejects = [&](const SkeletalMeshImportSpec& spec, AssetDiagnosticCode code,
                             uint64 detail, ObjectHandle output = {}) noexcept {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
        scene.AddReferences(context);
        const bool failed =
            !ImportSkeletalMesh(context, output.IsNull() ? scene.mesh : output, spec);
        return failed && context.failCode == code && context.failDetail == detail &&
               context.failCount == 1 && context.addedBulks == 0;
    };

    SkeletalMeshImportSpec empty = Basic(lod, list);
    empty.lods = {};
    CHECK(rejects(empty, AssetDiagnosticCode::InvalidSetting, 0));
    SkeletalMeshImportLod many[SkeletalMeshMaxLods + 1];
    for (SkeletalMeshImportLod& one : many) {
        one.source = &scene.source;
    }

    SkeletalMeshImportSpec tooMany = Basic(lod, list);
    tooMany.lods = many;
    CHECK(rejects(tooMany, AssetDiagnosticCode::InvalidSetting, SkeletalMeshMaxLods + 1));
    SkeletalMeshImportLod none;
    CHECK(rejects(Basic(none, list), AssetDiagnosticCode::InvalidSetting, 0));
    const SkeletalMeshImportLod secondMissing[] = {lod, none};
    SkeletalMeshImportSpec second = Basic(lod, list);
    second.lods = secondMissing;
    CHECK(rejects(second, AssetDiagnosticCode::InvalidSetting, 1));
    const StaticMeshImportFloat emptyKey[] = {{"", 0.5f}};
    SkeletalMeshImportLod keyed;
    keyed.source = &scene.source;
    keyed.screenSizePerPlatform = emptyKey;
    CHECK(rejects(Basic(keyed, list), AssetDiagnosticCode::InvalidSetting, 0));
    MeshSectionInfoEntry outside;
    outside.lod = 1;
    const MeshSectionInfoEntry badSections[] = {outside};
    SkeletalMeshImportSpec sectionSpec = Basic(lod, list);
    sectionSpec.sectionInfo = badSections;
    CHECK(rejects(sectionSpec, AssetDiagnosticCode::InvalidSetting, 0));

    SkeletalMeshImportSpec noBones = Basic(lod, list);
    noBones.bones = {};
    CHECK(rejects(noBones, AssetDiagnosticCode::SkeletonMismatch, 0));
    TestBoneList cyclic;
    cyclic.bones[0].parentIndex = 1;
    CHECK(rejects(Basic(lod, cyclic), AssetDiagnosticCode::InvalidSkeletonData, 0));

    // 본 하나뿐이면 원본의 영향 번호 1이 범위 밖이다.
    SkeletalMeshImportSpec oneBone = Basic(lod, list);
    oneBone.bones = Span<const SkeletonImportBone>(list.bones, 1);
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
        MeshDiagnostic meshFailure;
        CHECK(!ImportSkeletalMesh(context, scene.mesh, oneBone, &meshFailure));
        CHECK(context.failCode == meshFailure.code && context.failDetail == 0 &&
              context.failCode != AssetDiagnosticCode::None && context.addedBulks == 0);
    }

    const int32 badMap[] = {-2};
    SkeletalMeshImportLod mapped;
    mapped.source = &scene.source;
    mapped.lodMaterialMap = badMap;
    CHECK(rejects(Basic(mapped, list), AssetDiagnosticCode::InvalidSetting, 0));

    SkeletonImportSocket lost;
    lost.name = "Lost";
    lost.boneName = "nowhere";
    const SkeletonImportSocket sockets[] = {lost};
    SkeletalMeshImportSpec socketSpec = Basic(lod, list);
    socketSpec.sockets = sockets;
    CHECK(rejects(socketSpec, AssetDiagnosticCode::InvalidSetting, 0));
    SkeletonImportSocket hand;
    hand.name = "Hand";
    hand.boneName = "bb";
    const SkeletonImportSocket sameName[] = {hand, hand};
    SkeletalMeshImportSpec duplicateSockets = Basic(lod, list);
    duplicateSockets.sockets = sameName;
    CHECK(rejects(duplicateSockets, AssetDiagnosticCode::InvalidSetting, 1));

    SkeletalMeshImportProfile cloth;
    cloth.name = "Cloth";
    const SkeletalMeshImportProfile twice[] = {cloth, cloth};
    SkeletalMeshImportSpec profileSpec = Basic(lod, list);
    profileSpec.skinWeightProfiles = twice;
    CHECK(rejects(profileSpec, AssetDiagnosticCode::InvalidSetting, 1));

    const StaticMeshImportSlot slots[] = {{"Body", "Body", {}, {}, {}}};
    SkeletalMeshImportSpec wrong = Basic(lod, list);
    wrong.slots = slots;
    wrong.skeleton = WrongSkeleton;
    CHECK(rejects(wrong, AssetDiagnosticCode::TypeMismatch, 2));

    CHECK(rejects(Basic(lod, list), AssetDiagnosticCode::TypeMismatch, 0, scene.existing));
}

// 재임포트: 보존(슬롯 이름·머티리얼, LOD 설정, 옛 소켓 + 명세의 새 이름, 같은 이름 프로필의 기본
// 설정, bounds 확장·minLod)과 명세 값(원본 데이터·본·프로필 목록), replaceSettingsOnReimport면
// 설정도 명세 값.
TEST(SkeletalMeshImport_ReimportPreservesEdits) {
    ImportScene scene;
    REQUIRE(scene.ready);
    TestBoneList list;
    SkeletalMeshImportLod lod;
    lod.source = &scene.source;
    lod.lodHysteresis = 0.1f;
    const StaticMeshImportSlot slots[] = {{"Body", "BodyImported", MaterialAsset, {}, {}}};
    SkeletonImportSocket hand;
    hand.name = "Hand";
    hand.boneName = "bb";
    const SkeletonImportSocket sockets[] = {hand};
    SkeletalMeshImportProfile cloth;
    cloth.name = "Cloth";
    const SkeletalMeshImportProfile profiles[] = {cloth};
    SkeletalMeshImportSpec spec = Basic(lod, list);
    spec.slots = slots;
    spec.sockets = sockets;
    spec.skinWeightProfiles = profiles;
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, spec));
    }

    // 사용자 편집.
    {
        SkeletalMesh* mesh = scene.Mesh();
        REQUIRE(mesh != nullptr);
        mesh->materialSlots[0].slotName = scene.Intern("Renamed");
        mesh->sourceLods[0].lodHysteresis = 0.7f;
        mesh->sourceLods[0].build.useHighPrecisionSkinWeights = true;
        mesh->skinWeightProfiles[0].defaultProfile.defaultValue = true;
        mesh->sockets[0].relativeLocation = Vec3d(9.0, 0.0, 0.0);
        mesh->positiveBoundsExtension = Vec3d(0.0, 0.0, 7.0);
    }

    SkeletonImportSocket foot;
    foot.name = "Foot";
    foot.boneName = "ba";
    const SkeletonImportSocket moreSockets[] = {hand, foot};
    SkeletalMeshImportLod again = lod;
    again.lodHysteresis = 0.3f;
    SkeletalMeshImportSpec next = Basic(again, list);
    next.slots = slots;
    next.sockets = moreSockets;
    next.skinWeightProfiles = profiles;
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator, true);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, next));
    }

    {
        const SkeletalMesh* mesh = scene.Mesh();
        REQUIRE(mesh != nullptr);
        CHECK(mesh->materialSlots[0].slotName == scene.Intern("Renamed") &&
              mesh->sourceLods[0].lodHysteresis == 0.7f &&
              mesh->sourceLods[0].build.useHighPrecisionSkinWeights &&
              mesh->skinWeightProfiles[0].defaultProfile.defaultValue &&
              mesh->positiveBoundsExtension.z == 7.0);
        REQUIRE(mesh->sockets.Size() == 2);
        CHECK(mesh->sockets[0].relativeLocation.x == 9.0 &&
              mesh->sockets[1].name == scene.Intern("Foot"));
    }

    next.replaceSettingsOnReimport = true;
    {
        FakeContext context(*scene.registry, scene.mesh, scene.auxiliary, scene.allocator, true);
        scene.AddReferences(context);
        REQUIRE(ImportSkeletalMesh(context, scene.mesh, next));
    }

    const SkeletalMesh* mesh = scene.Mesh();
    REQUIRE(mesh != nullptr);
    CHECK(mesh->sourceLods[0].lodHysteresis == 0.3f &&
          !mesh->sourceLods[0].build.useHighPrecisionSkinWeights &&
          !mesh->skinWeightProfiles[0].defaultProfile.defaultValue &&
          mesh->positiveBoundsExtension.z == 0.0 && mesh->sockets.Size() == 2 &&
          mesh->sockets[0].relativeLocation.x == 0.0);
}

} // namespace skeletal_mesh_import_tests_file
