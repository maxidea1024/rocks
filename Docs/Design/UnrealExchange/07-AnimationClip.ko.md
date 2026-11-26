## 7. 애니메이션 클립

- 데이터 모델(`GetDataModelInterface()`, E/Classes/Animation/AnimSequenceBase.h:243-291): `GetFrameRate()`, `GetNumberOfFrames()`, `GetNumberOfKeys()`(= 프레임 수 + 1).
- 본 트랙: `GetBoneTrackNames()` 순으로 `GetBoneTrackTransforms(Name, Out)`(IAnimationDataModel.h:189-193)이 준 키 수만큼의 `FTransform`(double)을 float로 바꿔 이동·회전·크기 blob 세 개로 쓴다. 성분의 모든 키가 비트 단위로 같으면 키 1개로 쓴다.
- 커브: `GetFloatCurves()`의 `FRichCurve` 키(`InterpMode`·`TangentMode`·`TangentWeightMode`·시간·값·접선·가중치, E/Classes/Curves/RichCurve.h:80-118)와 외삽·기본값, 플래그. Rocks `TangentCurve`가 같은 필드를 갖는다([커브 데이터](../CurveData.ko.md)). transform 커브는 9개 커브로 쓴다.
- 이벤트: `Notifies`의 `GetTriggerTime()`·`GetDuration()`, `TrackIndex`, `NotifyName`, notify 클래스 경로(`Notify` 또는 `NotifyStateClass`의 클래스), `NotifyTriggerChance`, 서버·follower 트리거 플래그. notify 객체의 프로퍼티도 `properties`로 덤프하지만 Rocks v1은 받지 않는다(버리고 경고 — MeshAnimationAssets 7.3절). `AnimNotifyTracks` 이름, `AuthoredSyncMarkers`.
- 설정: MeshAnimationAssets 7.1절 표의 UE 프로퍼티(additive·root motion·interpolation·RateScale·bLoop·PlatformTargetFrameRate·RetargetSource). `RefPoseSeq`는 Hard 의존으로 기록한다.
- animated bone attribute는 개수만 진단으로 남긴다.

### 7.1 구현에서 정한 점(UX8a)

UX8a(UE 쪽 애니메이션 클립 추출, `FRocksAnimSequenceExtractor`) 중 7절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다.

