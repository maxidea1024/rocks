# 엔진 런타임 — 32. 타임라인 컴포넌트(CM5)

[목차](README.ko.md)

## 32. 타임라인 컴포넌트(CM5)

설계일 2026-11-28. [컴포넌트 도입](../ComponentCatalog/03-RequiredComponents.ko.md) CM5의 `TimelineComponent`(UE `UTimelineComponent` + `FTimeline`)다. 선행 CR10(커브 에셋 — CA1–CA7)은 구현됐고 CR7(이벤트)은 리플렉션 `Event` 필드로 해소됐다(카탈로그 개정 10). 렌더·물리 없이 **끝까지 동작**한다.

UE 근거: `RE/Classes/Components/TimelineComponent.h`(이하 `TH`), `RE/Private/Timeline.cpp`(이하 `TL`)다(UE 5.8.4).

### 32.1 모듈과 클래스

- 모듈은 `WorldIntegration`이다. 트랙이 커브 에셋(`FloatCurveAsset`·`VectorCurveAsset`·`ColorCurveAsset` — CurveAssets)을 `SoftObjectRef`로 가리키고 평탄 값(`VectorCurveValue`·`ColorCurveValue`)을 쓰므로 World 위 모듈이어야 한다(30.1절과 같은 이유). `WorldIntegration`의 공개 의존에 `CurveAssets`를 더한다.
- 파일: `Public/WorldIntegration/Components/TimelineComponent.h`와 트랙 값 struct마다 파일 하나(`TimelineFloatTrack.h`·`TimelineVectorTrack.h`·`TimelineColorTrack.h`·`TimelineEventKey.h`·`TimelineLengthMode.h`), `Private/Components/TimelineComponent.cpp`.
- `TimelineComponent : Component`다(UE `UActorComponent` — 위치가 없다).
- Tick: PrePhysics(UE `TG_PrePhysics` — `TL:778–784`). 시작 때 Tick은 꺼져 있다(UE `bStartWithTickEnabled = false`). `Play`·`PlayFromStart`·`Reverse`·`ReverseFromEnd`가 켜고(UE `Activate`), Tick에서 재생이 끝나 멈추면 끈다(UE `Deactivate`). 컴포넌트 활성(`SetEnabled`)은 건드리지 않는다 — UE `Activate`·`Deactivate`의 `bIsActive` 대응은 Tick 켜기·끄기만이다(32.6절 2).

### 32.2 데이터

| 필드 | 기본값(UE 생성자 — `TH:274–286`) | 설명 |
|---|---|---|
| `lengthMode` | `LastKeyFrame` | 길이를 정하는 방식(`TimelineLengthMode` — `TimelineLength`·`LastKeyFrame`, UE `ETimelineLengthMode`) |
| `length` | 5 | `TimelineLength` 모드의 길이(초, float) |
| `looping` | 거짓 | 끝에서 처음으로 돌아간다 |
| `reversePlayback` | 거짓 | 뒤로 재생한다 |
| `playing` | 거짓 | 재생 중이다(UE `PlayingStateTracker`의 첫 bit — 위 bit의 복제 dirty 수는 복제 단계 CR24 몫) |
| `playRate` | 1 | 재생 배율 |
| `position` | 0 | 현재 위치(초, float) |
| `ignoreTimeDilation` | 거짓 | 시간 배율(World 시간 배율·pause 배율)을 무시하고 unscaled delta를 쓴다(UE `bIgnoreTimeDilation`) |
| `floatTracks` | 빈 배열 | float 트랙(`TimelineFloatTrack`) |
| `vectorTracks` | 빈 배열 | 벡터 트랙(`TimelineVectorTrack`) |
| `colorTracks` | 빈 배열 | 색 트랙(`TimelineColorTrack`) |
| `eventKeys` | 빈 배열 | 이벤트 키(`TimelineEventKey`) |

트랙 struct(값 struct — `Array` 원소):
- `TimelineFloatTrack { Name trackName; TangentCurve curve; SoftObjectRef<FloatCurveAsset> curveAsset; }`
- `TimelineVectorTrack { Name trackName; VectorCurveValue curve; SoftObjectRef<VectorCurveAsset> curveAsset; }`
- `TimelineColorTrack { Name trackName; ColorCurveValue curve; SoftObjectRef<ColorCurveAsset> curveAsset; }`
- `TimelineEventKey { float time; Name eventName; }`(UE `FTimelineEventEntry {Time, EventFunc}` — 함수 대신 이름)

