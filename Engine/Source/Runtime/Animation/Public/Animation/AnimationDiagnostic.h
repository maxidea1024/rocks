#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Base/NumericLimits.h>

// 스켈레톤·본 계층 진단이다(Docs/Design/MeshAnimationAssets.ko.md 6.4절·11절).
namespace rocks {

// 진단이 위치를 가리키지 않을 때의 번호다.
inline constexpr uint32 SkeletonNoIndex = NumericLimits<uint32>::Max();

// 진단이 가리키는 세부 규칙이다(6.4절). AssetDiagnosticCode 하나(InvalidSkeletonData·
// SkeletonMismatch) 아래에서 어느 규칙을 어겼는지 가른다. 값은 저장 형식이 아니다.
enum class SkeletonIssue : uint8 {
    None,
    // 본 계층(ValidateBoneHierarchy — InvalidSkeletonData).
    TooManyBones,          // 본 수 > MeshMaxBones(병합 결과 포함)
    PoseLengthMismatch,    // referencePose 길이 != 본 수
    RootParent,            // 본 0의 부모가 NoParentBone이 아님
    ParentOrder,           // 본 i(≥ 1)의 부모가 [0, i) 밖(두 번째 루트 포함)
    InvalidBoneName,       // 본 이름이 None이거나 토큰 구조가 잘못됨
    DuplicateBoneName,     // 이름이 Name 비교 규칙으로 앞 본과 같음
    NonFinitePose,         // 기준 pose 성분이 비유한
    RotationNotNormalized, // 기준 pose 회전 |길이² − 1| > BoneRotationNormalizedTolerance
    // 스켈레톤(ValidateSkeleton — InvalidSkeletonData).
    RetargetModeCount,     // retargetModes 길이 != 본 수
    RetargetModeRange,     // BoneTranslationRetarget 값이 열거 밖
    VirtualBoneName,       // 가상 본 이름이 None·"VB " 접두 없음·접두뿐
    DuplicateVirtualBone,  // 가상 본 이름이 본·앞 가상 본과 같거나 (원본, 대상) 쌍이 같음
    VirtualBoneTarget,     // 가상 본의 원본·대상이 본·앞 가상 본에 없음
    InvalidSocket,         // 소켓 이름이 None이거나 앞 소켓과 같음
    SocketBone,            // 소켓 본이 본·가상 본에 없음
    NonFiniteSocket,       // 소켓 상대 변환 성분이 비유한
    InvalidCurve,          // 커브 이름이 None이거나 앞 커브와 같음
    CurveBone,             // 커브 linkedBones의 이름이 본에 없음
    InvalidSlotGroup,      // 슬롯 묶음 이름이 None이거나 앞 묶음과 같음
    InvalidSlot,           // 슬롯 이름이 None이거나 어느 묶음의 앞 슬롯과 같음
    InvalidRetargetSource, // 리타깃 원본 이름이 None이거나 앞 원본과 같음
    NonFiniteRetargetPose, // 리타깃 원본 pose 성분이 비유한
    // 병합(MergeBoneHierarchy — SkeletonMismatch).
    ParentChainMismatch, // 같은 이름 본의 부모 사슬 이름이 스켈레톤과 다름
    MissingAncestor      // 메시 본과 그 조상이 스켈레톤에 하나도 없음(루트가 다름)
};

// 세부 규칙 이름이다(예: ParentOrder → "ParentOrder"). 모르는 값은 "Unknown"이다. 정적 수명이다.
const char* SkeletonIssueName(SkeletonIssue issue) noexcept;

// 진단이 가리키는 계층이다(병합은 두 계층을 본다).
enum class BoneHierarchyRole : uint8 {
    Skeleton,
    Mesh
};

// 스켈레톤·본 계층 진단 하나다. 실패한 호출이 out 인자로 채운다(성공하면 건드리지 않는다).
struct SkeletonDiagnostic {
    // AssetSystem 4.6절 진단 이름이다(InvalidSkeletonData·SkeletonMismatch). 할당 실패는 None이다.
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    // 실패의 ErrorCode다(호출이 돌려준 것과 같다).
    ErrorCode error = ErrorCode::None;
    SkeletonIssue issue = SkeletonIssue::None;
    BoneHierarchyRole role = BoneHierarchyRole::Skeleton;
    // 처음 걸린 항목 번호(본·가상 본·소켓·커브·슬롯 묶음·리타깃 원본 번호)다. 없으면
    // SkeletonNoIndex다.
    uint32 index = SkeletonNoIndex;
    // 관련 항목 번호다(같은 이름의 앞 항목, 병합에서 대조한 스켈레톤 본, 슬롯 묶음 안 슬롯 번호).
    // 없으면 SkeletonNoIndex다.
    uint32 related = SkeletonNoIndex;
};

// diagnostic을 AssetSystem 4.6절 형식으로 옮긴다: code·error는 그대로, asset은 인자, section은 0,
// detail은 index(SkeletonNoIndex면 0)다.
AssetDiagnostic ToAssetDiagnostic(const SkeletonDiagnostic& diagnostic, AssetId asset) noexcept;

} // namespace rocks