1. **`data` 스키마.** `{ "frameRate": { "numerator", "denominator" }, "frameCount", "keyCount", "boneTracks", "curves", "transformCurves", "events", "eventTracks", "syncMarkers" }`다. 본 트랙은 데이터 모델 트랙 순서(`GetBoneTrackNames`)의 `{ "bone", "translation", "rotation", "scale" }`이고 성분마다 blob 하나(`f32`, 이동·크기 components 3, 회전 4 — x·y·z·w, count = 키 수 또는 모든 키가 비트 단위로 같으면 1)다. 값은 `GetBoneTrackTransforms`의 double `FTransform`을 float로 바꾼 것이다(7절). 정렬·이름 대소문자 접기는 Rocks `StoreClipSource`가 한다(MeshAnimationAssets 7.5절 6).
2. **커브.** float 커브는 `{ "name", "flags", "curve" }`, transform 커브는 `{ "name", "flags", "channels": [커브 9개 — 이동 x·y·z, 회전 x·y·z, 크기 x·y·z] }`다. 커브 객체는 `{ "keys": [{ "interpMode", "tangentMode", "tangentWeightMode", "time", "value", "arriveTangent", "arriveTangentWeight", "leaveTangent", "leaveTangentWeight" }], "preInfinityExtrap", "postInfinityExtrap", "defaultValue" }`이고 열거는 UE 값 이름(`RCIM_Cubic`·`RCTM_Auto`·`RCTWM_WeightedNone`·`RCCE_Constant` 등), `flags`는 UE `EAnimAssetCurveFlags` 비트 그대로(`AACF_Editable` 0x04·`AACF_Metadata` 0x10·`AACF_Disabled` 0x40 — Rocks `editable`·`metadata`·`disabled`로 옮기는 것은 임포트), `defaultValue`는 UE 값 그대로다(UE "기본값 없음"은 float 최대값 — Rocks `Optional` 비움으로 옮기는 것은 임포트). `RCTM_SmartAuto`는 Rocks에 없어 임포트가 `Auto`로 본다(UX8b에서 확정).
3. **이벤트.** notify마다 `{ "time", "duration", "track", "name", "type", "triggerChance", "triggerOnDedicatedServer", "triggerOnFollower", "properties" }`다. `time`·`duration`은 `GetTriggerTime()`·`GetDuration()`(link 방식을 계산한 절대 값 — MeshAnimationAssets 7.3절), `type`은 notify 또는 notify state 객체의 클래스 경로(없으면 `null`), `properties`는 그 객체의 UPROPERTY 덤프(Rocks v1은 받지 않고 `EventPropertiesDropped` — 7절)다. 순서는 UE `Notifies` 순서이고 정렬은 Rocks(`SortClipEvents`)가 한다. `eventTracks`는 `AnimNotifyTracks` 이름, `syncMarkers`는 `AuthoredSyncMarkers`의 `{ "name", "time", "track" }`다.
4. **`settings`.** UAnimSequence UPROPERTY 덤프(additive·root motion·interpolation·`RateScale`·`bLoop`·`PlatformTargetFrameRate`·`RetargetSource`·`RefPoseSeq`·압축 설정 참조 등)다. 데이터 모델(`DataModelInterface`·`DataModel` — 시퀀서 채널을 담은 하위 객체)은 `data`에 풀어 쓰므로 덤프에서 뺀다. `RefPoseSeq`·스켈레톤 참조는 의존에 합쳐진다(압축 설정 에셋은 범위 밖 의존).
5. **진단.** animated bone attribute는 info `AnimatedBoneAttributes`(개수), 데이터 모델이 없으면 error `NoAnimationData`다.
6. **자기 시험.** `RocksExport.AnimationExtraction`(commandlet `-SelfTest`에도 포함)이 엔진 튜토리얼 걷기 클립을 내보내 비율·프레임·키 수, 본 트랙 이름·순서, 모든 트랙의 회전 blob이 데이터 모델 키의 float 변환과 bytes가 같은지, `settings`에 데이터 모델이 없는지, 추출 전후 덤프가 같은지 본다.
7. **Rocks 쪽 왕복 확인.** 선택 시험 `UnrealExchange_ExternalBundle`이 클립마다 본 트랙을 MA6a `ClipSource`로 적재해 `ValidateClipSource`, 키 시간 `SampleClipSource`가 키 값과 비트로 같은지, 커브를 `ClipCurve`(`TangentCurve`)로 옮기고 이벤트·이벤트 트랙·동기 마커를 `AnimationClip`에 담아 정렬 뒤 `ValidateAnimationClip`, 묶음 안 스켈레톤으로 원본 비율·두 배 비율 `BuildClipPlayback`과 쿠킹 샘플링(`SampleClipPlayback`)이 성공하는지 본다.
8. **수동 검증(2026-11-22).** 엔진 튜토리얼 클립 둘(대기 188프레임·걷기 55프레임, 본 68개), 시험 프로젝트의 걷기 사본(notify 트랙 둘, notify `AnimNotify_PlaySound` 하나, notify state `AnimNotifyState_TimedParticleEffect` 하나, 동기 마커 둘, float 커브 하나 — Python `AnimationLibrary`로 더함), glTF로 임포트한 기둥 클립(morph 커브 `Bulge`)을 닫힘과 함께 내보낸 묶음(에셋 6개)을 두 번 내보내 `createdUtc` 말고 bytes가 같고, 리더가 클립 4개 모두 위 7번을 통과했다(키 불일치 0, 모르는 본 트랙 0).

9. **키 수가 다른 본 트랙(개정 15).** 성분 blob의 `count`는 키 수 또는 1이다(7절). 데이터 모델이 키 0개 또는 클립 키 수와 다른 수를 돌려준 트랙은 그 규칙으로 담을 수 없으므로 빼고 warning `BoneTrackKeyMismatch`(본·키 수)를 단다.
