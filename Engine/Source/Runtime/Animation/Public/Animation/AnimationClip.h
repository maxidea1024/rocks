#pragma once

#include <Animation/Skeleton.h>
#include <AssetRuntime/BulkData.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Time/FrameRate.h>
#include <Curves/TangentCurve.h>
#include <Object/ObjectRefs.h>

// 애니메이션 클립 에셋이다(Docs/Design/MeshAnimationAssets.ko.md 7절·7.5절, UE UAnimSequenceBase·
// UAnimSequence·UAnimDataModel 대응).
namespace rocks {

// 원본 frameCount의 상한이다(7.2절 — 쿠킹 할당(표본 수 × 트랙 수)의 넘침을 막는다).
inline constexpr uint32 ClipMaxFrames = 1u << 20;

// 원본·목표 frame rate의 상한(초당 프레임)이다(7.2절).
inline constexpr double ClipMaxFrameRate = 1000.0;

// 변환 커브 하나의 채널 수다(이동 x·y·z, 회전 x·y·z, 크기 x·y·z — 7.5절 5).
inline constexpr uint32 ClipTransformCurveChannels = 9;

// 키 사이 보간 방식이다(UE EAnimInterpolationType).
ROCKS_ENUM()
enum class ClipInterpolation : uint8 {
    // 이동·크기는 선형, 회전은 최단 경로 nlerp(7.5절 8).
    Linear = 0,
    // 앞 키 값을 그대로 쓴다.
    Step = 1
};

// root motion의 루트 고정 방식이다(UE ERootMotionRootLock).
ROCKS_ENUM()
enum class RootMotionRootLock : uint8 {
    RefPose = 0,
    FirstFrame = 1,
    Zero = 2
};

// additive 클립 종류다(UE EAdditiveAnimationType).
ROCKS_ENUM()
enum class AdditiveType : uint8 {
    None = 0,
    LocalSpace = 1,
    MeshSpaceRotation = 2
};

// additive 기준 pose다(UE EAdditiveBasePoseType).
ROCKS_ENUM()
enum class AdditiveBasePose : uint8 {
    None = 0,
    RefPose = 1,
    Scaled = 2,
    Frame = 3,
    LocalFrame = 4
};

// 클립 이벤트 하나다(UE FAnimNotifyEvent — 7.3절). duration > 0이면 구간 이벤트다. 시간은 클립 시작
// 기준 초다. type은 이벤트 종류 이름(처리기 연결은 게임플레이 단계)이다.
ROCKS_STRUCT()
struct ClipEvent {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float time = 0.0f;
    ROCKS_PROPERTY() float duration = 0.0f;
    // 이벤트 트랙 번호다(0 이상 — AnimationClip::eventTracks가 있으면 그 범위).
    ROCKS_PROPERTY() int32 track = 0;
    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() Name type;
    // 발생 확률이다([0, 1], UE NotifyTriggerChance 기본 1).
    ROCKS_PROPERTY() float triggerChance = 1.0f;
    ROCKS_PROPERTY() bool triggerOnDedicatedServer = true;
    ROCKS_PROPERTY() bool triggerOnFollower = false;
};

// 동기 마커 하나다(UE FAnimSyncMarker).
ROCKS_STRUCT()
struct ClipSyncMarker {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() float time = 0.0f;
    ROCKS_PROPERTY() int32 track = 0;
};

// root motion 설정이다(UE UAnimSequence의 bEnableRootMotion·RootMotionRootLock·bForceRootLock·
// bUseNormalizedRootMotionScale).
ROCKS_STRUCT()
struct RootMotionSettings {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool enable = false;
    ROCKS_PROPERTY() RootMotionRootLock rootLock = RootMotionRootLock::RefPose;
    ROCKS_PROPERTY() bool forceRootLock = false;
    ROCKS_PROPERTY() bool useNormalizedScale = true;
};

class AnimationClip;

// additive 설정이다(UE AdditiveAnimType·RefPoseType·RefPoseSeq·RefFrameIndex). basePoseClip은 Hard
// 참조이고 비어도 된다(7.1절 — 쿠킹 v1은 런타임에 기준 클립을 함께 평가한다).
ROCKS_STRUCT()
struct AdditiveSettings {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() AdditiveType type = AdditiveType::None;
    ROCKS_PROPERTY() AdditiveBasePose basePose = AdditiveBasePose::None;
    ROCKS_PROPERTY() ObjectRef<AnimationClip> basePoseClip;
    // 기준 pose 프레임이다(Frame·LocalFrame — 0 이상).
    ROCKS_PROPERTY() int32 basePoseFrame = 0;
};

// 플랫폼별 frame rate다(UE FPerPlatformFrameRate — 7.5절 4). overrides는 TargetProfile 이름별
// 덮어쓰기 표다.
//
// 사용 예:
//   const FrameRate rate = clip.targetFrameRate.Resolve(platformName);
ROCKS_STRUCT()
struct ROCKS_ANIMATION_API PlatformFrameRate {
    ROCKS_GENERATED_BODY()