**커브 원천(ERD71).** UE 트랙은 커브 객체(`TObjectPtr<UCurveFloat>` 등 — Blueprint 타임라인은 클래스 안 커브 객체)를 가리킨다. Rocks World는 에셋 객체를 직접 잡지 못한다(ERD60). 그래서 트랙마다:
- `curveAsset`이 null이면 인라인 `curve`가 트랙 커브다(Blueprint 안 커브 대응 — 레벨·Prefab에 그대로 저장된다).
- `curveAsset`이 있으면 로드 시스템이 lease를 얻은 뒤 `ApplyLoadedFloatCurve(trackIndex, requested, curve)`(벡터·색도 같은 꼴)로 **로드 사본**을 넣는다(30.3절 `ApplyLoadedMeshInfo`와 같은 규칙 — 요청 참조가 지금 `curveAsset`과 다르면 `InvalidArgument`, 늦게 끝난 옛 요청이 새 커브를 덮지 않는다). 사본은 native(저장하지 않는다)이고, 로드 전·해제 뒤(`ClearLoadedCurve(kind, trackIndex)`)에는 그 트랙을 평가하지 않는다(UE의 null 커브 트랙과 같다 — `TL:285`).
- 로드 시스템 연결(등록·시작 준비에서 lease 요청 — CR4)은 에셋 lease를 World에 잇는 단계(30.4절 메시와 같은 자리)에서 붙인다. 지금은 위 함수와 시험만 둔다.

**이벤트(ERD72 — 리플렉션 `Event` 필드, 구독할 때만 상태 할당).** UE는 트랙마다 delegate(`InterpFunc`)와 이름 기반 속성 쓰기(`PropertySetObject`)를 둔다. `Event`는 struct·배열 원소에 둘 수 없으므로(EventReflection ED14) 컴포넌트 이벤트 하나에 트랙 이름을 인자로 준다:
- `floatTrackUpdated: Event<Name, float>`(트랙 이름, 값)
- `vectorTrackUpdated: Event<Name, Vec3d>`
- `colorTrackUpdated: Event<Name, LinearColor>`
- `eventKeyReached: Event<Name>`(UE `Events[i].EventFunc`)
- `updated: Event<>`(UE `TimelinePostUpdateFunc`)
- `finished: Event<>`(UE `TimelineFinishedFunc` — `Stop`으로 멈추면 오지 않는다)

`Params`는 순서대로 `trackName, value`·`eventName`이다. 모두 `Transient`가 아니다(레벨에 저장한 바인딩은 EventReflection ED19 그대로 저장된다).

### 32.3 동작(UE `FTimeline` — 연산 순서·float 그대로)

- `Play()`: `reversePlayback = false`, `playing = true`, Tick 켬(`TL:38–43`, UE 컴포넌트 `Activate`).
- `PlayFromStart()`: `SetPlaybackPosition(0, false)` 뒤 `Play()`.
- `Reverse()`: `reversePlayback = true`, `playing = true`, Tick 켬.
- `ReverseFromEnd()`: `SetPlaybackPosition(GetTimelineLength(), false)` 뒤 `Reverse()`.
- `Stop()`: `playing = false`(Tick은 다음 Tick에서 끈다 — UE도 `Deactivate`를 Tick에서 부른다).
- `IsPlaying()`, `IsReversing()` = `playing && reversePlayback`.
- `SetPlaybackPosition(newPosition, fireEvents, fireUpdate = true)`(`TL:274–500`):
  1. `old = position`, `position = newPosition`.
  2. 벡터 → float → 색 순서로, 커브가 있는 트랙(인라인 또는 로드 사본)을 `position`에서 평가해 그 이벤트를 발행한다(트랙 순서대로). UE는 delegate나 속성 이름이 있을 때만 평가한다 — Rocks는 해당 이벤트에 구독이 있을 때만 평가한다(`Event::IsBound`, 결과 동작 같음). 색은 조정 적용 값(UE `GetLinearColorValue`)이다.
  3. fireEvents면 이벤트 키를 본다. 앞으로: 구간 [old, position), position이 타임라인 길이와 정확히 같으면 끝을 `KindaSmallNumber`(1e-4 — UE `UE_KINDA_SMALL_NUMBER`) 넓힌다. 뒤로: (position, old], position이 0이면 시작을 1e-4 넓힌다. 키 시간이 구간 안이면 `eventKeyReached(eventName)`을 키 순서대로 발행한다.
  4. fireUpdate면 `updated()`.
