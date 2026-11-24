# 엔진 런타임 — 25. SplineComponent(D3)

[목차](README.ko.md)

## 25. SplineComponent(D3)

설계일 2026-11-25. [24장](24-ComponentData.ko.md) 데이터 먼저 묶음의 D3다. 24장이 길어져 묶음마다 장을 나눈다(D4는 26장, D5는 27장). 스플라인은 렌더·물리 없이 완결되므로 **끝까지 동작**한다. 디버그 그리기와 편집기 시각화만 RS·Studio 몫이다. UE 경로 약어: `SC` = `Engine/Source/Runtime/Engine/Private/Components/SplineComponent.cpp`, `SC.h` = `Classes/Components/SplineComponent.h`, `IC.h` = `Core/Public/Math/InterpCurve.h`.

### 25.1 기준 경로

- UE 5.8의 기본 실행 경로는 옛 `FSplineCurves`다(`SplineComponent.UseSplineCurves` 기본 참 — `SC:45`). 새 `FSpline`·`FTangentSpline`은 `UE_EXPERIMENTAL(5.7)`이고 기본으로 꺼져 있다(`Curves/Spline.h:10`, `Spline.cpp:23`). Rocks는 `FSplineCurves` 의미만 들인다.
- 곡선은 Core `InterpCurve`(UE `FInterpCurve` 대응 — [InterpCurve 구현](../../Implementation/InterpCurve.ko.md))를 쓴다. 위치·scale은 `InterpCurve<Vec3d>`, 회전은 `InterpCurve<Quatd>`다(UE 5 `FVector`·`FQuat`는 double). 셋 다 CoreTypeBindings 미러가 있어 영속 필드가 된다.

### 25.2 데이터

`SplineComponent : PrimitiveComponent`(World, UE `USplineComponent : UPrimitiveComponent`). 생성자 `SC:334–383`.

| 필드(영속) | 기본 | UE |
|---|---|---|
| `position`·`rotation`·`scale` | 2점(25.3절) | `SplineCurves.Position`·`Rotation`·`Scale` |
| `reparamStepsPerSegment` | 10 | `ReparamStepsPerSegment`(ClampMin 4·ClampMax 100 — 밖은 거부) |
| `duration` | 1 (초) | `Duration`(0 이상) |
| `stationaryEndpoints` | 거짓 | `bStationaryEndpoints` |
| `closedLoop` | 거짓 | `bClosedLoop` |
| `loopPositionOverride`·`loopPosition` | 거짓·0 | `bLoopPositionOverride`·`LoopPosition` |
| `defaultUpVector` | (0, 0, 1) | `DefaultUpVector`(Local 공간) |

- 생성자: 충돌 profile 기본 "NoCollision", `generateOverlapEvents` 거짓(`SC:358–359`).
- 거리 재매개변수 표(UE `ReparamTable` — `InterpCurveFloat`, 거리 → 키)는 영속하지 않는다. 곡선에서 다시 만든다(25.4절). UE는 저장하지만 값은 곡선에서 유도된다.
- 열거: `SplinePointType : uint8 { Linear, Curve, Constant, CurveClamped, CurveCustomTangent }`(UE `ESplinePointType`), `SplineCoordinateSpace : uint8 { Local, World }`. 보간 모드 대응은 `SC:476–502`와 같다: Linear↔Linear, Curve↔CurveAuto, Constant↔Constant, CurveClamped↔CurveAutoClamped, CurveCustomTangent↔CurveUser, 그 밖의 모드 → Constant.
- `SplinePoint`(UE `FSplinePoint` — `SC.h:156`): `inputKey` 0, `position`·`arriveTangent`·`leaveTangent` 0, `rotation`(Rotator 도) 0, `scale` 1, `type` Curve. 영속 형식이 아니라 API 인자다.
- 들이지 않는 UE 필드: 편집기 시각화(`bDrawDebug`·색·`bShouldVisualizeScale`·`ScaleVisualizationWidth`·`bAllowDiscontinuousSpline`·`bAdjustTangentsOnSnap`), construction script 필드(`bSplineHasBeenEdited`·`bModifiedByConstructionScript`·`bInputSplinePointsToConstructionScript` — Rocks에 UCS가 없다), `OnSplineChanged`·`OnSplineUpdated` delegate(CR7 이벤트 때), 새 `FSpline`.

### 25.3 기본 점

