#pragma once

#include <Animation/BoneHierarchy.h>

// 메시 본 계층을 스켈레톤 본 계층에 합치는
// 판정이다(Docs/Design/MeshAnimationAssets.ko.md 6.2·6.4절, UE
// USkeleton::MergeBonesToBoneTree·IsCompatibleMesh·DoesParentChainMatch).
namespace rocks {

// 병합 결과다. bones는 스켈레톤 본을 그 번호 그대로 앞에 두고 스켈레톤에 없던 메시 본을 메시 순서로
// 뒤에 더한 계층이다. meshToMerged[m]은 메시 본 m의 결과 번호다.
//
// 수명: 할당기는 이 객체보다 오래 살아야 한다.
struct BoneMergeResult {
    explicit BoneMergeResult(Allocator& allocator) noexcept
        : bones(allocator), meshToMerged(allocator) {
    }

    BoneMergeResult(BoneMergeResult&&) noexcept = default;
    BoneMergeResult& operator=(BoneMergeResult&&) noexcept = default;

    BoneHierarchy bones;
    Array<int32> meshToMerged;
    // 스켈레톤에 더한 본 수다.
    uint32 addedBones = 0;
    // 대소문자만 다른 이름을 메시 쪽 표시로 바꾼 본 수다(UE와 같다 — 이름과 exportName을 메시
    // 것으로).
    uint32 renamedBones = 0;
};

// mesh를 skeleton에 합칠 수 있는지 판정하고 합친 계층과 대응표를 만든다(순수 함수 — 입력을 바꾸지
// 않는다).
//
// 규칙(UE와 같다, 부모 사슬 검사 켬): skeleton이 비어 있으면 결과는 mesh 그대로다. 아니면 메시
// 본마다 같은 이름의 스켈레톤 본이 있으면 그 본에서, 없으면 스켈레톤에 있는 가장 가까운 메시
// 조상에서 부모 사슬을 대조한다: 두 계층에서 부모 이름을 차례로 비교하며 한쪽이 루트에 닿으면 다른
// 쪽도 루트여야 한다(대조 시작 본이 스켈레톤 루트면 두 루트 이름만 비교). 조상이 하나도 없으면
// MissingAncestor다. 통과하면 없는 본을 메시 순서로 더하고(부모는 메시 부모의 결과 번호, pose는
// 메시 것), 있는 본은 이름이 대소문자만 다를 때 메시 표시로 바꾼다.
//
// 사용 예:
//   BoneMergeResult merged(allocator);
//   SkeletonDiagnostic failure;
//   if (auto result = MergeBoneHierarchy(skeleton->bones, meshBones, merged, &failure); !result) {
//       Report(AssetDiagnosticName(failure.code), SkeletonIssueName(failure.issue), failure.index);
//   }
//
// 오류: 두 계층 검증 실패는 ValidateBoneHierarchy 진단(role이 어느 계층인지)이다. 메시 본이 없으면
// SkeletonMismatch·InvalidArgument·EmptyMeshHierarchy(UE IsCompatibleMesh와 같이 거부)다. 사슬
// 불일치는 failure->code SkeletonMismatch, ErrorCode InvalidArgument, issue ParentChainMismatch·
// MissingAncestor, index = 메시 본, related = 대조한 스켈레톤 본(MissingAncestor면 없음)이다. 결과
// 본 수가 MeshMaxBones를 넘으면 InvalidSkeletonData·TooManyBones·CapacityExceeded다. 할당 실패는
// 진단 코드 없이 그 오류다. 실패하면 out은 그대로다.
//
// 전제: 두 계층의 Name은 같은 pool이 발급했다(Name 비교는 pool 안 토큰 비교다).
// 스레드: 입력을 읽기만 한다.
Result<void> MergeBoneHierarchy(const BoneHierarchy& skeleton, const BoneHierarchy& mesh,
                                BoneMergeResult& out,
                                SkeletonDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
