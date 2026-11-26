#include "AnimationInternal.h"

#include <Animation/Skeleton.h>

// 스켈레톤 검증과 진단 이름이다(Docs/Design/MeshAnimationAssets 6.2·6.4절).
namespace rocks {

using namespace animation_detail;

namespace {

bool IsFiniteRotator(const Rotator<double>& rotator) noexcept {
    return math::IsFinite(rotator.pitch) && math::IsFinite(rotator.yaw) &&
           math::IsFinite(rotator.roll);
}

bool IsFiniteVector(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

// 앞 count개 가상 본 가운데 name인 것이 있는지다.
bool HasEarlierVirtualBone(Span<const VirtualBone> bones, usize count, Name name) noexcept {
    for (usize at = 0; at < count; ++at) {
        if (bones[at].name == name) {
            return true;
        }
    }

    return false;
}

// 이름 항목 표를 만들고 정렬한다. 오류: 할당 실패.
template <class Item, class NameOf>
Result<void> BuildTable(Span<const Item> items, Array<NamedItem>& out, NameOf&& nameOf) noexcept {
    out.Clear();
    if (auto reserved = out.TryReserve(items.Size()); !reserved) {
        return reserved;
    }

    for (usize at = 0; at < items.Size(); ++at) {
        (void)out.TryPushBack(MakeNamedItem(nameOf(items[at]), static_cast<uint32>(at)));
    }

    SortNamedItems(out.View());
    return Result<void>::Success();
}

Result<void> ValidateVirtualBones(const Skeleton& skeleton, NamePool& names,
                                  Span<const NamedItem> boneNames,
                                  SkeletonDiagnostic* failure) noexcept {
    const Span<const VirtualBone> bones = skeleton.virtualBones.View();
    for (uint32 at = 0; at < bones.Size(); ++at) {
        const VirtualBone& bone = bones[at];
        if (!IsUsableName(bone.name)) {
            return Invalid(failure, SkeletonIssue::VirtualBoneName, at);
        }

        auto lease = names.Acquire(bone.name);
        if (!lease) {
            Report(failure, AssetDiagnosticCode::None, lease.Error(), SkeletonIssue::None,
                   BoneHierarchyRole::Skeleton, at);
            return Result<void>::Failure(lease.Error());
        }

        const StringView text = lease.Value().View();
        if (text.ByteCount() <= VirtualBonePrefix.ByteCount() ||
            !text.StartsWith(VirtualBonePrefix)) {
            return Invalid(failure, SkeletonIssue::VirtualBoneName, at);
        }

        const uint32 sameBone = FindNamed(boneNames, bone.name);
        if (sameBone != SkeletonNoIndex || HasEarlierVirtualBone(bones, at, bone.name)) {
            return Invalid(failure, SkeletonIssue::DuplicateVirtualBone, at, sameBone);
        }

        for (uint32 earlier = 0; earlier < at; ++earlier) {
            if (bones[earlier].sourceBone == bone.sourceBone &&
                bones[earlier].targetBone == bone.targetBone) {
                return Invalid(failure, SkeletonIssue::DuplicateVirtualBone, at, earlier);
            }
        }

        const Name ends[] = {bone.sourceBone, bone.targetBone};
        for (Name end : ends) {
            if (FindNamed(boneNames, end) == SkeletonNoIndex &&
                !HasEarlierVirtualBone(bones, at, end)) {
                return Invalid(failure, SkeletonIssue::VirtualBoneTarget, at);
            }
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateSockets(const Skeleton& skeleton, Span<const NamedItem> boneNames,
                             Allocator& scratch, SkeletonDiagnostic* failure) noexcept {
    Array<NamedItem> table(scratch);
    const Span<const BoneSocket> sockets = skeleton.sockets.View();
    for (uint32 at = 0; at < sockets.Size(); ++at) {
        const BoneSocket& socket = sockets[at];
        if (!IsUsableName(socket.name)) {
            return Invalid(failure, SkeletonIssue::InvalidSocket, at);
        }

        const bool known = FindNamed(boneNames, socket.boneName) != SkeletonNoIndex ||
                           HasEarlierVirtualBone(skeleton.virtualBones.View(),
                                                 skeleton.virtualBones.Size(), socket.boneName);
        if (!known) {
            return Invalid(failure, SkeletonIssue::SocketBone, at);
        }

        if (!IsFiniteVector(socket.relativeLocation) || !IsFiniteRotator(socket.relativeRotation) ||
            !IsFiniteVector(socket.relativeScale)) {
            return Invalid(failure, SkeletonIssue::NonFiniteSocket, at);
        }
    }

    if (auto built =
            BuildTable(sockets, table, [](const BoneSocket& item) noexcept { return item.name; });
        !built) {
        return built;
    }

    uint32 later = 0;
    uint32 earlier = 0;
    if (FindFirstDuplicate(table.View(), later, earlier)) {
        return Invalid(failure, SkeletonIssue::InvalidSocket, later, earlier);
    }

    return Result<void>::Success();
}

Result<void> ValidateCurves(const Skeleton& skeleton, Span<const NamedItem> boneNames,
                            Allocator& scratch, SkeletonDiagnostic* failure) noexcept {
    Array<NamedItem> table(scratch);
    const Span<const SkeletonCurveInfo> curves = skeleton.curves.View();
    for (uint32 at = 0; at < curves.Size(); ++at) {
        if (!IsUsableName(curves[at].name)) {
            return Invalid(failure, SkeletonIssue::InvalidCurve, at);
        }

        for (uint32 link = 0; link < curves[at].linkedBones.Size(); ++link) {
            if (FindNamed(boneNames, curves[at].linkedBones[link]) == SkeletonNoIndex) {
                return Invalid(failure, SkeletonIssue::CurveBone, at, link);
            }
        }
    }

    if (auto built = BuildTable(curves, table,
                                [](const SkeletonCurveInfo& item) noexcept { return item.name; });
        !built) {
        return built;
    }

    uint32 later = 0;
    uint32 earlier = 0;
    if (FindFirstDuplicate(table.View(), later, earlier)) {
        return Invalid(failure, SkeletonIssue::InvalidCurve, later, earlier);
    }

    return Result<void>::Success();
}

// 슬롯 하나다(묶음 번호·묶음 안 번호 — 진단 위치).
struct SlotRef {
    Name name;
    uint32 group = 0;
    uint32 slot = 0;
};

Result<void> ValidateSlotGroups(const Skeleton& skeleton, Allocator& scratch,
                                SkeletonDiagnostic* failure) noexcept {
    Array<NamedItem> table(scratch);
    Array<NamedItem> slotTable(scratch);
    const Span<const AnimSlotGroup> groups = skeleton.slotGroups.View();
    Array<SlotRef> slots(scratch);
    for (uint32 group = 0; group < groups.Size(); ++group) {
        if (!IsUsableName(groups[group].name)) {
            return Invalid(failure, SkeletonIssue::InvalidSlotGroup, group);
        }

        for (uint32 slot = 0; slot < groups[group].slots.Size(); ++slot) {
            if (!IsUsableName(groups[group].slots[slot])) {
                return Invalid(failure, SkeletonIssue::InvalidSlot, group, slot);
            }

            if (auto pushed = slots.TryPushBack(SlotRef{groups[group].slots[slot], group, slot});
                !pushed) {
                return pushed;
            }
        }
    }

    if (auto built =
            BuildTable(groups, table, [](const AnimSlotGroup& item) noexcept { return item.name; });
        !built) {
        return built;
    }

    uint32 later = 0;
    uint32 earlier = 0;
    if (FindFirstDuplicate(table.View(), later, earlier)) {
        return Invalid(failure, SkeletonIssue::InvalidSlotGroup, later, earlier);
    }

    // 슬롯은 모든 묶음을 통틀어 유일하다(UE: 슬롯은 묶음 하나에만 속한다).
    if (auto built = BuildTable(Span<const SlotRef>(slots.View()), slotTable,
                                [](const SlotRef& item) noexcept { return item.name; });
        !built) {
        return built;
    }

    if (FindFirstDuplicate(slotTable.View(), later, earlier)) {
        return Invalid(failure, SkeletonIssue::InvalidSlot, slots[later].group, slots[later].slot);
    }

    return Result<void>::Success();
}

Result<void> ValidateRetargetSources(const Skeleton& skeleton, Allocator& scratch,
                                     SkeletonDiagnostic* failure) noexcept {
    Array<NamedItem> table(scratch);
    const Span<const NamedReferencePose> sources = skeleton.retargetSources.View();
    for (uint32 at = 0; at < sources.Size(); ++at) {
        if (!IsUsableName(sources[at].name)) {
            return Invalid(failure, SkeletonIssue::InvalidRetargetSource, at);
        }

        for (uint32 bone = 0; bone < sources[at].pose.Size(); ++bone) {
            if (!IsFiniteTransform(sources[at].pose[bone])) {
                return Invalid(failure, SkeletonIssue::NonFiniteRetargetPose, at, bone);
            }
        }
    }

    if (auto built = BuildTable(sources, table,
                                [](const NamedReferencePose& item) noexcept { return item.name; });
        !built) {
        return built;
    }

    uint32 later = 0;
    uint32 earlier = 0;
    if (FindFirstDuplicate(table.View(), later, earlier)) {
        return Invalid(failure, SkeletonIssue::InvalidRetargetSource, later, earlier);
    }

    return Result<void>::Success();
}

} // namespace

Result<void> ValidateSkeleton(const Skeleton& skeleton, NamePool& names, Allocator& scratch,
                              SkeletonDiagnostic* failure) noexcept {
    if (auto valid = ValidateBoneHierarchy(skeleton.bones, scratch, failure); !valid) {
        return valid;
    }

    if (skeleton.retargetModes.Size() != skeleton.bones.bones.Size()) {
        return Invalid(failure, SkeletonIssue::RetargetModeCount, SkeletonNoIndex);
    }

    for (uint32 bone = 0; bone < skeleton.retargetModes.Size(); ++bone) {
        if (static_cast<uint32>(skeleton.retargetModes[bone]) >= BoneTranslationRetargetCount) {
            return Invalid(failure, SkeletonIssue::RetargetModeRange, bone);
        }
    }

    Array<NamedItem> boneNames(scratch);
    if (auto built = BuildBoneNameTable(skeleton.bones, boneNames); !built) {
        return built;
    }

    if (auto valid = ValidateVirtualBones(skeleton, names, boneNames.View(), failure); !valid) {
        return valid;
    }

    if (auto valid = ValidateSockets(skeleton, boneNames.View(), scratch, failure); !valid) {
        return valid;
    }

    if (auto valid = ValidateCurves(skeleton, boneNames.View(), scratch, failure); !valid) {
        return valid;
    }

    if (auto valid = ValidateSlotGroups(skeleton, scratch, failure); !valid) {
        return valid;
    }

    return ValidateRetargetSources(skeleton, scratch, failure);
}

const char* SkeletonIssueName(SkeletonIssue issue) noexcept {
    switch (issue) {
    case SkeletonIssue::None:
        return "None";
    case SkeletonIssue::TooManyBones:
        return "TooManyBones";
    case SkeletonIssue::PoseLengthMismatch:
        return "PoseLengthMismatch";
    case SkeletonIssue::RootParent:
        return "RootParent";
    case SkeletonIssue::ParentOrder:
        return "ParentOrder";
    case SkeletonIssue::InvalidBoneName:
        return "InvalidBoneName";
    case SkeletonIssue::DuplicateBoneName:
        return "DuplicateBoneName";
    case SkeletonIssue::NonFinitePose:
        return "NonFinitePose";
    case SkeletonIssue::RotationNotNormalized:
        return "RotationNotNormalized";
    case SkeletonIssue::RetargetModeCount:
        return "RetargetModeCount";
    case SkeletonIssue::RetargetModeRange:
        return "RetargetModeRange";
    case SkeletonIssue::VirtualBoneName:
        return "VirtualBoneName";
    case SkeletonIssue::DuplicateVirtualBone:
        return "DuplicateVirtualBone";
    case SkeletonIssue::VirtualBoneTarget:
        return "VirtualBoneTarget";
    case SkeletonIssue::InvalidSocket:
        return "InvalidSocket";
    case SkeletonIssue::SocketBone:
        return "SocketBone";
    case SkeletonIssue::NonFiniteSocket:
        return "NonFiniteSocket";
    case SkeletonIssue::InvalidCurve:
        return "InvalidCurve";
    case SkeletonIssue::CurveBone:
        return "CurveBone";
    case SkeletonIssue::InvalidSlotGroup:
        return "InvalidSlotGroup";
    case SkeletonIssue::InvalidSlot:
        return "InvalidSlot";
    case SkeletonIssue::InvalidRetargetSource:
        return "InvalidRetargetSource";
    case SkeletonIssue::NonFiniteRetargetPose:
        return "NonFiniteRetargetPose";
    case SkeletonIssue::ParentChainMismatch:
        return "ParentChainMismatch";
    case SkeletonIssue::MissingAncestor:
        return "MissingAncestor";
    case SkeletonIssue::EmptyMeshHierarchy:
        return "EmptyMeshHierarchy";
    case SkeletonIssue::MissingSkeletonBones:
        return "MissingSkeletonBones";
    }

    return "Unknown";
}

AssetDiagnostic ToAssetDiagnostic(const SkeletonDiagnostic& diagnostic, AssetId asset) noexcept {
    AssetDiagnostic out;
    out.code = diagnostic.code;
    out.error = diagnostic.error;
    out.asset = asset;
    out.detail = diagnostic.index == SkeletonNoIndex ? 0 : diagnostic.index;
    return out;
}

} // namespace rocks
