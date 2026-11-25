# 입력 액션 계층 — 3. 모디파이어·트리거

[목차](README.ko.md)

## 3. 모디파이어·트리거

UE는 `UInputModifier`·`UInputTrigger` 파생 객체(Instanced)다. Rocks는 **종류 enum과 값 struct**다.
- 내장 종류의 매개변수를 한 struct에 둔다(`InputModifier { kind; 매개변수… }`).
- 평가는 `switch`다.
- 실행 중 상태(Smooth 표본, 트리거 누적 시간·직전 값)는 평가기가 소유한다. 바인딩마다, 액션마다, 주입마다 따로 둔다(4.6절). 데이터는 불변이다. UE는 플레이어마다 객체를 복제해 상태를 그 안에 둔다(`InputAction.cpp:24-49`).
- 사용자 정의 종류는 Sparks 바인딩 단계다(7.4절).

### 3.1 모디파이어(UE `EI/Private/InputModifiers.cpp`)

적용 순서:
1. 키 원시값을 액션 타입으로 바꾼다(2.1절).
2. 바인딩 모디파이어를 배열 순서로 적용한다.
3. 같은 액션의 바인딩 값을 누적한다(4.3절).
4. 액션 모디파이어를 적용한다.

모디파이어 사이에서 값 타입은 입력 타입으로 되돌린다(UE `EnhancedPlayerInput.cpp:234-235·682-683·1149-1163`). 모디파이어가 받는 dt는 dilation을 뺀 dt다(4.5절).

| 종류 | 매개변수(기본) | 식 |
|---|---|---|
| `DeadZone` | lower 0.2, upper 1.0, type `Radial` | f(a) = min(1, max(0, \|a\| − L) / (U − L)) · sign(a)<br>`Axial`: 성분마다 f<br>`Radial`: 2D는 v̂ · f(\|v₂\|), 3D는 v̂ · f(\|v\|), 1D는 f(x)<br>`UnscaledRadial`: \|a\| < L이면 0, 아니면 min(\|a\|, U) · sign(a)<br>Boolean은 그대로 둔다 (cpp:188-243) |
| `Scalar` | (1, 1, 1) | v ⊙ s. Boolean은 그대로 둔다 (cpp:100-108) |
| `ScaleByDeltaTime` | — | v · dt. Boolean은 그대로 둔다 (cpp:114-122) |
| `Negate` | x·y·z 참 | 켠 축 × −1 (cpp:129-132) |
| `Swizzle` | `YXZ` | YXZ: x↔y<br>ZYX: x↔z<br>XZY: y↔z<br>YZX: (y, z, x)<br>ZXY: (z, x, y) (cpp:473-495) |
| `ResponseCurveExponential` | (1, 1, 1) | 타입의 축마다 sign(a) · \|a\|^e (cpp:350-366) |
| `ResponseCurveUser` | 축마다 커브 | curve(a). 커브는 Curves 모듈 `TangentCurve`(UE `UCurveFloat` = FRichCurve)다. 커브가 없으면 그 축은 0이다 (cpp:368-385) |
| `FovScaling` | scale 1, type `Standard` | scale · tan(fov/2) / tan(40°)(기준 FOV 80 — cpp:405-419). fov는 평가 문맥의 카메라 FOV다. `UE4BackCompat`은 레거시라 넣지 않는다 |
| `ToWorldSpace` | — | 2D (y, x), 3D (z, x, y) (cpp:435-454) |
| `Smooth` | 표본 시간 0.0083 s | UE 마우스 평활 식(cpp:270-333). dt ≥ 0.25 s면 초기화한다 |
| `SmoothDelta` | method Lerp, speed 0.5, exponent 2 | 출력은 **정규화한 변화 방향**이다. 이전 값을 normalize(새 값 − 이전 값) 쪽으로 정한 보간식으로 옮긴다(cpp:32-65). 보간식은 UE 15종 전부다: Lerp, InterpTo, InterpConstantTo, Circular In·Out·InOut, Ease In·Out·InOut(exponent), Expo In·Out·InOut, Sin In·Out·InOut. Boolean은 지원하지 않는다(그대로 둔다) |

레거시 축 설정의 자동 모디파이어(`input.GlobalAxisConfigMode`)는 들이지 않는다.

### 3.2 트리거(UE `EI/Private/InputTriggers.cpp`)