    explicit PlatformFrameRate(Allocator& allocator) noexcept : overrides(allocator) {
    }

    // 기본값은 UE UAnimationSettings::DefaultFrameRate와 같은 30/1이다.
    ROCKS_PROPERTY() FrameRate defaultRate = FrameRate::TryCreate(30, 1).Value();
    ROCKS_PROPERTY() OrderedMap<Name, FrameRate> overrides;

    // platform의 값이다(덮어쓰기 표에 없거나 None이면 기본값).
    FrameRate Resolve(Name platform) const noexcept;
};

// 본 트랙 하나의 저장 형태다(7.2절, 7.5절 6). 성분마다 bulk 하나(little-endian float — 이동·크기
// Vec3, 회전 Quat x·y·z·w)이고 원소 수는 1 또는 키 수다.
ROCKS_STRUCT()
struct ClipBoneTrack {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name bone;
    ROCKS_PROPERTY() BulkDataRef translation;
    ROCKS_PROPERTY() BulkDataRef rotation;
    ROCKS_PROPERTY() BulkDataRef scale;
};

// 클립 float 커브 하나다(7.2절, UE FFloatCurve). 세 표시는 UE AACF_Editable·AACF_Metadata·
// AACF_Disabled다(7.5절 5).
ROCKS_STRUCT()
struct ClipCurve {
    ROCKS_GENERATED_BODY()

    explicit ClipCurve(Allocator& allocator) noexcept : curve(allocator) {
    }

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() bool editable = true;
    ROCKS_PROPERTY() bool metadata = false;
    ROCKS_PROPERTY() bool disabled = false;
    ROCKS_PROPERTY() TangentCurve curve;
};

// 클립 변환 커브 하나다(UE FTransformCurve). channels는 정확히 ClipTransformCurveChannels개다(이동
// x·y·z, 회전 x·y·z, 크기 x·y·z — 7.5절 5).
ROCKS_STRUCT()
struct ClipTransformCurve {
    ROCKS_GENERATED_BODY()

    explicit ClipTransformCurve(Allocator& allocator) noexcept : channels(allocator) {
    }

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() bool editable = true;
    ROCKS_PROPERTY() bool metadata = false;
    ROCKS_PROPERTY() bool disabled = false;
    ROCKS_PROPERTY() Array<TangentCurve> channels;
};

// 클립 원본 데이터의 저장 형태다(7.2절). 본 트랙은 본 이름 순(7.5절 6)이다. 메모리 형태는
// ClipSource (Animation/ClipSource.h)이고 LoadClipSource·StoreClipSource가 본 트랙 bulk를 오간다.
ROCKS_STRUCT()
struct ClipSourceData {
    ROCKS_GENERATED_BODY()

    explicit ClipSourceData(Allocator& allocator) noexcept
        : boneTracks(allocator), curves(allocator), transformCurves(allocator) {
    }