- UE `SetDefaultSpline`(`SC:424–465`): 키 0 위치 (0, 0, 0), 키 1 위치 (100, 0, 0), 모두 `CurveAuto`, 회전 단위 quaternion, scale (1, 1, 1). 생성자가 `UpdateSpline`을 부르면 자동 접선이 (100, 0, 0)이 되고 길이는 100이다.
- Rocks는 생성자에서 두 점을 넣는다(`InterpCurve`는 allocator를 빌려 키 배열을 할당한다). 할당이 실패하면 빈 스플라인으로 남는다(생성자는 실패를 알릴 수 없다). 저장된 객체는 로드가 곡선을 덮어쓴다.

### 25.4 UpdateSpline

UE `FSplineCurves::UpdateSpline`(`SC:733–788`)과 같은 순서다.

1. 닫힌 스플라인이면 루프 키 = `loopPositionOverride ? loopPosition : 마지막 키 + 1`을 세 곡선에 건다(`TrySetLoopKey`). 루프 키가 마지막 키 이하면 UE처럼 루프가 꺼진다(열린 스플라인). 열려 있으면 `ClearLoopKey`.
2. 세 곡선에 `AutoSetTangents(0, stationaryEndpoints)`.
3. 재매개변수 표: 구간 수 = 루프면 N, 아니면 max(0, N − 1). 구간 s, 단계 i ∈ [0, steps)마다 (누적 거리 + 길이(s, i/steps), s + i/steps)를 **차례로 덧붙인다**(Constant 구간은 같은 거리 키가 이어진다 — 순서가 의미다). 구간 끝마다 누적 거리 += 길이(s, 1). 마지막 항목은 (전체 길이, 구간 수)다. 표 값은 구간 번호(키 0, 1, 2, …를 가정)다 — UE와 같다.
4. 구간 길이(UE `GetSegmentLength` — `SC:854–920`): scale은 **월드 scale**(`WorldScale3D` — UE `GetComponentTransform().GetScale3D()`)을 성분별로 곱한다.
   - Linear: |(P1 − P0)·S|·param. Constant: param = 1이면 |(P1 − P0)·S|, 아니면 0. 모드는 시작 점의 것이다.
   - 그 밖: 5점 Legendre–Gauss 적분. C1 = ((P0 − P1)·2 + T0 + T1)·3, C2 = (P1 − P0)·6 − T0·4 − T1·2, C3 = T0(T0 = 시작 leave, T1 = 끝 arrive — 키 간격 1 가정, UE와 같다). h = param/2, a = h(1 + x), L += |((C1·a + C2)·a + C3)·S|·w, L ×= h. 가중치·abscissa는 UE 상수(0·0.5688889, ±0.5384693·0.47862867, ±0.90617985·0.23692688)이고 float로 누적한다.
   - 표의 거리·값은 UE처럼 `float`다.
5. 표를 만들 때의 월드 scale을 기억한다. UE는 Transform이 바뀌어도 표를 다시 만들지 않는다(표가 낡는다). Rocks는 `OnTransformUpdated`에서 월드 scale이 바뀌었으면 표를 버리고 다음 질의가 표만 다시 만든다(루프 키·접선은 그대로). 문서화된 차이다. `updateSpline` 거짓으로 편집한 뒤 scale이 바뀌면, 다시 만드는 표는 갱신되지 않은 접선·루프 키의 곡선에서 나온다(UE는 옛 표를 그대로 둔다). 편집을 마치면 `UpdateSpline()`을 부르는 것이 규약이다.
- 생성자는 루프 키·접선만 계산하고 표는 만들지 않는다. 로드는 생성자 뒤에 곡선을 덮어쓰므로 첫 질의가 표를 만든다.
- 편집 API의 `updateSpline` 인자는 UE와 같다: 거짓이면 접선·표를 다시 계산하지 않는다. 여러 점을 바꾼 뒤 `UpdateSpline()`을 한 번 부른다.

### 25.5 질의

