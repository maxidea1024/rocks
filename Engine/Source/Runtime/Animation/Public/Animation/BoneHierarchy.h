#pragma once

#include <Base/Compiler.h>
#include <Animation/AnimationDiagnostic.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Transform.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// 본 계층이다(Docs/Design/MeshAnimationAssets.ko.md 6.1·6.4절, UE FReferenceSkeleton의 raw 계층).
namespace rocks {

// 루트 본의 부모 번호다(UE INDEX_NONE).
inline constexpr int32 NoParentBone = -1;

// 계층 하나의 최대 본 수다(본 번호가 uint16 — UE FBoneIndexType, 3.2절 BoneInfluence::boneIndex).
inline constexpr uint32 MeshMaxBones = 65535;

// 기준 pose 회전을 정규화된 것으로 보는 |길이² − 1|의 상한이다(UE THRESH_QUAT_NORMALIZED —
// FTransform::IsRotationNormalized).
inline constexpr double BoneRotationNormalizedTolerance = 0.01;

// 정규화할 수 없는 회전의 길이² 하한이다(UE FQuat::Normalize의 UE_SMALL_NUMBER — 그 아래는
// 단위 회전으로 바꾼다).
inline constexpr double BoneRotationMinimumLengthSquared = 1.e-8;

// 본 하나다. name이 본의 안정 식별자다(메시·클립·스켈레톤 사이 대응은 이름으로 한다 — UE와 같다).
ROCKS_STRUCT()
struct BoneInfo {
    ROCKS_GENERATED_BODY()

    explicit BoneInfo(Allocator& allocator) noexcept
#if ROCKS_WITH_EDITOR_DATA
        : exportName(allocator)
#endif
    {
        (void)allocator;
    }

    ROCKS_PROPERTY() Name name;
    // 부모 본 번호다. 루트만 NoParentBone이고 나머지는 자기 번호보다 작다.
    ROCKS_PROPERTY() int32 parentIndex = NoParentBone;
    // 대소문자를 보존한 원래 이름이다(UE ExportName). 검사하지 않는다(6.4절).
#if ROCKS_WITH_EDITOR_DATA
    ROCKS_PROPERTY(EditorOnly) String exportName;
#endif
};

// 원본 본 계층이다(가상 본은 담지 않는다 — 스켈레톤이 따로 갖는다). bones와 referencePose는 같은
// 길이이고 pose는 부모 기준 local(UE FTransform, double)이다.
//
// 불변식(ValidateBoneHierarchy): 본 수 ≤ MeshMaxBones, 루트는 하나이고 번호 0, 본 i ≥ 1의 부모는
// [0, i), 이름은 None이 아니고 Name 비교 규칙(ASCII 대소문자 무시)으로 유일, pose 성분은 유한하고
// 회전 |길이² − 1| ≤ BoneRotationNormalizedTolerance. 빈 계층(본 0개)도 정상이다.
//
// 사용 예:
//   BoneHierarchy bones(allocator);
//   BoneInfo root(allocator);
//   root.name = names.TryIntern("root").Value();
//   bones.bones.TryPushBack(Move(root));
//   bones.referencePose.TryPushBack(Transform());
//   SkeletonDiagnostic failure;
//   if (!ValidateBoneHierarchy(bones, allocator, &failure)) { ... }
//
// 수명: allocator는 이 객체보다 오래 살아야 한다. 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct BoneHierarchy {
    ROCKS_GENERATED_BODY()

    explicit BoneHierarchy(Allocator& allocator) noexcept
        : bones(allocator), referencePose(allocator) {
    }

    ROCKS_PROPERTY() Array<BoneInfo> bones;
    ROCKS_PROPERTY() Array<Transform> referencePose;
};

// hierarchy가 불변식을 지키는지 본다(첫 위반에서 멈춘다 — 본 수, pose 길이, 본 번호 순으로 부모·
// 이름·pose, 마지막에 이름 중복(같은 이름의 두 번째 본 번호가 가장 작은 쌍)). scratch는 이름 정렬
// 표의 할당기다.
//
// 오류: 위반은 failure->code InvalidSkeletonData, ErrorCode InvalidArgument(본 수만
// CapacityExceeded) 이고 issue·index(본 번호)·related(중복이면 앞 본)가 위치다. role은
// Skeleton이다. 할당 실패는 진단 코드 없이 그 오류다. 스레드: hierarchy를 읽기만 한다.
ROCKS_ANIMATION_API Result<void>
ValidateBoneHierarchy(const BoneHierarchy& hierarchy, Allocator& scratch,
                      SkeletonDiagnostic* failure = nullptr) noexcept;

// 기준 pose 회전을 단위 quaternion으로 정규화한다(6.1절 "저장 전에 정규화" — UE FQuat::Normalize:
// 길이²가 BoneRotationMinimumLengthSquared 미만이면 단위 회전). 비유한 회전은 그대로 둔다(검증이
// 거부한다). 바꾼 회전 수를 돌려준다(이미 길이² = 1이면 바꾸지 않는다).
ROCKS_ANIMATION_API uint32 NormalizeBoneReferencePose(BoneHierarchy& hierarchy) noexcept;

// name인 본의 번호다(Name 비교 규칙, 앞에서부터 첫 일치). 없으면 NoParentBone이다.
ROCKS_ANIMATION_API int32 FindBoneIndex(const BoneHierarchy& hierarchy, Name name) noexcept;

} // namespace rocks
