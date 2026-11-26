#include <AnimationTools/SkeletonMergeApply.h>

// 본 병합 결과를 스켈레톤에 쓴다(Docs/Design/MeshAnimationAssets 6.6절 5).
namespace rocks {

Result<void> MergeBonesIntoSkeleton(Skeleton& skeleton, const BoneHierarchy& meshBones,
                                    Span<const BoneTranslationRetarget> newModes,
                                    BoneMergeResult& result, SkeletonDiagnostic* failure) noexcept {
    if (!newModes.IsEmpty() && newModes.Size() != meshBones.bones.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 합친 계층과 늘린 리타깃 방식을 모두 만든 뒤에 쓴다(실패하면 스켈레톤이 그대로다). 계층 검증
    // (MergeBoneHierarchy) 다음에 리타깃 방식 길이를 본다(ValidateSkeleton과 같은 순서).
    Allocator& allocator = skeleton.retargetModes.GetAllocator();
    BoneMergeResult merged(allocator);
    if (auto checked = MergeBoneHierarchy(skeleton.bones, meshBones, merged, failure); !checked) {
        return checked;
    }

    const usize oldCount = skeleton.bones.bones.Size();
    if (skeleton.retargetModes.Size() != oldCount) {
        if (failure != nullptr) {
            *failure = SkeletonDiagnostic{};
            failure->code = AssetDiagnosticCode::InvalidSkeletonData;
            failure->error = ErrorCode::InvalidArgument;
            failure->issue = SkeletonIssue::RetargetModeCount;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Array<BoneTranslationRetarget> modes(allocator);
    if (auto reserved = modes.TryReserve(merged.bones.bones.Size()); !reserved) {
        return reserved;
    }

    (void)modes.TryAppend(skeleton.retargetModes.View());
    while (modes.Size() < merged.bones.bones.Size()) {
        (void)modes.TryPushBack(BoneTranslationRetarget::Animation);
    }

    for (usize mesh = 0; mesh < merged.meshToMerged.Size() && !newModes.IsEmpty(); ++mesh) {
        const usize target = static_cast<usize>(merged.meshToMerged[mesh]);
        if (target >= oldCount) {
            modes[target] = newModes[mesh];
        }
    }

    skeleton.bones = Move(merged.bones);
    skeleton.retargetModes = Move(modes);
    result = Move(merged);
    return Result<void>::Success();
}

} // namespace rocks
