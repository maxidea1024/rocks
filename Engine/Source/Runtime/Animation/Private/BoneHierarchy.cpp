#include "AnimationInternal.h"

#include <Core/Algorithms.h>

// 본 계층 검증·정규화·조회와 모듈 내부 공용
// 함수다(Docs/Design/MeshAnimationAssets.ko.md 6.1·6.4절).
namespace rocks {

namespace animation_detail {

void SortNamedItems(Span<NamedItem> items) noexcept {
    algo::Sort(items, [](const NamedItem& a, const NamedItem& b) noexcept {
        if (a.domain != b.domain) {
            return a.domain < b.domain;
        }

        if (a.id != b.id) {
            return a.id < b.id;
        }

        if (a.number != b.number) {
            return a.number < b.number;
        }

        return a.index < b.index;
    });
}

namespace {

bool SameName(const NamedItem& a, const NamedItem& b) noexcept {
    return a.domain == b.domain && a.id == b.id && a.number == b.number;
}

} // namespace

bool FindFirstDuplicate(Span<const NamedItem> items, uint32& later, uint32& earlier) noexcept {
    // 같은 이름 묶음 안은 번호 오름차순이라, 묶음의 두 번째 원소가 그 묶음에서 가장 작은 "뒤"
    // 번호이고 바로 앞 원소가 묶음의 첫 번호다(세 번째 이후 원소는 두 번째보다 번호가 커 고를 일이
    // 없다).
    bool found = false;
    for (usize at = 1; at < items.Size(); ++at) {
        if (SameName(items[at], items[at - 1]) && (!found || items[at].index < later)) {
            found = true;
            later = items[at].index;
            earlier = items[at - 1].index;
        }
    }

    return found;
}

uint32 FindNamed(Span<const NamedItem> items, Name name) noexcept {
    const NamedItem key = MakeNamedItem(name, 0);
    usize low = 0;
    usize high = items.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const NamedItem& item = items[middle];
        const bool less = item.domain != key.domain ? item.domain < key.domain
                          : item.id != key.id       ? item.id < key.id
                                                    : item.number < key.number;
        if (less) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < items.Size() && SameName(items[low], key) ? items[low].index : SkeletonNoIndex;
}

void Report(SkeletonDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error,
            SkeletonIssue issue, BoneHierarchyRole role, uint32 index, uint32 related) noexcept {
    if (failure != nullptr) {
        SkeletonDiagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.error = error;
        diagnostic.issue = issue;
        diagnostic.role = role;
        diagnostic.index = index;
        diagnostic.related = related;
        *failure = diagnostic;
    }
}

Result<void> Invalid(SkeletonDiagnostic* failure, SkeletonIssue issue, uint32 index,
                     uint32 related) noexcept {
    Report(failure, AssetDiagnosticCode::InvalidSkeletonData, ErrorCode::InvalidArgument, issue,
           BoneHierarchyRole::Skeleton, index, related);
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsFiniteTransform(const Transform& transform) noexcept {
    const double values[] = {transform.rotation.x,    transform.rotation.y,
                             transform.rotation.z,    transform.rotation.w,
                             transform.translation.x, transform.translation.y,
                             transform.translation.z, transform.scale.x,
                             transform.scale.y,       transform.scale.z};
    for (double value : values) {
        if (!math::IsFinite(value)) {
            return false;
        }
    }

    return true;
}

Result<void> AppendBone(Array<BoneInfo>& to, const BoneInfo& bone) noexcept {
    BoneInfo copy(to.GetAllocator());
    copy.name = bone.name;
    copy.parentIndex = bone.parentIndex;
    if (auto assigned = copy.exportName.TryAssign(bone.exportName.View()); !assigned) {
        return assigned;
    }

    return to.TryPushBack(Move(copy));
}

Result<void> CopyHierarchy(const BoneHierarchy& from, BoneHierarchy& to) noexcept {
    if (auto reserved = to.bones.TryReserve(from.bones.Size()); !reserved) {
        return reserved;
    }

    for (const BoneInfo& bone : from.bones) {
        if (auto appended = AppendBone(to.bones, bone); !appended) {
            return appended;
        }
    }

    return to.referencePose.TryAppend(from.referencePose.View());
}

Result<void> BuildBoneNameTable(const BoneHierarchy& hierarchy, Array<NamedItem>& out) noexcept {
    out.Clear();
    if (auto reserved = out.TryReserve(hierarchy.bones.Size()); !reserved) {
        return reserved;
    }

    for (usize bone = 0; bone < hierarchy.bones.Size(); ++bone) {
        (void)out.TryPushBack(MakeNamedItem(hierarchy.bones[bone].name, static_cast<uint32>(bone)));
    }

    SortNamedItems(out.View());
    return Result<void>::Success();
}

} // namespace animation_detail

using namespace animation_detail;

Result<void> ValidateBoneHierarchy(const BoneHierarchy& hierarchy, Allocator& scratch,
                                   SkeletonDiagnostic* failure) noexcept {
    const usize count = hierarchy.bones.Size();
    if (count > MeshMaxBones) {
        Report(failure, AssetDiagnosticCode::InvalidSkeletonData, ErrorCode::CapacityExceeded,
               SkeletonIssue::TooManyBones, BoneHierarchyRole::Skeleton, MeshMaxBones);
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (hierarchy.referencePose.Size() != count) {
        return Invalid(failure, SkeletonIssue::PoseLengthMismatch, SkeletonNoIndex);
    }

    for (uint32 bone = 0; bone < count; ++bone) {
        const BoneInfo& info = hierarchy.bones[bone];
        if (bone == 0 && info.parentIndex != NoParentBone) {
            return Invalid(failure, SkeletonIssue::RootParent, bone);
        }

        if (bone != 0 && (info.parentIndex < 0 || static_cast<uint32>(info.parentIndex) >= bone)) {
            return Invalid(failure, SkeletonIssue::ParentOrder, bone);
        }

        if (!IsUsableName(info.name)) {
            return Invalid(failure, SkeletonIssue::InvalidBoneName, bone);
        }

        const Transform& pose = hierarchy.referencePose[bone];
        if (!IsFiniteTransform(pose)) {
            return Invalid(failure, SkeletonIssue::NonFinitePose, bone);
        }

        const Quatd& rotation = pose.rotation;
        const double length = rotation.x * rotation.x + rotation.y * rotation.y +
                              rotation.z * rotation.z + rotation.w * rotation.w;
        if (math::Abs(length - 1.0) > BoneRotationNormalizedTolerance) {
            return Invalid(failure, SkeletonIssue::RotationNotNormalized, bone);
        }
    }

    Array<NamedItem> names(scratch);
    if (auto built = BuildBoneNameTable(hierarchy, names); !built) {
        return built;
    }

    uint32 later = 0;
    uint32 earlier = 0;
    if (FindFirstDuplicate(names.View(), later, earlier)) {
        return Invalid(failure, SkeletonIssue::DuplicateBoneName, later, earlier);
    }

    return Result<void>::Success();
}

uint32 NormalizeBoneReferencePose(BoneHierarchy& hierarchy) noexcept {
    uint32 changed = 0;
    for (Transform& pose : hierarchy.referencePose) {
        Quatd& rotation = pose.rotation;
        const double length = rotation.x * rotation.x + rotation.y * rotation.y +
                              rotation.z * rotation.z + rotation.w * rotation.w;
        if (!math::IsFinite(length) || length == 1.0) {
            continue;
        }

        if (length < BoneRotationMinimumLengthSquared) {
            rotation = Quatd::Identity();
        } else {
            const double scale = math::InvSqrt(length);
            rotation = Quatd(rotation.x * scale, rotation.y * scale, rotation.z * scale,
                             rotation.w * scale);
        }

        ++changed;
    }

    return changed;
}

int32 FindBoneIndex(const BoneHierarchy& hierarchy, Name name) noexcept {
    for (usize bone = 0; bone < hierarchy.bones.Size(); ++bone) {
        if (hierarchy.bones[bone].name == name) {
            return static_cast<int32>(bone);
        }
    }

    return NoParentBone;
}

} // namespace rocks
