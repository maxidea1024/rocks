#include <MeshTools/SkeletalMeshImport.h>

#include <Animation/Skeleton.generated.h>
#include <Animation/SkeletonMerge.h>
#include <AnimationTools/SkeletonMergeApply.h>
#include <Base/Compiler.h>
#include <Core/Text/NamePool.h>
#include <Material/Material.generated.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.generated.h>
#include <MeshSource/MeshSourceValidation.h>
#include <Object/ObjectRegistry.h>

#include "MeshImportCommon.h"

// 스킨 메시 임포트 서비스다(Docs/Design/MeshAnimationAssets 6.7.3절 — MA5b3). 정적 메시 임포트와
// 같은 단계는 MeshImportCommon.h다.
namespace rocks {
// 유니티 빌드에서 정적 임포트(StaticMeshImport.cpp)의 같은 이름 도우미와 겹치지 않게 파일 전용
// namespace에 둔다.
namespace skeletal_mesh_import {

using mesh_import::InternChecked;
using mesh_import::InternProfiles;
using mesh_import::Invalid;
using mesh_import::OpenTyped;
using mesh_import::PreservedSlot;
using mesh_import::ReferencesPerSlot;

// 보조 출력 스켈레톤의 key다(6.7.3절 2 (c) — 출력 경로 <메시 경로>_Skeleton).
constexpr StringView SkeletonOutputKey = "Skeleton";

// 검증을 마친 이름들이다(Borrow 밖에서 intern).
struct InternedNames {
    explicit InternedNames(Allocator& allocator) noexcept
        : slots(allocator), imported(allocator), sockets(allocator), socketBones(allocator),
          screens(allocator), removes(allocator), prioritizes(allocator), profiles(allocator) {
    }

    Array<Name> slots;
    Array<Name> imported;
    Array<Name> sockets;
    Array<Name> socketBones;
    // LOD마다 화면 크기(PerPlatformFloat)·본 목록이다.
    Array<PerPlatformFloat> screens;
    Array<Array<Name>> removes;
    Array<Array<Name>> prioritizes;
    Array<SkinWeightProfileInfo> profiles;
};

// 열린 머티리얼 참조다(null id면 비움).
struct OpenedSlots {
    explicit OpenedSlots(Allocator& allocator) noexcept
        : materials(allocator), overlays(allocator), densities(allocator) {
    }

