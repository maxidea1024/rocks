## 7. 애니메이션 클립

### 7.1 클래스

```text
AnimationAsset : Asset (추상)
  skeleton : ObjectRef<Skeleton>     // Hard + Build(쿠킹 트랙 → 본 번호가 스켈레톤에 의존 — 너겟 키에 스켈레톤 build key가 들어간다)

AnimationClip : AnimationAsset
  EditorOnly:
    source           : ClipSourceData
    eventTracks      : Array<Name>                                   // 이벤트 트랙 이름(UE AnimNotifyTracks)
  저장:
    events           : Array<ClipEvent { time float, duration float, track int32, name Name, type Name,
                                         triggerChance float, triggerOnDedicatedServer bool, triggerOnFollower bool }>
    syncMarkers      : Array<ClipSyncMarker { name Name, time float, track int32 }>
    interpolation    : ClipInterpolation { Linear, Step }
    rateScale        : float
    loop             : bool
    rootMotion       : RootMotionSettings { enable bool, rootLock { RefPose, FirstFrame, Zero }, forceRootLock bool, useNormalizedScale bool }
    additive         : AdditiveSettings { type { None, LocalSpace, MeshSpaceRotation },
                                          basePose { None, RefPose, Scaled, Frame, LocalFrame },
                                          basePoseClip ObjectRef<AnimationClip>(Hard, 비어도 됨), basePoseFrame int32 }
    targetFrameRate  : FrameRate(플랫폼별)                            // 쿠킹 샘플링 비율
    retargetSource   : Name
  쿠킹 데이터:
    playback         : ClipPlaybackData
```

UE 대응: AnimSequence.h:285-336(additive·root motion·interpolation), AnimSequenceBase.h:61-68(RateScale·bLoop), AnimSequence.h:731-735(PlatformTargetFrameRate). notify·sync marker 필드는 AnimTypes.h:276-356, 479-506이다.

`basePoseClip`이 Hard인 이유: 쿠킹 v1은 additive 차이를 미리 계산하지 않고 런타임에 기준 클립을 함께 평가한다. 쿠킹에서 미리 계산하게 되면 Build + EditorOnly로 바꾼다.

### 7.2 원본 데이터 `ClipSourceData`

```text
ClipSourceData {
  frameRate   : FrameRate                // 유리수(Core FrameRate). UE FFrameRate Numerator/Denominator
  frameCount  : uint32                   // 키 수 = frameCount + 1(UE NumberOfKeys = NumberOfFrames + 1)
  boneTracks  : Array<BoneTrack { bone Name,
                                  translation BulkDataRef(Vec3 float, 1 또는 키 수),
                                  rotation    BulkDataRef(Quat float, 1 또는 키 수),
                                  scale       BulkDataRef(Vec3 float, 1 또는 키 수) }>   // bone 이름 순 정렬
  curves      : Array<ClipCurve { name Name, flags ClipCurveFlags, curve TangentCurve }>
  transformCurves : Array<ClipTransformCurve { name Name, flags, translation TangentCurve[3], rotation TangentCurve[3], scale TangentCurve[3] }>
}
```

- **키는 프레임 균일 간격이다.** UE 5.8 데이터 모델도 tick resolution과 표시 비율을 같게 두고 프레임 0..키 수−1에서 본 키를 평가한다(Engine/Plugins/Animation/AnimationData/Source/AnimationData/Private/AnimSequencerController.cpp:320-321, AnimSequencerDataModel.cpp:1280-1293).
- **상한.** `frameCount` ≤ `ClipMaxFrames`(= 2^20), 원본·목표 frame rate는 0보다 크고 `ClipMaxFrameRate`(= 1000 fps) 이하, 본 트랙 수 ≤ `MeshMaxBones`. 넘으면 `InvalidClipData` 오류다. 쿠킹 할당(샘플 수 × 트랙 수)의 넘침을 막는다.
- **회전 원본에 관한 한계.** UE 5.8은 본 키를 FK 채널(이동·회전·크기 각 3개 float 채널)에 저장하고 회전은 Euler 각이다. 읽을 때 quaternion으로 바꾼다(AnimSequencerDataModel.cpp:1260-1293). 그래서 Rocks가 받는 원본은 "프레임별로 평가한 Transform"이다. float로 저장하는 이유는 UE의 원본 키 형식(`FRawAnimSequenceTrack`, AnimTypes.h:852-866)과 런타임 평가가 float이기 때문이다. quaternion의 연속 키 사이 부호 반전은 원본 그대로 둔다. 보간 때 최단 경로를 고르는 것은 평가 쪽 규칙이다.
- **상수 트랙.** 성분 하나의 모든 키가 비트 단위로 같으면 키 1개로 줄인다(손실 없음).
- 트랙은 본 이름으로 스켈레톤과 맞춘다. 가상 본 이름도 받는다(UE는 전체 계층 기준으로 검증한다 — AnimSequencerController.cpp:1496-1502). 스켈레톤에 없는 본의 트랙은 임포트 때 `UnknownBoneTrack` 경고를 내고 보존한다. 빌드는 무시한다.
- 커브는 Curves 모듈 `TangentCurve`(UE `FRichCurve` 대응, [커브 데이터](../CurveData.ko.md))를 그대로 쓴다. 키 시간은 float 초다(UE도 시퀀서 채널에서 `FRichCurve`로 줄 때 float 초로 바꾼다 — AnimSequencerHelpers.cpp:13-28). `ClipCurveFlags`는 UE `AACF_*`(AnimCurveTypes.h:41-57) 가운데 `Editable`·`Metadata`·`Disabled`를 둔다.
- animated bone attribute(UE `FAnimationAttributeIdentifier`)는 후속이다. 임포트 때 개수만 경고한다.