모두 Local 공간에서 계산한 뒤 World면 컴포넌트 월드 변환을 적용한다.
- 위치는 월드 행렬 `TransformPoint`, 접선은 `TransformVector`(scale 포함), 방향은 `TransformVector` 뒤 정규화다.
- 회전·up·right는 컴포넌트 월드 회전 `SceneComponent::WorldQuaternion()`을 쓴다(UE `GetComponentQuat` — 부모 사슬의 상대·socket 회전 합성). 행렬 분해(`TryWorldTransform`)는 shear·0 scale에서 실패하고 반사 부호를 X로 옮겨 UE와 다른 회전을 내므로 쓰지 않는다.
- World 입력은 역행렬로 바꾼다(역변환할 수 없으면 `InvalidArgument`). Local 입력은 그대로 쓴다(월드를 거치지 않는다).

| 함수 | 식(UE) |
|---|---|
| `LocationAtSplineInputKey(k, space)` | `position.Eval(k)` |
| `TangentAtSplineInputKey` | `position.EvalDerivative(k)` |
| `DirectionAtSplineInputKey` | 위 미분의 정규화(0이면 0) |
| `QuaternionAtSplineInputKey` | q = 정규화(`rotation.Eval(k)`), up = q·defaultUpVector, `MakeFromXZ(방향, up)` → quaternion. World는 컴포넌트 회전 × 결과(`SC:1051–1083`) |
| `UpVector`·`RightVector`·`RollAtSplineInputKey` | 위 quaternion의 +Z·+Y, Rotator roll(도) |
| `ScaleAtSplineInputKey` | `scale.Eval(k, 1)`(공간 없음) |
| `TransformAtSplineInputKey(k, space, useScale)` | (quaternion, 위치, scale 또는 1). World는 UE `T × ComponentTransform`처럼 성분 합성이다: 회전 = `WorldQuaternion` × 회전, scale = 성분별 곱(`WorldScale3D`), 위치 = 월드 행렬로 옮긴 점. 실패하지 않는다 |
| `DistanceAlongSplineAtSplineInputKey(k)` | 0 ≤ k < 구간 수면 표[⌊k⌋·steps].거리 + 길이(⌊k⌋, k − ⌊k⌋), k ≥ 구간 수면 전체 길이, 아니면 0 |
| `InputKeyValueAtDistanceAlongSpline(d)` | 표 선형 평가(범위 밖은 끝값으로 자름). 점이 2개 미만이면 0 |
| `DistanceAlongSplineAtSplinePoint(i)`·`SplineLength()` | 표[i·steps].거리(닫힌 스플라인 i = N이면 전체 길이)·표 마지막 거리 |
| `*AtDistanceAlongSpline(d, …)` | 위 키 질의(InputKeyValueAtDistanceAlongSpline(d)) |
| `InputKeyValueAtTime(t)` | duration > 0이면 키 범위를 clamp(t, 0, duration)/duration으로 보간, 아니면 범위 시작 |
| `*AtTime(t, …, useConstantVelocity)` | duration = 0이면 0·단위 회전. 일정 속도면 `*AtDistance(t/duration·길이)`, 아니면 `*AtSplineInputKey(InputKeyValueAtTime(t))` |
| `TimeAtDistanceAlongSpline(d)` | 점 2개 미만이면 0, 아니면 duration × GetT(키)(UE `FInterval1f::GetT` — 키 ≤ 시작 0, ≥ 끝 1, 그 사이 비율) |
| `FindInputKeyClosestToWorldLocation(p)` | 월드 → Local, `position.TryFindNearest`(빈 스플라인 0) |
| `FindLocationClosestToWorldLocation(p, space)`·`DistanceAlongSplineAtLocation(p, space)` | 위 키에서 위치·거리 |

- 표 선형 평가는 UE `FInterpCurve::Eval`(Linear 키 — `IC.h:310`)과 같다: 입력 < 첫 거리면 첫 값, 아니면 거리 ≤ 입력인 마지막 항목 i(이진 탐색 — `GetPointIndexForInputValue`)를 고르고, i가 마지막이면 마지막 값, 아니면 i와 i + 1 사이를 보간한다(간격 0이면 i의 값). 같은 거리가 이어지면(Constant 구간·길이 0 구간) 뒤 항목이 뽑힌다. 예: 첫 구간이 Constant면 거리 0의 키는 0.9(단계 10)다.
- `MakeFromXZ`(UE `RotationMatrix.h:137`): X = 정규화(방향), N = 정규화(up). |X·N|이 1에 가까우면(1e-8) N을 |X.z| < 1 − 1e-4일 때 (0, 0, 1), 아니면 (1, 0, 0)으로 바꾼다. Y = 정규화(N × X), Z = X × Y. 방향이 0이면 UE처럼 단위 quaternion이다.
- 키 범위(UE `GetInputKeyRange`): [첫 키, 마지막 키 + (루프면 오프셋)]. 점이 없으면 [0, 0]이다(UE는 빈 구간 FLT_MAX — 의미 없는 값이라 0으로 둔다).