    ROCKS_PROPERTY() FrameRate frameRate = FrameRate::TryCreate(30, 1).Value();
    // 프레임 수다(키 수 = frameCount + 1 — UE NumberOfKeys = NumberOfFrames + 1).
    ROCKS_PROPERTY() uint32 frameCount = 0;
    ROCKS_PROPERTY() Array<ClipBoneTrack> boneTracks;
    ROCKS_PROPERTY() Array<ClipCurve> curves;
    ROCKS_PROPERTY() Array<ClipTransformCurve> transformCurves;
};

// 쿠킹 재생 데이터 v1이다(7.4절, 7.5절 10). tracks bulk에 트랙마다 원소 수 uint32 셋(이동·회전·
// 크기 — 각 1 또는 sampleCount)과 값을 잇는다. 메모리 형태는 ClipPlayback이다.
ROCKS_STRUCT()
struct ClipPlaybackData {
    ROCKS_GENERATED_BODY()

    explicit ClipPlaybackData(Allocator& allocator) noexcept
        : trackToBone(allocator), curves(allocator) {
    }

    ROCKS_PROPERTY() FrameRate sampleRate = FrameRate::TryCreate(30, 1).Value();
    // 표본(키) 수다(프레임 수 + 1). 0이면 쿠킹 데이터가 없다(에디터 패키지).
    ROCKS_PROPERTY() uint32 sampleCount = 0;
    // 쿠킹 트랙 → 스켈레톤 본 번호다.
    ROCKS_PROPERTY() Array<uint16> trackToBone;
    ROCKS_PROPERTY() BulkDataRef tracks;
    ROCKS_PROPERTY() Array<ClipCurve> curves;
};

// 스켈레톤에 묶인 애니메이션 에셋의 추상 기반이다(UE UAnimationAsset). skeleton은 Hard 참조다(Build
// 의존 선언은 MA6b — 7.5절 2).
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS(Abstract)
class AnimationAsset : public Asset {
    ROCKS_GENERATED_BODY()

public:
    ROCKS_PROPERTY() ObjectRef<Skeleton> skeleton;
};

// 애니메이션 클립이다(UE UAnimSequence — 7.1절). 원본(source)·이벤트 트랙 이름은 EditorOnly이고,
// 이벤트·동기 마커·재생 설정은 쿠킹에도 남는다. playback은 쿠킹 데이터다(에디터 패키지에서는 비어
// 있다).
//
// 사용 예:
//   AnimationClip* clip = ...;   // Registry가 만든 객체
//   clip->interpolation = ClipInterpolation::Linear;
//   clip->targetFrameRate.defaultRate = FrameRate::TryCreate(60, 1).Value();
//   SortClipEvents(*clip);                     // 이벤트 정렬(7.5절 11)
//   ValidateAnimationClip(*clip, scratch, &failure);
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class AnimationClip final : public AnimationAsset {
    ROCKS_GENERATED_BODY()

public:
    explicit AnimationClip(Allocator& allocator) noexcept
        : source(allocator), eventTracks(allocator), events(allocator), syncMarkers(allocator),
          targetFrameRate(allocator), playback(allocator) {
    }

    ROCKS_PROPERTY(EditorOnly) ClipSourceData source;
    // 이벤트 트랙 이름이다(UE AnimNotifyTracks).
    ROCKS_PROPERTY(EditorOnly) Array<Name> eventTracks;
    ROCKS_PROPERTY() Array<ClipEvent> events;
    ROCKS_PROPERTY() Array<ClipSyncMarker> syncMarkers;
    ROCKS_PROPERTY() ClipInterpolation interpolation = ClipInterpolation::Linear;
    ROCKS_PROPERTY() float rateScale = 1.0f;
    ROCKS_PROPERTY() bool loop = false;
    ROCKS_PROPERTY() RootMotionSettings rootMotion;
    ROCKS_PROPERTY() AdditiveSettings additive;
    // 쿠킹 샘플링 비율이다(플랫폼별).
    ROCKS_PROPERTY() PlatformFrameRate targetFrameRate;
    ROCKS_PROPERTY() Name retargetSource;
    ROCKS_PROPERTY() ClipPlaybackData playback;
};

} // namespace rocks