- `TickTimeline(delta)`(`TL:516–599`): 재생 중이면 `effective = delta × (reversePlayback ? −playRate : playRate)`, `newPosition = position + effective`.
  - `effective > 0`이고 `newPosition > length`(지금 길이): loop면 `SetPlaybackPosition(length, true, false)` → `SetPlaybackPosition(0, false, false)` → 길이 > 0이면 `newPosition`에서 길이를 뺀다(32.6절 3 — 반복 상한), 아니면 0. loop가 아니면 `newPosition = length`, `Stop()`, 끝남 표시.
  - `effective ≤ 0`이고 `newPosition < 0`: loop면 `SetPlaybackPosition(0, true, false)` → `SetPlaybackPosition(length, false, false)` → 길이 > 0이면 더한다, 아니면 0. 아니면 0, `Stop()`, 끝남 표시.
  - `SetPlaybackPosition(newPosition, true)`. 끝남 표시면 `finished()`.
- 컴포넌트 `OnTick`(`TL:786–814`): delta는 `ignoreTimeDilation`이면 `TickContext::unscaledDeltaSeconds`, 아니면 `deltaSeconds`(float로). `TickTimeline` 뒤 재생 중이 아니면 Tick을 끈다(UE `Deactivate` — 복제 시뮬레이션 예외는 CR24).
- `SetNewTime(t)`: `t`를 [0, `length`]로 자른 뒤 `SetPlaybackPosition(t, false)`(UE는 `LengthMode`와 무관하게 `Length`로 자른다 — 그대로).
- `GetTimelineLength()`: `TimelineLength`면 `length`, `LastKeyFrame`면 마지막 키 시간 = max(0, 이벤트 키 시간들, 각 트랙 커브의 마지막 키 시간)(`TL:676–709` — 빈 커브는 0, 커브 없는 트랙은 0 — UE는 null 커브 트랙에서 null 역참조).
- `GetScaledTimelineLength()`: `playRate`가 0이 아니면 길이 / `playRate`, 0이면 0(UE는 Error 로그 — Rocks는 로그 없이 0, 32.6절 4).
- `SetTimelineLength(newLength)`: `length = newLength`, `position > newLength`면 `SetNewTime(newLength − 1e-4)`.
- `SetTimelineLengthMode`, `SetLooping`/`IsLooping`, `SetPlayRate`/`GetPlayRate`, `GetPlaybackPosition`, `SetIgnoreTimeDilation`/`GetIgnoreTimeDilation`.
- 트랙 편집(UE `AddInterpFloat`·`SetFloatCurve` 대응, 실패 Result):
  - `TryAddFloatTrack(trackName, curve)`·`TryAddVectorTrack`·`TryAddColorTrack`(인라인 커브 사본), `TryAddEventKey(time, eventName)`. 오류: 할당 실패, 시간 비유한 `InvalidArgument`.
  - `SetFloatTrackCurveAsset(trackName, asset)`(벡터·색 같은 꼴 — UE `SetFloatCurve(curve, trackName)`): 이름이 같은 첫 트랙의 `curveAsset`을 바꾸고 로드 사본을 지운다. 이름이 비었거나 없으면 `NotFound`(UE는 Log만).
  - UE `AddInterp*`는 null 커브를 거부한다(Warning). Rocks 인라인 트랙은 늘 커브가 있다(빈 커브는 0).

### 32.4 시험(`WorldIntegration/Tests/TimelineComponentTests.cpp`, `Timeline_` 접두)