### 25.6 점 편집

UE `SC:1462–2300`의 의미를 따른다. 범위 밖 index는 UE가 조용히 무시하지만 Rocks는 `InvalidArgument`다(값 유지).

- `ClearSplinePoints(update)`, `AddSplinePoint(위치, space, update)`(키 = 마지막 + 1, 빈 곡선은 0, CurveAuto·단위 회전·scale 1, `loopPositionOverride`면 `loopPosition` + 1), `AddSplinePointAtIndex(위치, i, space, update)`(키 = i = 0이면 0, 아니면 키[i−1] + 1, 뒤 키를 1씩 민다), `AddPoint(SplinePoint, update)`(UE `AddPoint` — 앞 키와 같으면(`FMath::IsNearlyEqual(float, float)` 기본 1e-8) 그 자리에 넣고 뒤를 민다, 회전은 `Rotator → quaternion`, `loopPosition` ≤ 마지막 키면 override를 끈다, 비유한 값은 거부), `RemoveSplinePoint(i, update)`(뒤 키에서 앞 간격만큼 뺀다), `SetSplinePoints(위치들, space, update)`(키 0, 1, 2, …로 다시 만든다, override를 끈다).
- `SetLocationAtSplinePoint`, `SetTangentAtSplinePoint`·`SetTangentsAtSplinePoint`(모드를 CurveUser로), `SetUpVectorAtSplinePoint`(회전 키 = `FindBetween(defaultUpVector, 정규화(up))`), `SetQuaternionAtSplinePoint`(UE 같은 이름 — 상대 회전 Rel = q × 지금 회전⁻¹, up을 q의 +Z로, 접선 방향을 Rel로 돌리며 크기 유지 — CurveUser가 된다)·`SetRotationAtSplinePoint`(도 단위 Rotator 판), `SetScaleAtSplinePoint`, `SetSplinePointType`(위치 곡선의 모드만), `SetClosedLoop(b, update)`(override 끔), `SetClosedLoopAtPosition(b, key, update)`, `SetDefaultUpVector(up, space)`(늘 update), `SetReparamStepsPerSegment`·`SetDuration`·`SetStationaryEndpoints`(UE 편집 후처리처럼 update).
- 읽기: `SplinePointCount()`, `SplineSegmentCount()`(점 2개 미만 0, 루프면 N, 아니면 N − 1 — 루프 여부는 곡선의 실제 루프 상태), `InputKeyValueAtSplinePoint(i)`, `LocationAtSplinePoint(i, space)`, `ArriveTangentAtSplinePoint`·`LeaveTangentAtSplinePoint`, `ScaleAtSplinePoint`, `SplinePointTypeAt(i)`, `QuaternionAtSplinePoint(i, space)`(그 점의 키에서 평가), `DistanceAlongSplineAtSplinePoint(i)`. 점 단위 up·right·roll·transform은 `*AtSplineInputKey(InputKeyValueAtSplinePoint(i))`로 얻는다.
- `FindBetween`(UE 5.8 `FindBetween_Helper` — `UnrealMath.cpp:1402`): w = |A||B| + A·B, w ≥ 1e-6·|A||B|면 (A × B, w), 아니면(반대 방향) 기저 = (|A.x| > |A.y| 그리고 |A.x| > |A.z|) ? +Y : −X, (A × 기저, 0), 정규화. UE 4의 옛 식((−A.y, A.x, 0) 등)과 다르다.

### 25.7 bounds

- UE `CalcBounds`(`SC:3925–3971`): 구간마다 `CurveVectorFindIntervalBounds`의 최소·최대(끝점과 Hermite 극값), 점 1개면 그 점, 0개면 원점. 결과는 `FBoxSphereBounds(FBox(min, max).TransformBy(LocalToWorld))` — 구 반지름은 **옮긴 상자 extent의 길이**다.
- Rocks는 `position.TryCalcBounds`(같은 알고리즘, 루프 구간 포함)로 지역 상자를 만들고, `TryLocalBounds`로 넘긴다. 구 반지름 규칙은 Shape(24.3.6절 — min(R × 최대 열 길이, |extent|))와 달라서, `TransformComponentBounds`에 `BoundsSphereRule::BoxExtent`(반지름 = |새 extent|)를 더하고 SceneComponent의 virtual `LocalBoundsSphereRule()`이 고른다(기본 `TransformedSphere`).
- 스플라인을 바꾸면(`UpdateSpline`) bounds를 다시 계산한다.