    Array<ObjectHandle> materials;
    Array<ObjectHandle> overlays;
    Array<MeshUvDensity> densities;
};

Result<void> OutOfMemory(AssetImportContext& context) noexcept {
    return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
}

// 스켈레톤 진단을 문맥 실패로 옮긴다(할당 실패는 진단 코드 없이 — detail은 본 번호, 없으면 0).
Result<void> SkeletonFail(AssetImportContext& context, const SkeletonDiagnostic& diagnostic,
                          ErrorCode error, SkeletonDiagnostic* out) noexcept {
    if (out != nullptr) {
        *out = diagnostic;
    }

    const uint64 detail = diagnostic.index == SkeletonNoIndex ? 0 : diagnostic.index;
    return context.Fail(error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None
                                                        : diagnostic.code,
                        error, detail);
}

// 메시 본 계층을 만든다(이름 intern — 실패는 InvalidSetting·detail = 본, 정규화, 검증 — 위반은
// InvalidSkeletonData·detail = 본).
Result<void> BuildBones(AssetImportContext& context, NamePool& names,
                        const SkeletalMeshImportSpec& spec, Allocator& allocator,
                        BoneHierarchy& out, SkeletonDiagnostic* skeletonFailure) noexcept {
    if (spec.bones.IsEmpty()) {
        SkeletonDiagnostic empty;
        empty.code = AssetDiagnosticCode::SkeletonMismatch;
        empty.error = ErrorCode::InvalidArgument;
        empty.issue = SkeletonIssue::EmptyMeshHierarchy;
        empty.role = BoneHierarchyRole::Mesh;
        return SkeletonFail(context, empty, ErrorCode::InvalidArgument, skeletonFailure);
    }

    if (!out.bones.TryReserve(spec.bones.Size()) ||
        !out.referencePose.TryReserve(spec.bones.Size())) {
        return OutOfMemory(context);
    }

    for (usize index = 0; index < spec.bones.Size(); ++index) {
        const SkeletonImportBone& source = spec.bones[index];
        auto name = InternChecked(context, names, source.name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        BoneInfo bone(allocator);
        bone.name = name.Value();
        bone.parentIndex = source.parentIndex;
#if ROCKS_WITH_EDITOR_DATA
        if (!bone.exportName.TryAssign(source.name)) {
            return OutOfMemory(context);
        }
#endif
        (void)out.bones.TryPushBack(Move(bone));
        (void)out.referencePose.TryPushBack(source.referencePose);
    }

    // 저장 전 정규화(6.4절 3 — 스켈레톤 임포트와 같다).
    (void)NormalizeBoneReferencePose(out);
    SkeletonDiagnostic diagnostic;
    if (auto valid = ValidateBoneHierarchy(out, allocator, &diagnostic); !valid) {
        diagnostic.role = BoneHierarchyRole::Mesh;
        return SkeletonFail(context, diagnostic, valid.Error(), skeletonFailure);
    }

    return Result<void>::Success();
}

// 1단계 검증(6.7.3절 5 — LOD·원본·섹션·슬롯·머티리얼 맵). 메시 데이터 진단은 meshFailure에도 둔다.
Result<void> ValidateMeshes(AssetImportContext& context, const SkeletalMeshImportSpec& spec,
                            uint32 boneCount, MeshDiagnostic* meshFailure) noexcept {
    for (usize index = 0; index < spec.lods.Size(); ++index) {
        const MeshSource* meshes[] = {spec.lods[index].source, spec.lods[index].bakedSource};
        for (const MeshSource* mesh : meshes) {
            if (mesh == nullptr) {
                continue;
            }

            MeshValidateOptions options;
            options.boneCount = boneCount;
            MeshDiagnostic failure;
            if (auto valid = ValidateMeshSource(*mesh, options, &failure); !valid) {
                if (meshFailure != nullptr) {
                    *meshFailure = failure;
                }

                return context.Fail(failure.code, valid.Error(), index);
            }
        }
    }

    if (auto valid = mesh_import::ValidateSectionInfo(context, spec.sectionInfo, spec.lods.Size());
        !valid) {
        return valid;
    }

    if (spec.slots.Size() > SkeletalMeshMaxMaterialSlots) {
        return Invalid(context, spec.slots.Size());
    }

    for (usize index = 0; index < spec.lods.Size(); ++index) {
        for (const int32 remap : spec.lods[index].lodMaterialMap) {
            if (remap < SkeletalMeshNoMaterialRemap) {
                return Invalid(context, index);
            }
        }
    }

    return Result<void>::Success();
}

Result<void> InternList(AssetImportContext& context, NamePool& names, Span<const StringView> texts,
                        uint64 detail, Array<Name>& out) noexcept {
    for (const StringView text : texts) {
        auto name = InternChecked(context, names, text, detail);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        if (!out.TryPushBack(name.Value())) {
            return OutOfMemory(context);
        }
    }

    return Result<void>::Success();
}

// 이름 검증과 intern(6.7.3절 5 — 슬롯·소켓(본은 메시 본)·LOD 본 목록·프로필·프로파일 키).
Result<void> InternNames(AssetImportContext& context, NamePool& names,
                         const SkeletalMeshImportSpec& spec, const BoneHierarchy& bones,
                         Allocator& allocator, PerPlatformInt32& minLod,
                         InternedNames& out) noexcept {
    for (usize index = 0; index < spec.slots.Size(); ++index) {
        auto name = InternChecked(context, names, spec.slots[index].slotName, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        auto imported = InternChecked(context, names, spec.slots[index].importedSlotName, index);
        if (!imported) {
            return Result<void>::Failure(imported.Error());
        }

        if (!out.slots.TryPushBack(name.Value()) || !out.imported.TryPushBack(imported.Value())) {
            return OutOfMemory(context);
        }
    }

    for (usize index = 0; index < spec.sockets.Size(); ++index) {
        auto name = InternChecked(context, names, spec.sockets[index].name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        auto bone = InternChecked(context, names, spec.sockets[index].boneName, index);
        if (!bone) {
            return Result<void>::Failure(bone.Error());
        }

        // 소켓은 메시 본에 붙고(UE USkeletalMeshSocket::BoneName — 없는 본은 거부) 이름이 겹치지
        // 않는다(UE FindSocket은 첫 이름만 찾는다).
        bool duplicate = false;
        for (const Name previous : out.sockets) {
            duplicate = duplicate || previous == name.Value();
        }

        if (duplicate || FindBoneIndex(bones, bone.Value()) == NoParentBone) {
            return Invalid(context, index);
        }

        if (!out.sockets.TryPushBack(name.Value()) || !out.socketBones.TryPushBack(bone.Value())) {
            return OutOfMemory(context);
        }
    }

    for (usize index = 0; index < spec.lods.Size(); ++index) {
        const SkeletalMeshImportLod& lod = spec.lods[index];
        PerPlatformFloat screen(allocator);
        screen.defaultValue = lod.screenSize;
        if (auto made = InternProfiles(context, names, lod.screenSizePerPlatform,
                                       screen.perPlatform, index);
            !made) {
            return made;
        }

        Array<Name> removes(allocator);
        Array<Name> prioritizes(allocator);
        if (auto made = InternList(context, names, lod.bonesToRemove, index, removes); !made) {
            return made;
        }

        if (auto made = InternList(context, names, lod.bonesToPrioritize, index, prioritizes);
            !made) {
            return made;
        }

        if (!out.screens.TryPushBack(Move(screen)) || !out.removes.TryPushBack(Move(removes)) ||
            !out.prioritizes.TryPushBack(Move(prioritizes))) {
            return OutOfMemory(context);
        }
    }

    for (usize index = 0; index < spec.skinWeightProfiles.Size(); ++index) {
        const SkeletalMeshImportProfile& source = spec.skinWeightProfiles[index];
        auto name = InternChecked(context, names, source.name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        for (const SkinWeightProfileInfo& previous : out.profiles) {
            if (previous.name == name.Value()) {
                return Invalid(context, index);
            }
        }

        SkinWeightProfileInfo profile(allocator);
        profile.name = name.Value();
        profile.defaultProfile.defaultValue = source.defaultProfile;
        profile.defaultProfileFromLod.defaultValue = source.defaultProfileFromLod;
        if (auto made = InternProfiles(context, names, source.defaultProfilePerPlatform,
                                       profile.defaultProfile.perPlatform, index);
            !made) {
            return made;
        }

        if (auto made = InternProfiles(context, names, source.defaultProfileFromLodPerPlatform,
                                       profile.defaultProfileFromLod.perPlatform, index);
            !made) {
            return made;
        }

        if (!out.profiles.TryPushBack(Move(profile))) {
            return OutOfMemory(context);
        }
    }

    return InternProfiles(context, names, spec.minLodPerPlatform, minLod.perPlatform, 0);
}

Result<void> OpenSlots(AssetImportContext& context, const SkeletalMeshImportSpec& spec,
                       OpenedSlots& out) noexcept {
    for (usize index = 0; index < spec.slots.Size(); ++index) {
        const uint64 position = index * ReferencesPerSlot;
        auto material =
            OpenTyped(context, spec.slots[index].material, TypeIdOf<MaterialBase>(), position);
        if (!material) {
            return Result<void>::Failure(material.Error());
        }

        auto overlay = OpenTyped(context, spec.slots[index].overlayMaterial,
                                 TypeIdOf<MaterialBase>(), position + 1);
        if (!overlay) {
            return Result<void>::Failure(overlay.Error());
        }

        if (!out.materials.TryPushBack(material.Value()) ||
            !out.overlays.TryPushBack(overlay.Value()) ||
            !out.densities.TryPushBack(spec.slots[index].uvDensity)) {
            return OutOfMemory(context);
        }
    }

    return Result<void>::Success();
}

// 스켈레톤을 정한다(6.7.3절 2·3): null이면 보조 출력, 아니면 기존·구성원 스켈레톤에 메시 본을
// 맞출 수 있는지 판정한다. out은 메시가 참조할 스켈레톤 대표 객체, needsMerge는 기존 스켈레톤에
// 본을 더해야 하는지다(고치기는 다른 단계가 모두 끝난 뒤 MergeIntoSkeleton — 실패해도 이미 열려
// 있던 스켈레톤에 반쯤 고친 값이 남지 않게).
Result<void> ResolveSkeleton(AssetImportContext& context, const SkeletalMeshImportSpec& spec,
                             const BoneHierarchy& meshBones, Allocator& allocator,
                             SkeletonDiagnostic* skeletonFailure, ObjectHandle& out,
                             bool& needsMerge) noexcept {
    needsMerge = false;
    ObjectRegistry& registry = context.Registry();
    if (spec.skeleton.IsNull()) {
        auto made = context.Output(SkeletonOutputKey, TypeIdOf<Skeleton>());
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        SkeletonImportSpec skeleton;
        skeleton.bones = spec.bones;
        if (auto imported = ImportSkeleton(context, made.Value(), skeleton, skeletonFailure);
            !imported) {
            return imported;
        }

        out = made.Value();
        return Result<void>::Success();
    }

    const uint64 position = spec.slots.Size() * ReferencesPerSlot;
    auto opened = OpenTyped(context, spec.skeleton, TypeIdOf<Skeleton>(), position);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    // 판정(읽기 Borrow 안 — 고치기 전에 놓는다).
    uint32 added = 0;
    uint32 firstMissing = SkeletonNoIndex;
    {
        auto borrowed = registry.Resolve<Skeleton>(context.Access(), opened.Value());
        if (!borrowed) {
            return context.Fail(AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return context.Fail(AssetDiagnosticCode::None, value.Error());
        }

        const Skeleton& skeleton = *value.Value();
        BoneMergeResult merged(allocator);
        SkeletonDiagnostic diagnostic;
        if (auto result = MergeBoneHierarchy(skeleton.bones, meshBones, merged, &diagnostic);
            !result) {
            return SkeletonFail(context, diagnostic, result.Error(), skeletonFailure);
        }

        added = merged.addedBones;
        const usize existing = skeleton.bones.bones.Size();
        for (usize bone = 0; bone < merged.meshToMerged.Size(); ++bone) {
            if (static_cast<usize>(merged.meshToMerged[bone]) >= existing) {
                firstMissing = static_cast<uint32>(bone);
                break;
            }
        }
    }

    out = opened.Value();
    if (added == 0) {
        return Result<void>::Success();
    }

    // 묶음 구성원 스켈레톤은 고치지 않는다(그 구성원의 임포터가 만든다 — 6.7.3절 3 (b)).
    for (usize index = 0; index < context.GroupSize(); ++index) {
        if (context.MemberAsset(index) == spec.skeleton) {
            SkeletonDiagnostic missing;
            missing.code = AssetDiagnosticCode::SkeletonMismatch;
            missing.error = ErrorCode::InvalidArgument;
            missing.issue = SkeletonIssue::MissingSkeletonBones;
            missing.role = BoneHierarchyRole::Mesh;
            missing.index = firstMissing;
            return SkeletonFail(context, missing, ErrorCode::InvalidArgument, skeletonFailure);
        }
    }

    needsMerge = true;
    return Result<void>::Success();
}

// 기존 스켈레톤에 모자란 본을 더한다(UE MergeAllBonesToBoneTree — 함께 저장, A2f4). 병합 뒤
// ValidateSkeleton으로 본다(병합은 본 계층만 보므로 가상 본·소켓과의 겹침은 여기서 잡는다 —
// 위반이면 InvalidSkeletonData, 이미 열려 있던 스켈레톤이면 고친 값이 메모리에 dirty로 남는다: A2f4
// 계약).
Result<void> MergeIntoSkeleton(AssetImportContext& context, const SkeletalMeshImportSpec& spec,
                               const BoneHierarchy& meshBones, NamePool& names,
                               Allocator& allocator, SkeletonDiagnostic* skeletonFailure) noexcept {
    ObjectRegistry& registry = context.Registry();
    auto editable = context.EditAsset(spec.skeleton);
    if (!editable) {
        return Result<void>::Failure(editable.Error());
    }

    auto borrowed = registry.ResolveForWrite<Skeleton>(context.Access(), editable.Value());
    if (!borrowed) {
        return context.Fail(AssetDiagnosticCode::None, borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return context.Fail(AssetDiagnosticCode::None, value.Error());
    }

    BoneMergeResult merged(allocator);
    SkeletonDiagnostic diagnostic;
    if (auto applied = MergeBonesIntoSkeleton(*value.Value(), meshBones, {}, merged, &diagnostic);
        !applied) {
        return SkeletonFail(context, diagnostic, applied.Error(), skeletonFailure);
    }

    if (auto valid = ValidateSkeleton(*value.Value(), names, allocator, &diagnostic); !valid) {
        return SkeletonFail(context, diagnostic, valid.Error(), skeletonFailure);
    }

    return Result<void>::Success();
}

} // namespace skeletal_mesh_import

Result<void> ImportSkeletalMesh(AssetImportContext& context, ObjectHandle output,
                                const SkeletalMeshImportSpec& spec, MeshDiagnostic* meshFailure,
                                SkeletonDiagnostic* skeletonFailure) noexcept {
    using namespace skeletal_mesh_import;
    ObjectRegistry& registry = context.Registry();
    const ObjectAccessContext& access = context.Access();
    NamePool* names = registry.Names();
    if (names == nullptr) {
        return context.Fail(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto outputType = registry.AcquireType(access, output);
    if (!outputType) {
        return context.Fail(AssetDiagnosticCode::None, outputType.Error());
    }

    if (outputType.Value().Id() != TypeIdOf<SkeletalMesh>()) {
        return context.Fail(AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch);
    }

    Allocator& allocator = registry.ObjectAllocator();

    // 1. 검증(저장 전에 모두 — 6.7.3절 5).
    if (spec.lods.IsEmpty() || spec.lods.Size() > SkeletalMeshMaxLods) {
        return Invalid(context, spec.lods.Size());
    }

    for (usize index = 0; index < spec.lods.Size(); ++index) {
        if (spec.lods[index].source == nullptr && spec.lods[index].bakedSource == nullptr) {
            return Invalid(context, index);
        }
    }

    BoneHierarchy bones(allocator);
    if (auto built = BuildBones(context, *names, spec, allocator, bones, skeletonFailure); !built) {
        return built;
    }

    if (auto valid =
            ValidateMeshes(context, spec, static_cast<uint32>(bones.bones.Size()), meshFailure);
        !valid) {
        return valid;
    }

    InternedNames interned(allocator);
    PerPlatformInt32 minLod(allocator);
    if (auto named = InternNames(context, *names, spec, bones, allocator, minLod, interned);
        !named) {
        return named;
    }

    OpenedSlots slots(allocator);
    if (auto opened = OpenSlots(context, spec, slots); !opened) {
        return opened;
    }

    // 2. 스켈레톤(보조 출력·참조·본 더하기).
    ObjectHandle skeleton{};
    bool needsMerge = false;
    if (auto resolved =
            ResolveSkeleton(context, spec, bones, allocator, skeletonFailure, skeleton, needsMerge);
        !resolved) {
        return resolved;
    }

    // 3. 원본 → 출력 패키지 bulk.
    Array<SkeletalMeshSourceLod> lods(allocator);
    if (auto reserved = lods.TryReserve(spec.lods.Size()); !reserved) {
        return context.Fail(AssetDiagnosticCode::None, reserved.Error());
    }

    for (usize index = 0; index < spec.lods.Size(); ++index) {
        const SkeletalMeshImportLod& source = spec.lods[index];
        SkeletalMeshSourceLod lod(allocator);
        lod.hasSource = source.source != nullptr;
        lod.hasBakedSource = source.bakedSource != nullptr;
        if (lod.hasSource) {
            if (auto stored = mesh_import::StoreMesh(context, output, *names, *source.source,
                                                     lod.source, allocator);
                !stored) {
                return context.Fail(AssetDiagnosticCode::None, stored.Error(), index);
            }
        }

        if (lod.hasBakedSource) {
            if (auto stored = mesh_import::StoreMesh(context, output, *names, *source.bakedSource,
                                                     lod.bakedSource, allocator);
                !stored) {
                return context.Fail(AssetDiagnosticCode::None, stored.Error(), index);
            }
        }

        lod.build = source.build;
        lod.reduction = source.reduction;
        lod.screenSize = Move(interned.screens[index]);
        lod.lodHysteresis = source.lodHysteresis;
        lod.weightOfPrioritization = source.weightOfPrioritization;
        lod.bonesToRemove = Move(interned.removes[index]);
        lod.bonesToPrioritize = Move(interned.prioritizes[index]);
        if (!lod.lodMaterialMap.TryAppend(source.lodMaterialMap)) {
            return OutOfMemory(context);
        }

        (void)lods.TryPushBack(Move(lod));
    }

    // 4. 필드 쓰기(재임포트는 사용자 편집을 보존 — 6.7.3절 5).
    auto borrowed = registry.ResolveForWrite<SkeletalMesh>(access, output);
    if (!borrowed) {
        return context.Fail(AssetDiagnosticCode::None, borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return context.Fail(AssetDiagnosticCode::None, value.Error());
    }

    SkeletalMesh& mesh = *value.Value();
    const bool reimport = context.IsReimport();
    const bool keepSettings = reimport && !spec.replaceSettingsOnReimport;
    Array<PreservedSlot> oldSlots(allocator);
    if (reimport) {
        if (auto collected = mesh_import::CollectSlots(mesh.materialSlots, oldSlots); !collected) {
            return context.Fail(AssetDiagnosticCode::None, collected.Error());
        }
    }

    // 원본 LOD: 데이터는 새 값, 설정은 옛 LOD가 있으면 보존.
    Result<void> step = Result<void>::Success();
    for (usize index = 0;
         keepSettings && step && index < lods.Size() && index < mesh.sourceLods.Size(); ++index) {
        const SkeletalMeshSourceLod& old = mesh.sourceLods[index];
        SkeletalMeshSourceLod& lod = lods[index];
        lod.build = old.build;
        lod.reduction = old.reduction;
        lod.lodHysteresis = old.lodHysteresis;
        lod.weightOfPrioritization = old.weightOfPrioritization;
        step = mesh_import::CopyPerPlatform(old.screenSize, lod.screenSize);
        lod.lodMaterialMap.Clear();
        lod.bonesToRemove.Clear();
        lod.bonesToPrioritize.Clear();
        if (step) {
            step = lod.lodMaterialMap.TryAppend(old.lodMaterialMap.View());
        }

        if (step) {
            step = lod.bonesToRemove.TryAppend(old.bonesToRemove.View());
        }

        if (step) {
            step = lod.bonesToPrioritize.TryAppend(old.bonesToPrioritize.View());
        }
    }

    // 가중치 프로필: 목록은 명세(원본의 데이터), 같은 이름의 옛 기본 프로필 설정은 보존.
    for (usize index = 0; keepSettings && step && index < interned.profiles.Size(); ++index) {
        SkinWeightProfileInfo& profile = interned.profiles[index];
        for (const SkinWeightProfileInfo& old : mesh.skinWeightProfiles) {
            if (old.name == profile.name) {
                step = mesh_import::CopyPerPlatform(old.defaultProfile, profile.defaultProfile);
                if (step) {
                    step = mesh_import::CopyPerPlatform(old.defaultProfileFromLod,
                                                        profile.defaultProfileFromLod);
                }

                break;
            }
        }
    }

    // 소켓: 보존이면 새 메시 본에 그 본이 있는 옛 소켓을 두고 명세의 새 이름만 더한다(BoneSocket에
    // 임포트 표시가 없다 — UE SkeletalMeshImportUtils의 소켓 복원도 본이 남은 소켓만 되살린다).
    Array<BoneSocket> sockets(allocator);
    for (usize index = 0; step && keepSettings && index < mesh.sockets.Size(); ++index) {
        if (FindBoneIndex(bones, mesh.sockets[index].boneName) != NoParentBone) {
            step = sockets.TryPushBack(mesh.sockets[index]);
        }
    }

    for (usize index = 0; step && index < spec.sockets.Size(); ++index) {
        bool present = false;
        for (const BoneSocket& socket : sockets) {
            present = present || socket.name == interned.sockets[index];
        }

        if (present) {
            continue;
        }

        const SkeletonImportSocket& source = spec.sockets[index];
        BoneSocket socket;
        socket.name = interned.sockets[index];
        socket.boneName = interned.socketBones[index];
        socket.relativeLocation = source.relativeLocation;
        socket.relativeRotation = source.relativeRotation;
        socket.relativeScale = source.relativeScale;
        socket.forceAlwaysAnimated = source.forceAlwaysAnimated;
        step = sockets.TryPushBack(socket);
    }

    if (!step) {
        return context.Fail(AssetDiagnosticCode::None, step.Error());
    }

    mesh.bones = Move(bones);
    mesh.sourceLods = Move(lods);
    if (!reimport || !spec.sectionInfo.IsEmpty()) {
        mesh.sectionInfo.Clear();
        step = mesh.sectionInfo.TryAppend(spec.sectionInfo);
    }

    if (step) {
        step = mesh_import::WriteSlots(registry, access, output, mesh.materialSlots,
                                       oldSlots.View(), interned.imported.View(),
                                       interned.slots.View(), slots.materials.View(),
                                       slots.overlays.View(), slots.densities.View());
    }

    if (step) {
        step = registry.SetReference(access, output, mesh.skeleton, skeleton);
    }

    if (!step) {
        return context.Fail(AssetDiagnosticCode::None, step.Error());
    }

    mesh.sockets = Move(sockets);
    mesh.skinWeightProfiles = Move(interned.profiles);
    if (!keepSettings) {
        mesh.positiveBoundsExtension = spec.positiveBoundsExtension;
        mesh.negativeBoundsExtension = spec.negativeBoundsExtension;
        minLod.defaultValue = spec.minLod;
        mesh.minLod = Move(minLod);
        mesh.virtualGeometry = spec.virtualGeometry;
    }

    SkeletalMeshRenderData empty(allocator);
    mesh.renderData = Move(empty);

    // 5. 기존 스켈레톤 고치기(마지막 — 6.7.3절 3).
    if (needsMerge) {
        return MergeIntoSkeleton(context, spec, mesh.bones, *names, allocator, skeletonFailure);
    }

    return Result<void>::Success();
}

} // namespace rocks