1. 기본값(UE 생성자)·Tick 꺼짐으로 시작, `Play`가 Tick을 켜고 끝나면 끔.
2. 앞으로 재생: delta별 위치, 트랙 값(인라인 float·벡터·색 — 색은 조정 적용), 이벤트 순서(벡터 → float → 색 → 키 → updated).
3. 이벤트 키 구간 규칙(앞으로 [old, new), 끝에서 1e-4 넓힘, 뒤로 (new, old], 0에서 넓힘), 같은 시간 키 여럿.
4. 끝(loop 아님): 끝 위치·Stop·`finished` 한 번·`Stop`으로 멈추면 `finished` 없음.
5. loop 앞으로·뒤로: 끝 키 발행, 0·끝 점프에서 이벤트 없음·update 없음, 남은 delta 위치, 길이 0 loop.
6. `Reverse`·`ReverseFromEnd`·`PlayFromStart`, `playRate` 음수(뒤로)·0, `GetScaledTimelineLength`.
7. 길이: `LastKeyFrame`(이벤트 키·트랙 커브 마지막 키·빈 커브), `SetTimelineLength`가 위치를 자름, `SetNewTime` 자르기.
8. `ignoreTimeDilation`: World 시간 배율 0.5에서 delta 차이.
9. 로드 사본: 에셋 트랙은 사본 전 평가하지 않음, `ApplyLoaded*` 뒤 평가, 요청 참조 불일치 `InvalidArgument`, `SetFloatTrackCurveAsset`이 사본을 지움, 이름 없음 `NotFound`.
10. 구독 없는 트랙은 평가하지 않음(구독이 생기면 평가 — 결과 같음), NaN·Inf delta, 큰 delta loop 반복 상한(32.6절 3).
11. 리플렉션 왕복(트랙·키·설정 저장, 로드 사본은 저장하지 않음)과 이벤트 바인딩 저장(ED19).

### 32.5 결정

| ID | 내용 | 근거 |
|---|---|---|
| ERD71 | 트랙 커브는 인라인 값 + 선택적 `SoftObjectRef` 에셋(로드 사본) | ERD60(World → 에셋은 soft), Blueprint 안 커브 대응, 30.3절 로드 사본 규칙 |
| ERD72 | 트랙 출력·이벤트 키·update·finished는 컴포넌트 `Event` 필드(트랙 이름 인자) | `Event`는 struct·배열에 둘 수 없다(ED14), 구독할 때만 비용(ED13 — CR7 해소) |
| ERD73 | 이름 기반 속성 쓰기(`PropertySetObject`·`DirectionPropertyName`)는 두지 않는다 | Blueprint 생성 변수 전용이다. Sparks·C++는 이벤트로 받는다(방향은 `IsReversing`) |
| ERD74 | 모듈 `WorldIntegration` | 커브 에셋 타입 — World는 에셋 모듈을 참조하지 않는다(30.1절) |

### 32.6 UE와 다른 점

1. 트랙 출력은 컴포넌트 이벤트 하나에 트랙 이름 인자로 온다(ERD72). 이름 기반 속성 쓰기와 방향 속성은 없다(ERD73).
2. UE `Activate`·`Deactivate`(컴포넌트 활성 bit + Tick)는 Tick 켜기·끄기만이다. Rocks 컴포넌트 활성(`SetEnabled`)은 사용자 몫이다(꺼진 컴포넌트는 Tick이 없어 재생이 멈춘다).
3. loop에서 남은 위치를 구하는 뺄셈 반복은 `MaxTimelineLoopWraps`(1024)번까지이고 넘으면 정확한 나머지(`math::Fmod`)로 줄인다. UE는 상한 없는 `while`이라 길이에 비해 아주 큰 delta에서 오래 돌고, float에서 `newPosition − length == newPosition`이면 끝나지 않는다. 비유한 delta·`playRate`(effective가 NaN·±Inf)는 그 Tick을 건너뛴다(UE는 위치가 NaN이 된다).
4. `GetScaledTimelineLength`의 `playRate` 0과 알 수 없는 길이 모드는 로그 없이 0이다(UE Error 로그). 커브 없는 트랙(로드 전 에셋 트랙)은 마지막 키 시간 계산에서 0이다(UE는 null 역참조).
5. `SetFloatTrackCurveAsset` 등은 이름이 없으면 `NotFound`이다(UE Log). 트랙 추가는 인라인 커브라 null 거부가 없다.
6. 복제(`ReplicatedUsing=OnRep_Timeline`, `PlayingStateTracker`의 dirty 수, 시뮬레이션 클라이언트의 Deactivate 예외)는 CR24 복제 단계다.
7. UE `IsReadyForOwnerToAutoDestroy`(재생 중이 아니면 참)는 Rocks에 소유 자동 파괴가 생길 때 붙인다.