- `TriggerState { None, Ongoing, Triggered }`
- `TriggerKind { Explicit, Implicit, Blocker }`(기본 Explicit)
- 공통 `actuationThreshold` 0.5. 작동은 \|value\|² ≥ thr²다(UE h:130·143).
- 트리거마다 직전 값(`lastValue`)을 상태로 둔다. 평가 뒤 갱신한다(UE `InputAction.cpp:91`).
- `alwaysTick` 트리거는 키 이벤트가 없어도 매 평가 돈다(UE `bShouldAlwaysTick`).

**시간 트리거**(UE `UInputTriggerTimedBase` — h:245):
- 해당: Hold·HoldAndRelease·Tap·RepeatedTap·Pulse
- 공통 `affectedByTimeDilation`(기본 거짓)
- 작동 중이면 Ongoing이고 누적 += dt다. 이 dt는 dilation을 뺀 dt이고, `affectedByTimeDilation`이면 × dilation이다. 비작동이면 누적을 0으로 둔다(cpp:114-149).

| 종류 | 매개변수(기본) | 상태 |
|---|---|---|
| `Down` | — | 작동이면 Triggered |
| `Pressed` | — | 작동이고 직전이 비작동이면 Triggered |
| `Released` | — | 작동 중 Ongoing, 놓는 평가에 Triggered |
| `Hold` | holdTime 1.0, oneShot 거짓 | 누적 ≥ holdTime이면 Triggered. oneShot이면 첫 한 번만이고 그 뒤 None |
| `HoldAndRelease` | holdTime 0.5 | 놓는 평가에서 직전 누적 ≥ holdTime이면 Triggered |
| `Tap` | releaseTime 0.2 | 직전 작동·지금 비작동·직전 누적 < releaseTime이면 Triggered. 누적 ≥ releaseTime이면 None |
| `RepeatedTap` | repeatDelay 0.5, taps 2, releaseTime 0.2 | 탭 사이 Ongoing, N번째 탭에 Triggered. alwaysTick이다 |
| `Pulse` | triggerOnStart 참, interval 1.0, limit 0(무제한) | 누적 > interval · (start ? n : n+1)이면 Triggered이고 n++. 한도에 닿으면 None |
| `ChordAction` | chordAction(InputAction) | Implicit. 대상 액션의 **이번 평가 바인딩 수준 결합 상태**(4.3절 2 뒤, 액션 트리거 전)를 따른다. Triggered가 아니면 막는다(UE `InputAction.cpp:68-71`) |
| `ChordBlocker` | chordAction(평가기가 넣는다) | Blocker. 대상 chord가 Triggered면 막는다 |

시간 기준:
- UE `RepeatedTap`은 벽시계(`FPlatformTime::Seconds()` — cpp:270)를 쓴다. Rocks는 평가 dt 누적을 쓴다(결정적 시험 — IAD3).
- UE `Combo`는 5.8에서 deprecated라 넣지 않는다(h:571).

### 3.3 트리거 결합(UE `InputAction.cpp:73-129` `FTriggerStateTracker`)

모든 트리거를 끝까지 평가한다.
- Explicit: `foundExplicit = true`, `anyExplicit |= (state == Triggered)`
- Implicit: `allImplicit &= (state == Triggered)`
- Explicit·Implicit 중 None이 아닌 것이 있으면 `active = true`다. **Blocker는 active를 세우지 않는다**(:96-110).
- 막는 트리거가 있으면 `blocking = true`

결과:
- 트리거가 하나도 없으면 값이 nonzero일 때 Triggered, 아니면 None이다. 임계 0.5가 아니다(UE `EnhancedPlayerInput.cpp:241`).
- blocking이면 None이다.
- (`!foundExplicit || anyExplicit`) && `allImplicit`면 Triggered다.
- 그 밖에 active면 Ongoing, 아니면 None이다.

### 3.4 상태 전이와 이벤트

`ActionEvent` 비트 플래그는 Triggered 1, Started 2, Ongoing 4, Canceled 8, Completed 16이다(UE h:34-56).

| 이전 → 지금 | 이벤트 |
|---|---|
| None → Ongoing | Started |
| None → Triggered | Started \| Triggered |
| Ongoing → None | Canceled |
| Ongoing → Ongoing | Ongoing |
| Ongoing → Triggered | Triggered |
| Triggered → Triggered | Triggered |
| Triggered → Ongoing | Ongoing |
| Triggered → None | Completed |

(UE `EnhancedPlayerInput.cpp:120-176`)
