#pragma once

#include <Animation/Skeleton.h>
#include <Animation/SkeletonMerge.h>
#include <Core/Containers/Span.h>

// 본 병합 결과를 스켈레톤에 쓰는 도우미다(Docs/Design/MeshAnimationAssets 6.6절 5). 스켈레톤
// 재임포트(ImportSkeleton)와 스킨 메시 임포트(MA5b3)가 함께 쓴다.
namespace rocks {

// meshBones를 skeleton에 합친다: MergeBoneHierarchy(6.4절 6 — 순수 판정)로 합친 계층을 만들고
// skeleton.bones를 그것으로 바꾸며, retargetModes를 결과 본 수로 늘려 더한 본에 newModes[메시 본
// 번호](newModes가 비면 Animation — UE 새 본 기본)를 둔다. result는 병합 결과(meshToMerged 대응표·
// 더한 본 수·이름 바꾼 수)를 받는다 — 합친 계층은 skeleton.bones로 옮기므로 result.bones는 비고,
// result는 skeleton의 할당기를 쓴다(skeleton보다 오래 쥐지 않는다).
//
// 사용 예(스킨 메시 임포트):
//   BoneMergeResult merged(allocator);
//   SkeletonDiagnostic failure;
//   if (auto applied = MergeBonesIntoSkeleton(*skeleton, meshBones, {}, merged, &failure);
//       !applied) {
//       const uint64 detail = failure.index == SkeletonNoIndex ? 0 : failure.index;
//       return context.Fail(failure.code, applied.Error(), detail);   // code None = 진단 없음
//   }
//
// 인자: newModes는 비었거나 meshBones 본 수와 같은 길이다(아니면 InvalidArgument — 진단 없음).
// 오류: 차례로 MergeBoneHierarchy의 오류·진단(계층 위반 InvalidSkeletonData, 사슬 불일치
// SkeletonMismatch), skeleton.retargetModes 길이 != 본 수(InvalidArgument — InvalidSkeletonData·
// RetargetModeCount, index 없음 — ValidateSkeleton과 같다), 할당 실패. 실패하면 skeleton과 result는
// 그대로다(할당을 먼저 마친다).
// 전제: 두 계층의 Name은 같은 pool이 발급했다.
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다(skeleton을 바꾼다).
ROCKS_ANIMATIONTOOLS_API Result<void>
MergeBonesIntoSkeleton(Skeleton& skeleton, const BoneHierarchy& meshBones,
                       Span<const BoneTranslationRetarget> newModes, BoneMergeResult& result,
                       SkeletonDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