### 7.3 이벤트

- 이벤트 `type`은 이벤트 종류의 이름이다(UE에서는 `UAnimNotify`·`UAnimNotifyState` 클래스 경로). 게임플레이 단계에서 이 이름을 처리기에 연결한다. 등록되지 않은 종류는 보존하고 무시한다.
- 종류별 속성(UE notify 객체의 UPROPERTY)은 v1에서 받지 않는다. 임포터가 버리고 `EventPropertiesDropped` 경고(개수)를 낸다. 게임플레이 단계에서 `InstancedStruct` 필드로 더한다.
- `duration > 0`이면 구간 이벤트(UE NotifyState)다.
- 시간은 클립 시작 기준 초다. UE의 link 방식(`LinkMethod`·`SegmentBeginTime` — AnimLinkableElement.h:142-165)은 montage용이라 클립에서는 계산한 절대 시간만 저장한다.

### 7.4 쿠킹 데이터 v1

```text
ClipPlaybackData {
  sampleRate    : FrameRate        // targetFrameRate 플랫폼 값. 원본 비율과 다르면 원본을 보간해 다시 샘플링한다
  sampleCount   : uint32
  trackToBone   : Array<uint16>    // 쿠킹 트랙 → 스켈레톤 본 번호(스켈레톤에 없는 트랙은 빠진다)
  tracks        : BulkDataRef      // 트랙별 성분 3개, 각 성분은 상수(1키) 또는 sampleCount 키, float
  curves        : 원본 그대로(TangentCurve)
}
```

- 재샘플링의 회전 보간(slerp)은 Core 결정적 수학으로 한다(AssetSystem 10절 쿠킹 결정성).
- 압축(ACL)은 후속이고, 압축이 생기면 이 구조의 `tracks`가 코덱 표시와 압축 바이트로 바뀐다. 오차 목표는 계획 15절(끝 효과기 1 cm, 회전 0.5°)이다.

### 7.5 구현에서 정한 점(MA6a)

MA6a 구현(`Animation` 모듈의 클립 클래스·원본·쿠킹 데이터, CoreTypeBindings `FrameRate` 미러) 중 7.1–7.4절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다. UE 근거는 E/Private/Animation/AnimDataModel.cpp(`Evaluate`), AnimationRuntime.cpp(`GetKeyIndicesFromTime`), AnimSequence.cpp(`CalculateNumberOfSampledKeys`·`SortSyncMarkers`), RT/Core/Public/Math/TransformNonVectorized.h(`Blend`)·Quat.h(`FastLerp`), E/Public/Animation/AnimTypes.h(`FAnimNotifyEvent::operator<`)다.

