#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Base/NumericLimits.h>

// 스켈레톤·본 계층·클립 진단이다(Docs/Design/MeshAnimationAssets.ko.md 6.4절·7.5절·11절).
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

// 클립 진단이 가리키는 세부 규칙이다(7.5절). 값은 저장 형식이 아니다. 경고(UnknownBoneTrack)를 빼면
// 모두 InvalidClipData(로드 중이면 CorruptPackage) 아래의 규칙이다.
enum class ClipIssue : uint8 {
    None,
    // 원본 머리·본 트랙(ValidateClipSource — index는 트랙 번호).
    FrameRateTooHigh,      // 원본 frame rate > ClipMaxFrameRate
    TooManyFrames,         // frameCount > ClipMaxFrames
    TooManyTracks,         // 본 트랙 수 > MeshMaxBones
    InvalidTrackName,      // 트랙 본 이름이 None이거나 토큰 구조가 잘못됨
    TrackKeyCount,         // 성분 원소 수가 1도 키 수도 아님(related = 성분 0·1·2)
    NonFiniteKey,          // 키 성분이 비유한(related = 성분)
    RotationNotNormalized, // 회전 키 |길이² − 1| > BoneRotationNormalizedTolerance
    DuplicateTrack,        // 이름이 Name 비교 규칙으로 다른 트랙과 같음(related = 다른 트랙)
    // 로드 배치(LoadClipSource·LoadClipPlayback — CorruptPackage).
    TrackOrder,      // 저장된 트랙이 이름 순(7.5절 6)이 아님
    BulkUnreadable,  // bulk를 읽을 수 없음(빈 ref·없는 id·크기·SHA-256 불일치)
    BulkLength,      // bulk 길이가 배치와 다름
    TrackBoneLength, // 쿠킹 trackToBone 길이·sampleCount가 트랙 배치와 다름
    // 클립(ValidateAnimationClip — index는 그 항목 번호).
    InvalidCurveName,       // 커브 이름이 None
    DuplicateCurve,         // 커브 이름이 앞 커브와 같음(related = 앞 커브)
    TransformCurveChannels, // 변환 커브 채널 수 != ClipTransformCurveChannels
    EventTime,              // 이벤트 시간이 비유한이거나 [0, 길이] 밖
    EventDuration,          // duration이 비유한·음수이거나 끝이 길이를 넘음
    EventChance,            // triggerChance가 [0, 1] 밖
    EventTrack,             // track이 음수이거나 eventTracks 범위 밖
    EventOrder,             // 이벤트가 (시간, track) 순이 아님
    InvalidSyncMarker,      // 동기 마커 이름이 None
    SyncMarkerTime,         // 동기 마커 시간이 비유한이거나 [0, 길이] 밖
    SyncMarkerTrack,        // 동기 마커 track이 음수이거나 eventTracks 범위 밖
    SyncMarkerOrder,        // 동기 마커가 시간 순이 아님
    RateScale,              // rateScale이 비유한
    BasePoseFrame,          // additive basePoseFrame이 음수
    TargetFrameRateTooHigh, // 목표 frame rate(기본·덮어쓰기)가 ClipMaxFrameRate 초과(index =
                            // 덮어쓰기 번호, 기본이면 ClipNoIndex)
    // 쿠킹 변환(BuildClipPlayback).
    IncompatibleTargetFrameRate, // frameCount × 목표/원본이 정수가 아니거나 표본이 상한 초과
    UnknownBoneTrack             // 경고: 트랙 본이 스켈레톤 본·가상 본에 없음
};

// 클립 진단이 위치를 가리키지 않을 때의 번호다.
inline constexpr uint32 ClipNoIndex = NumericLimits<uint32>::Max();

// 세부 규칙 이름이다(예: EventOrder → "EventOrder"). 모르는 값은 "Unknown"이다. 정적 수명이다.
const char* ClipIssueName(ClipIssue issue) noexcept;

// 클립 진단 하나다. 실패한 호출이 out 인자로 채우고(성공하면 건드리지 않는다), 경고는 배열로
// 받는다.
struct ClipDiagnostic {
    // AssetSystem 4.6절 진단 이름이다(InvalidClipData·CorruptPackage·UnknownBoneTrack). 할당·pool
    // 오류는 None이다.
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    ErrorCode error = ErrorCode::None;
    ClipIssue issue = ClipIssue::None;
    // 처음 걸린 항목 번호(트랙·커브·이벤트·마커)다. 없으면 ClipNoIndex다.
    uint32 index = ClipNoIndex;
    // 관련 번호(성분·앞 항목)다. 없으면 ClipNoIndex다.
    uint32 related = ClipNoIndex;
};

// diagnostic을 AssetSystem 4.6절 형식으로 옮긴다: code·error는 그대로, asset은 인자, section은 0,
// detail은 index(ClipNoIndex면 0)다.
AssetDiagnostic ToAssetDiagnostic(const ClipDiagnostic& diagnostic, AssetId asset) noexcept;

} // namespace rocks
