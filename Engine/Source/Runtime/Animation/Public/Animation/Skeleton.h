#pragma once

#include <Animation/BoneHierarchy.h>
#include <AssetRuntime/Package.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Vector3.h>
#include <Core/Text/NamePool.h>
#include <Object/ModuleRegistration.h>
#include <Object/SoftObjectRef.h>

// 스켈레톤 에셋이다(Docs/Design/MeshAnimationAssets 6.2·6.4절, UE USkeleton).
namespace rocks {

// 본마다 이동 트랙을 리타깃하는 방식이다(UE EBoneTranslationRetargetingMode와 같은 순서·뜻). 번호는
// 저장 값이라 바꾸지 않는다.
ROCKS_ENUM()
enum class BoneTranslationRetarget : uint8 {
    // 애니메이션의 이동을 그대로 쓴다(UE 기본).
    Animation = 0,
    // 스켈레톤 기준 pose 이동을 쓴다.
    Skeleton = 1,
    // 애니메이션 이동을 스켈레톤 길이 비율로 늘인다.
    AnimationScaled = 2,
    // 애니메이션 이동의 기준 pose 대비 차이를 더한다.
    AnimationRelative = 3,
    // 방향과 길이를 맞춘다.
    OrientAndScale = 4
};

// BoneTranslationRetarget 값의 수다.
inline constexpr uint32 BoneTranslationRetargetCount = 5;

// 가상 본 이름의 접두사다(UE VirtualBoneNameHelpers::VirtualBonePrefix, ASCII 대소문자 무시).
inline constexpr StringView VirtualBonePrefix{"VB "};

// 원본 본 둘 사이에 두는 가상 본이다(UE FVirtualBone). 계층에 저장하지 않고 로드 때
// 덧붙인다(6.1절).
ROCKS_STRUCT()
struct VirtualBone {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name sourceBone;
    ROCKS_PROPERTY() Name targetBone;
    // "VB " 접두사로 시작한다.
    ROCKS_PROPERTY() Name name;
};

// 본에 붙은 소켓이다(UE USkeletalMeshSocket — Rocks는 값 구조체, 결정 MA-D6).
ROCKS_STRUCT()
struct BoneSocket {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() Name boneName;
    ROCKS_PROPERTY() Vec3d relativeLocation;
    ROCKS_PROPERTY() Rotator<double> relativeRotation;
    ROCKS_PROPERTY() Vec3d relativeScale = Vec3d(1.0, 1.0, 1.0);
    ROCKS_PROPERTY() bool forceAlwaysAnimated = true;
};

// 스켈레톤 커브 정보다(UE FCurveMetaData — 이름, 머티리얼·morph 표시, 연결 본, LOD 상한).
ROCKS_STRUCT()
struct SkeletonCurveInfo {
    ROCKS_GENERATED_BODY()

    explicit SkeletonCurveInfo(Allocator& allocator) noexcept : linkedBones(allocator) {
    }

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() bool isMaterial = false;
    ROCKS_PROPERTY() bool isMorphTarget = false;
    ROCKS_PROPERTY() Array<Name> linkedBones;
    ROCKS_PROPERTY() uint8 maxLod = NumericLimits<uint8>::Max();
};

// montage 슬롯 묶음이다(UE FAnimSlotGroup). 보존 필드다.
ROCKS_STRUCT()
struct AnimSlotGroup {
    ROCKS_GENERATED_BODY()

    explicit AnimSlotGroup(Allocator& allocator) noexcept : slots(allocator) {
    }

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() Array<Name> slots;
};

// 이름 있는 리타깃 기준 pose다(UE FReferencePose). 보존 필드다.
ROCKS_STRUCT()
struct NamedReferencePose {
    ROCKS_GENERATED_BODY()

    explicit NamedReferencePose(Allocator& allocator) noexcept : pose(allocator) {
    }

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() Array<Transform> pose;
};

// 스켈레톤 에셋이다(6.2절). 메시·클립이 본 이름으로 맞추는 공통 본 계층과 본별 리타깃 방식, 가상
// 본, 소켓, 커브 정보, 보존 필드(슬롯 묶음·리타깃 원본·호환 스켈레톤)를 담는다.
//
// 사용 예(검증):
//   SkeletonDiagnostic failure;
//   if (auto valid = ValidateSkeleton(*skeleton, names, allocator, &failure); !valid) {
//       Report(AssetDiagnosticName(failure.code), SkeletonIssueName(failure.issue), failure.index);
//   }
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class Skeleton final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit Skeleton(Allocator& allocator) noexcept
        : bones(allocator), retargetModes(allocator), virtualBones(allocator), sockets(allocator),
          curves(allocator), slotGroups(allocator), retargetSources(allocator),
          compatibleSkeletons(allocator) {
    }

    ROCKS_PROPERTY() BoneHierarchy bones;
    // bones와 같은 길이다(구현 — 애니메이션 평가가 쓴다).
    ROCKS_PROPERTY() Array<BoneTranslationRetarget> retargetModes;
    ROCKS_PROPERTY() Array<VirtualBone> virtualBones;
    ROCKS_PROPERTY() Array<BoneSocket> sockets;
    ROCKS_PROPERTY() Array<SkeletonCurveInfo> curves;
    // 보존 필드(montage·리타깃 단계).
    ROCKS_PROPERTY() Array<AnimSlotGroup> slotGroups;
    ROCKS_PROPERTY() Array<NamedReferencePose> retargetSources;
    ROCKS_PROPERTY() Array<SoftObjectRef<Skeleton>> compatibleSkeletons;
    ROCKS_PROPERTY() bool useRetargetModesFromCompatibleSkeleton = false;
};

// skeleton이 6.2·6.4절 불변식을 지키는지 본다. 차례: 본 계층(ValidateBoneHierarchy), retargetModes
// 길이·값, 가상 본(이름 접두사·유일·쌍 유일·원본과 대상이 본 또는 앞 가상 본), 소켓(이름 유일·본
// 또는 가상 본·유한 변환), 커브(이름 유일·연결 본), 슬롯 묶음(이름 유일, 슬롯은 모든 묶음에서
// 유일), 리타깃 원본(이름 유일·유한 pose). compatibleSkeletons는 보지 않는다.
//
// 인자: names는 skeleton의 Name을 발급한 pool이다(가상 본 접두사를 원문으로 본다). scratch는 이름
// 정렬 표의 할당기다.
// 오류: 위반은 failure->code InvalidSkeletonData, ErrorCode InvalidArgument(본 수만
// CapacityExceeded)다. 이름 원문을 읽지 못하면(stale 토큰 등) 그 pool 오류, 할당 실패는 그 오류이고
// 진단 코드는 None이다.
// 스레드: skeleton을 읽기만 한다.
ROCKS_ANIMATION_API Result<void> ValidateSkeleton(const Skeleton& skeleton, NamePool& names,
                                                  Allocator& scratch,
                                                  SkeletonDiagnostic* failure = nullptr) noexcept;

// Animation 리플렉션 타입(BoneInfo·BoneHierarchy·Skeleton 등)을 등록한다(생성 집계 함수
// generated::Animation::RegisterModuleTypes를 부른다 — 의존 모듈 타입도 함께). 도구·시험은 직접,
// 엔진은 애니메이션 에셋 모듈이 부른다.
ROCKS_ANIMATION_API Result<void> RegisterAnimationTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