1. **파일과 의존.** 리플렉션 헤더 `Animation/AnimationClip.h`(열거·값 struct·`ClipSourceData`·`ClipPlaybackData`·`AnimationAsset`·`AnimationClip`)와 메모리 형태·순수 함수 헤더 `Animation/ClipSource.h`(`ClipSource`·`ClipPlayback`, 검증, Load·Store, 샘플링, 쿠킹 변환)를 둔다. `Animation`은 `Curves`(`TangentCurve`)에 public으로 의존한다(8절). 진단은 `AnimationDiagnostic.h`의 `ClipIssue`·`ClipDiagnostic{code, error, issue, index, related}`이다.
2. **`AnimationAsset`·Build 의존.** `AnimationAsset`은 `ROCKS_CLASS(Abstract)`이고 `skeleton`은 `ObjectRef<Skeleton>`(Hard)이다. 7.1절의 Build 의존은 필드 플래그가 아니라 `DependencyDeclarer`(AssetSystem 16.2절 — A2 이후)가 선언하므로 쿠킹 처리기와 함께 MA6b에서 더한다.
3. **`FrameRate` 미러(MR8).** CoreTypeBindings에 `ROCKS_STRUCT(Binds = "rocks::FrameRate", Atomic, Version = 1)`을 두고 private 멤버(`numerator_`·`denominator_`, 도구 이름 `numerator`·`denominator`)를 InterpCurve 미러와 같은 방식(`GeneratedType` friend)으로 리플렉션한다. 검증 hook이 두 값이 양수인지 본다(0·음수 값은 게시하지 않는다 — `TryCreate`와 같은 불변식).
4. **플랫폼별 목표 비율.** `targetFrameRate`는 `PlatformFrameRate{defaultRate, overrides OrderedMap<Name, FrameRate>}`(UE `FPerPlatformFrameRate`)이고 `Resolve(platform)`이 덮어쓰기 표에 없으면 기본값을 준다. 기본값은 UE `UAnimationSettings::DefaultFrameRate`와 같은 30/1이다. 공용 플랫폼별 값 타입이 생기면 그것으로 옮긴다.
5. **커브 표시.** `ClipCurveFlags`는 bool 필드 셋(`editable` 기본 참 — UE `AACF_DefaultCurve`, `metadata`, `disabled`)으로 둔다(UE bit 값을 저장 형식으로 끌고 오지 않는다). `ClipTransformCurve`의 TangentCurve 9개는 `Array<TangentCurve> channels`(이동 x·y·z, 회전 x·y·z, 크기 x·y·z 순, 길이 정확히 `ClipTransformCurveChannels` = 9)다 — 할당기를 받는 struct의 정적 배열 필드는 리플렉션이 받지 않는다.
6. **원본 bulk 배치.** 성분마다 bulk 하나이고 little-endian float, 패딩 없음이다(이동·크기 Vec3 12 byte, 회전 Quat x·y·z·w 16 byte). 원소 수는 1 또는 키 수(`frameCount + 1`)이고 0개·나머지 byte·다른 개수는 손상(로드 `CorruptPackage`)이다. 메모리 형태 `ClipSource`는 트랙마다 성분 배열을 갖고, `Store`가 상수 성분(모든 키 bit 같음)을 1키로 접고 트랙을 본 이름 원문 순으로 정렬해 쓴다. 순서는 원문을 ASCII 소문자로 접은 byte 비교다(9.2절의 NFC UTF-8 byte 순에 Name 비교 규칙을 겹쳐, 대소문자만 다른 두 이름이 같은 자리가 되어 중복이 한 규칙으로 드러난다). `Load`는 엄격 증가가 아니면(정렬 위반·중복) 손상으로 본다. 쿠킹 데이터와 같이 원본 커브는 `ClipSourceData`에 리플렉션 값으로 그대로 있고 `Store`·`Load`는 본 트랙만 다룬다.
7. **원본 검증(`ValidateClipSource`).** 차례로 비율 ≤ `ClipMaxFrameRate`(1000 fps — `FrameRate`는 이미 양수), `frameCount` ≤ `ClipMaxFrames`, 트랙 수 ≤ `MeshMaxBones`, 트랙마다(이름 None 아님, 성분 원소 수 1 또는 키 수, 값 유한, 회전 |길이² − 1| ≤ `BoneRotationNormalizedTolerance`), 이름 중복(Name 비교 규칙 — 두 번째 트랙 번호가 가장 작은 쌍)이다. 순서는 `Store`가 정하므로 보지 않는다(pool이 필요 없다). 위반은 `InvalidClipData`, 할당 실패는 그 오류다. 커브 키(시간 순·유한)는 `TangentCurve` API와 리플렉션 검증 hook이 이미 지키므로 다시 보지 않는다.
8. **샘플링 규칙(UE `UAnimDataModel::Evaluate`).** 시간 t(초, double)가 0 이하이거나 키가 하나면 키 0, `FrameRate::AsFrameTime(t)`가 마지막 키 이상이면 마지막 키, 아니면 키 = 프레임, α = subframe이다. Step이면 α = 0이고, α < `ClipKeySnapThreshold`(1e-4, UE `KINDA_SMALL_NUMBER`)면 앞 키, α > 1 − 그 값이면 다음 키, 그 밖은 UE `FTransform::Blend`처럼 이동·크기는 선형, 회전은 `FastLerp`(내적 부호로 최단 경로를 고른 nlerp) 뒤 정규화(제곱합 < 1e-8이면 단위)다. 성분이 1키면 그 값이다. **7.4절의 slerp 대신 UE와 같은 nlerp를 쓴다** — UE 평가·재샘플링이 nlerp이고 초월 함수가 없어 결정성도 쉽다.
9. **쿠킹 변환(`BuildClipPlayback`).** 목표 비율이 원본과 같으면(`FrameRate` ==) 키를 그대로 옮긴다. 다르면 표본 수 N = frameCount × 목표/원본이 정수여야 한다(정수 연산으로 확인 — 아니면 `InvalidClipData`·`IncompatibleTargetFrameRate`. UE는 `ensure`만 하고 내림해 끝 프레임을 잃는다), N ≤ `ClipMaxFrames`이다. 키 k는 목표 비율의 k 프레임 시간(초)에서 8의 규칙(클립 보간 방식)으로 원본을 평가한 값이다(UE도 시간으로 평가한다). 그다음 성분마다 상수 접기를 한다. 트랙은 원본 순서대로 스켈레톤 원본 본 이름(`FindBoneIndex`)과 맞추고 없는 트랙은 뺀다. 원본 본도 가상 본도 아닌 이름은 `UnknownBoneTrack` 경고(index = 원본 트랙), 가상 본 트랙은 경고 없이 뺀다(가상 본은 평가가 계산한다). 커브는 그대로 복사하고, 변환 커브는 쿠킹 v1에 들어가지 않는다(UE는 본 트랙에 bake한다 — bake는 후속) — 사용 중(disabled 아님)인 수를 결과 `skippedTransformCurves`로 돌려준다. 전체를 `RunWithDeterministicFloatingPoint` 안에서 한다.
10. **쿠킹 bulk 배치.** `tracks` bulk 하나에 트랙마다 원소 수 `uint32` 셋(이동·회전·크기, 각 1 또는 `sampleCount`)과 그 값들을 잇는다(6과 같은 원소 배치). `trackToBone` 길이 = 트랙 수, `sampleCount` = 표본 수(키 수)다. 로드는 배치·길이·값 유한을 보고 어기면 `CorruptPackage`다. 쿠킹 데이터 샘플링은 8과 같은 규칙에 `sampleRate`·`sampleCount`를 쓴다. 로드 머리 판정의 진단은 규칙마다 다르다: `sampleCount` 0은 `NoSamples`, 프레임 상한 초과는 `TooManyFrames`, 비율 상한 초과는 `FrameRateTooHigh`, `trackToBone` 길이 불일치는 `TrackBoneLength`다(리뷰 반영 개정 13 — 처음엔 모두 `TrackBoneLength`였다). `trackToBone`의 본 번호와 회전 정규화는 스켈레톤을 알아야 하므로 이 로드는 보지 않는다 — 쿠킹 로드(MA6b)가 스켈레톤과 대조하기 전에는 믿을 수 없다(헤더 계약에 적었다).
11. **클립 검증(`ValidateAnimationClip`).** 에디터 데이터가 있는 클립을 본다(원본 머리·커브 이름(None·중복)·변환 커브 채널 수 + 설정 + 이벤트). 길이 = frameCount / 원본 비율(UE `GetPlayLength`). 이벤트: 시간 유한·[0, 길이], duration 유한·0 이상·끝 ≤ 길이, `triggerChance` ∈ [0, 1], track ≥ 0이고 `eventTracks`가 있으면 그 범위, 정렬(시간 오름차순, 같으면 track 오름차순 — UE `operator<`의 근사 비교 대신 정확 비교). 동기 마커: 이름 None 아님, 시간 [0, 길이], track 규칙 같음, 시간 오름차순(UE `SortSyncMarkers`. UE는 범위 밖을 잘라 넣지만 Rocks는 거부한다). `rateScale` 유한, `basePoseFrame` ≥ 0, 목표 비율(기본·덮어쓰기)이 `ClipMaxFrameRate` 이하. 정렬 도우미 `SortClipEvents`·`SortClipSyncMarkers`(안정 정렬)를 둔다. 쿠킹 클립의 검증은 쿠킹 로드(MA6b)가 한다.
12. **진단 등록.** `AssetDiagnosticCode`에 `InvalidClipData`·`UnknownBoneTrack`·`EventPropertiesDropped`(임포터가 내는 경고 — MA6b·UX 임포터가 쓴다)를 더했다.
13. **결정성.** 정렬은 원문 byte·정확 비교로 입력만으로 정해지고, 부동소수점은 √(정확 반올림)와 사칙연산뿐이다. 스레드를 쓰지 않는다.
