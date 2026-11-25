#include "AnimationInternal.h"

#include <Animation/SkeletonMerge.h>

// 본 계층 병합이다(Docs/Design/MeshAnimationAssets 6.2·6.4절, UE
// USkeleton::MergeBonesToBoneTree — 부모 사슬 검사를 켠 기본 경로).
namespace rocks {

using namespace animation_detail;

namespace {

Result<void> Mismatch(SkeletonDiagnostic* failure, SkeletonIssue issue, uint32 meshBone,
                      uint32 skeletonBone) noexcept {
    Report(failure, AssetDiagnosticCode::SkeletonMismatch, ErrorCode::InvalidArgument, issue,
           BoneHierarchyRole::Mesh, meshBone, skeletonBone);
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// UE DoesParentChainMatch: 스켈레톤 본 skeletonBone과 같은 이름의 메시 본에서 시작해 부모 이름을
// 차례로 비교한다. 한쪽이 루트에 닿으면 다른 쪽도 루트여야 한다. 시작 본이 스켈레톤 루트면 두 루트
// 이름만 비교한다.
bool ParentChainMatches(const BoneHierarchy& skeleton, const BoneHierarchy& mesh,
                        Span<const NamedItem> meshNames, uint32 skeletonBone) noexcept {
    if (skeletonBone == 0) {
        return skeleton.bones[0].name == mesh.bones[0].name;
    }

    // 호출자가 같은 이름의 메시 본이 있음을 확인했다.
    int32 skeletonIndex = static_cast<int32>(skeletonBone);
    int32 meshIndex = static_cast<int32>(FindNamed(meshNames, skeleton.bones[skeletonBone].name));
    for (;;) {
        const int32 skeletonParent = skeleton.bones[static_cast<usize>(skeletonIndex)].parentIndex;
        const int32 meshParent = mesh.bones[static_cast<usize>(meshIndex)].parentIndex;
        if (skeletonParent == NoParentBone || meshParent == NoParentBone) {
            return skeletonParent == meshParent;
        }

        if (!(skeleton.bones[static_cast<usize>(skeletonParent)].name ==
              mesh.bones[static_cast<usize>(meshParent)].name)) {
            return false;
        }

        skeletonIndex = skeletonParent;
        meshIndex = meshParent;
    }
}

// UE IsCompatibleMesh(부모 사슬 검사 켬): 메시 본마다 같은 이름의 스켈레톤 본 또는 스켈레톤에 있는
// 가장 가까운 메시 조상에서 사슬을 대조한다.
Result<void> CheckCompatible(const BoneHierarchy& skeleton, const BoneHierarchy& mesh,
                             Span<const NamedItem> skeletonNames, Span<const NamedItem> meshNames,
                             SkeletonDiagnostic* failure) noexcept {
    for (uint32 meshBone = 0; meshBone < mesh.bones.Size(); ++meshBone) {
        uint32 skeletonBone = FindNamed(skeletonNames, mesh.bones[meshBone].name);
        int32 current = static_cast<int32>(meshBone);
        while (skeletonBone == SkeletonNoIndex && current != NoParentBone) {
            current = mesh.bones[static_cast<usize>(current)].parentIndex;
            if (current != NoParentBone) {
                skeletonBone =
                    FindNamed(skeletonNames, mesh.bones[static_cast<usize>(current)].name);
            }
        }

        if (skeletonBone == SkeletonNoIndex) {
            return Mismatch(failure, SkeletonIssue::MissingAncestor, meshBone, SkeletonNoIndex);
        }

        if (!ParentChainMatches(skeleton, mesh, meshNames, skeletonBone)) {
            return Mismatch(failure, SkeletonIssue::ParentChainMismatch, meshBone, skeletonBone);
        }
    }

    return Result<void>::Success();
}

// 검증 진단의 계층 표시를 role로 바꿔 돌려준다.
Result<void> ValidateRole(const BoneHierarchy& hierarchy, Allocator& scratch,
                          BoneHierarchyRole role, SkeletonDiagnostic* failure) noexcept {
    auto valid = ValidateBoneHierarchy(hierarchy, scratch, failure);
    if (!valid && failure != nullptr) {
        failure->role = role;
    }

    return valid;
}

} // namespace

Result<void> MergeBoneHierarchy(const BoneHierarchy& skeleton, const BoneHierarchy& mesh,
                                BoneMergeResult& out, SkeletonDiagnostic* failure) noexcept {
    Allocator& allocator = out.meshToMerged.GetAllocator();
    if (auto valid = ValidateRole(skeleton, allocator, BoneHierarchyRole::Skeleton, failure);
        !valid) {
        return valid;
    }

    if (auto valid = ValidateRole(mesh, allocator, BoneHierarchyRole::Mesh, failure); !valid) {
        return valid;
    }

    // UE IsCompatibleMesh는 맞는 본이 하나도 없으면(NumOfBoneMatches == 0) 거부하고,
    // CreateReferenceSkeletonFromMesh도 본이 없으면 만들지 않는다 — 본 없는 메시는 병합하지 않는다.
    if (mesh.bones.IsEmpty()) {
        return Mismatch(failure, SkeletonIssue::EmptyMeshHierarchy, SkeletonNoIndex,
                        SkeletonNoIndex);
    }

    Array<NamedItem> skeletonNames(allocator);
    Array<NamedItem> meshNames(allocator);
    if (auto built = BuildBoneNameTable(skeleton, skeletonNames); !built) {
        return built;
    }

    if (auto built = BuildBoneNameTable(mesh, meshNames); !built) {
        return built;
    }

    if (!skeleton.bones.IsEmpty()) {
        if (auto compatible =
                CheckCompatible(skeleton, mesh, skeletonNames.View(), meshNames.View(), failure);
            !compatible) {
            return compatible;
        }
    }

    // 결과 본 수 = 스켈레톤 본 + 스켈레톤에 없는 메시 본.
    usize added = 0;
    for (const BoneInfo& bone : mesh.bones) {
        added += FindNamed(skeletonNames.View(), bone.name) == SkeletonNoIndex ? 1 : 0;
    }

    if (skeleton.bones.Size() + added > MeshMaxBones) {
        Report(failure, AssetDiagnosticCode::InvalidSkeletonData, ErrorCode::CapacityExceeded,
               SkeletonIssue::TooManyBones, BoneHierarchyRole::Mesh, MeshMaxBones);
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    BoneMergeResult result(allocator);
    if (auto copied = CopyHierarchy(skeleton, result.bones); !copied) {
        return copied;
    }

    if (auto resized = result.meshToMerged.TryResize(mesh.bones.Size()); !resized) {
        return resized;
    }

    for (uint32 meshBone = 0; meshBone < mesh.bones.Size(); ++meshBone) {
        const BoneInfo& bone = mesh.bones[meshBone];
        const uint32 existing = FindNamed(skeletonNames.View(), bone.name);
        if (existing != SkeletonNoIndex) {
            result.meshToMerged[meshBone] = static_cast<int32>(existing);
            BoneInfo& target = result.bones.bones[existing];
            // 대소문자만 다른 이름(같은 토큰 id, 다른 표시)은 메시 표시로 바꾼다(UE Rename).
            if (target.name.displayId != bone.name.displayId) {
                if (auto assigned = target.exportName.TryAssign(bone.exportName.View());
                    !assigned) {
                    return assigned;
                }

                target.name = bone.name;
                ++result.renamedBones;
            }

            continue;
        }

        // 부모는 메시 순서상 앞이라 이미 대응이 있다(루트는 스켈레톤이 비었을 때만 여기 온다).
        BoneInfo copy(allocator);
        copy.name = bone.name;
        copy.parentIndex = bone.parentIndex == NoParentBone
                               ? NoParentBone
                               : result.meshToMerged[static_cast<usize>(bone.parentIndex)];
        if (auto assigned = copy.exportName.TryAssign(bone.exportName.View()); !assigned) {
            return assigned;
        }

        result.meshToMerged[meshBone] = static_cast<int32>(result.bones.bones.Size());
        if (auto pushed = result.bones.bones.TryPushBack(Move(copy)); !pushed) {
            return pushed;
        }

        if (auto pushed = result.bones.referencePose.TryPushBack(mesh.referencePose[meshBone]);
            !pushed) {
            return pushed;
        }

        ++result.addedBones;
    }

    out = Move(result);
    return Result<void>::Success();
}

} // namespace rocks