### 25.8 UE와 다른 점

| 항목 | UE | Rocks | 근거 |
|---|---|---|---|
| 회전 곡선 보간 | `FInterpCurveQuat`(간격으로 나눈 접선 × Diff, Squad) | Core `InterpCurve<Quatd>`의 표준 SQUAD | Core 결정([InterpCurve 구현](../../Implementation/InterpCurve.ko.md) 4절). 기본 단위 회전에서는 같고, 사용자 회전 키 사이 모양이 조금 다르다 |
| CurveClamped 위치 접선 | FVector 성분을 float 쌍으로 잘못 읽는 버그(`SC:95–105` 주석이 인정) | 성분별로 올바르게 clamp | Core InterpCurve |
| Linear 자동 접선 | 값 차이 | 기울기(값 차이 ÷ 키 간격) | Core InterpCurve. 키 간격 1이면 같다 |
| scale이 바뀐 뒤 표 | 낡은 채로 둔다 | 월드 scale이 바뀌면 표를 다시 만든다 | 25.4절 5 |
| 범위 밖 index | 조용히 무시 | `InvalidArgument` | Rocks setter 규약 |
| 점 없는 키 범위 | FLT_MAX 빈 구간 | [0, 0] | 25.5절 |
| 재매개변수 표 영속 | 저장 | 저장하지 않고 다시 만든다 | 유도 값 |
| 거리·구간 길이의 구간 수 | 컴포넌트 `bClosedLoop`(`SC:1176`, `GetSegmentLength`에 넘긴다) | 곡선의 실제 루프 상태(`IsLooped`) | 잘못된 루프 키(마지막 키 이하)로 UE 곡선이 열렸는데 `bClosedLoop`가 참이면 없는 닫는 구간을 센다 — Rocks는 표와 같은 구간 수를 쓴다 |
| 컴포넌트 월드 회전 | `GetComponentQuat` | `WorldQuaternion`(같은 합성) | 같다. 행렬 분해를 쓰지 않는다(25.5절) |

### 25.9 결정·시험

| ID | 항목 | 결정 | 근거 |
|---|---|---|---|
| ERD42 | 스플라인 경로 | UE `FSplineCurves`만, 새 `FSpline`은 들이지 않음 | UE 기본 경로, 실험 기능 |
| ERD43 | 곡선 형식 | Core `InterpCurve<Vec3d>`·`<Quatd>` 재사용, 표는 World 내부 float 배열 | 중복 금지, UE 정밀도 |
| ERD44 | 표 갱신 | 편집·월드 scale 변경 때 다시 만들고 영속하지 않음 | 25.4절 |
| ERD45 | bounds 구 반지름 | `BoundsSphereRule`로 클래스가 고른다 | UE 클래스마다 다른 식 |

시험 `World/Tests/WorldSplineComponentTests.cpp`(`Spline_` 접두):
1. 기본값·기본 두 점·길이 100·profile.
2. 점 편집: 추가·삽입·삭제의 키 이동, `loopPosition` 조정, 범위 밖 거부, `updateSpline` 거짓의 지연, `SetSplinePoints`.
3. 길이: Linear·Constant·Curve 구간(곡선은 독립 수치 적분과 대조), 월드 scale 반영과 scale 변경 뒤 갱신, 닫힌 루프·`loopPositionOverride`·잘못된 루프 키.
4. 거리↔키 왕복, 일정 속도·시간 질의, duration 0.
5. 위치·접선·방향·회전(MakeFromXZ, up 평행 경우)·up·right·roll·scale·transform, Local·World(회전·scale한 컴포넌트).
6. 최근접 키·위치·거리.
7. 회전·up 설정(`SetUpVectorAtSplinePoint`·`SetRotationAtSplinePoint`), 접선 설정과 CurveUser, 점 type.
8. bounds(곡선 극값 포함, 월드 회전 — 반지름 = |extent|).
9. 리플렉션·저장 왕복(곡선·설정), 로드 뒤 첫 질의가 표를 만든다.
