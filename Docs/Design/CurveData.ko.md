# 커브 데이터 계층 설계

개정 1 초안, 2026-11-18. 이 문서는 Curve 단계(리플렉션 B9 직후, A0-G 앞 — 2026-11-15 사용자 결정)의 **값 타입 층** 설계다. 근거는 [에셋 시스템](AssetSystem.ko.md) 1절 Curve 행, [Core 상세 명세](CoreDetailedSpec.ko.md) G09, 이미 구현된 수학 층 [키 커브](../Implementation/InterpCurve.ko.md)(`InterpCurve<T>`)다. UE 대응 대상은 UE 5.8.4의 `Engine/Source/Runtime/Engine/Classes/Curves`(`FRealCurve`·`FRichCurve`·`FSimpleCurve`·`FIndexedCurve`·`FKeyHandle`·`FKeyHandleMap`·`FIntegralCurve`·`FNameCurve`·`FStringCurve`·`FCompressedRichCurve`)와 평가 함수 `Public/Curves/CurveEvaluation.h`(`UE::Curves::EvalForTwoKeys`·`BezierInterp`·`WeightedEvalForTwoKeys`)다. 아래에서 `RC` = `Engine/Source/Runtime/Engine/Private/Curves/RichCurve.cpp`, `CE` = 같은 폴더 `CurveEvaluation.cpp`, `SC` = `SimpleCurve.cpp`, `KH` = `Classes/Curves/KeyHandle.h`다. 레거시·deprecated 경로(옛 버전 키 직렬화 보정, `UE_DEPRECATED` 함수, `FInterpCurvePoint` → `FRichCurveKey` 변환 생성자)는 대응하지 않는다.

에셋 층(`UCurveFloat`·`UCurveVector`·`UCurveLinearColor`·`UCurveTable`·`FRuntimeFloatCurve`·`FCurveTableRowHandle`)은 A1 에셋 시스템 뒤 A 단계의 일이라 범위 밖이다. 이 문서는 그 층이 기댈 경계(13절)만 정한다.

## 1. 요약

- 만드는 것은 스칼라 키 커브 둘(접선 있는 커브, 접선 없는 커브), 계단 커브 셋(정수·Name·String), 압축된 접선 커브 하나, 그리고 편집기·도구가 쓰는 키 핸들 표다.
- 평가 식은 UE `FRichCurve::Eval`과 같은 식을 같은 float 연산 순서로 옮긴다. UE와 비트까지 같게 하는 것이 기본이고, 다르게 하는 곳은 UE 결함이나 Rocks 규약 때문인 곳뿐이며 14절에 모두 적는다.
- 값 타입은 처음부터 리플렉션 struct(`ROCKS_STRUCT`)로 만든다. B9에서 연 struct 상속(B9k)·Optional(B9e)·Name(B9c)·Map 값·InstancedStruct<Base>(B9j·B9k)를 쓴다. 미러(`Binds`)는 쓰지 않는다 — 새 타입이라 감쌀 Core 타입이 없다.
- 평가 핵심 식(두 키 사이 보간, Bezier, 가중 접선 3차 방정식, 시간 순환)은 Core 수학 층(`Core/Math/KeyCurveMath.h`)에 두고, 리플렉션 값 타입은 새 Core 런타임 모듈 `Curves`에 둔다. Engine 층에는 아무것도 두지 않는다.
- 키 핸들은 값의 일부가 아니다. UE처럼 저장하지 않고, 값 타입 밖의 편집용 표(`CurveKeyHandleTable`)가 따로 쥔다. 값 타입의 공개 API는 키 index로 동작한다.

## 2. 타입과 이름

Rocks 이름은 서술형이다. UE 이름은 아래 대응표에만 둔다.

| UE 5.8 | Rocks | 한 줄 설명 |
|---|---|---|
| `FIndexedCurve` | (없음) | 가상 함수 기반 공통 기반. Rocks 리플렉션 struct는 virtual 함수를 가질 수 없으므로(RMC2301) 공통 동작은 템플릿 자유 함수로 둔다(D2) |
| `FRealCurve` | `ScalarCurveBase` | 스칼라 커브 공통 칸: 기본값(Optional)·앞·뒤 외삽. 파생 둘의 리플렉션 base |
| `FRichCurve` | `TangentCurve` | 키마다 보간·접선·가중 접선을 갖는 스칼라 커브 |
| `FRichCurveKey` | `TangentCurveKey` | 시간·값·보간·접선 방식·가중 방식·도착·출발 접선과 가중치 |
| `FSimpleCurve` | `PlainCurve` | 접선 없이 커브 전체에 보간 하나(Linear·Constant)인 스칼라 커브 |
| `FSimpleCurveKey` | `PlainCurveKey` | 시간·값 |
| `FIntegralCurve` | `IntegerStepCurve` | int32 값 계단 커브 |
| `FIntegralKey` | `IntegerStepKey` | 시간·int32 값 |
| `FNameCurve` | `NameStepCurve` | Name 값 계단 커브 |
| `FNameCurveKey` | `NameStepKey` | 시간·Name 값 |
| `FStringCurve` | `StringStepCurve` | String 값 계단 커브 |
| `FStringCurveKey` | `StringStepKey` | 시간·String 값 |
| `FCompressedRichCurve` | `PackedTangentCurve` | `TangentCurve`의 압축 저장 형태(쿡 데이터용) |
| `FKeyHandle` | `CurveKeyHandle` | 키 하나를 가리키는 편집용 핸들 |
| `FKeyHandleMap` | `CurveKeyHandleTable` | 키 index ↔ 핸들 표(값 밖의 편집 상태) |
| `ERichCurveInterpMode` | `CurveInterp` | Linear 0·Constant 1·Cubic 2 |
| `ERichCurveTangentMode` | `CurveTangentMode` | Auto 0·User 1·Break 2 |
| `ERichCurveTangentWeightMode` | `CurveTangentWeight` | None 0·Arrive 1·Leave 2·Both 3 |
| `ERichCurveExtrapolation` | `CurveExtrapolation` | Cycle 0·CycleWithOffset 1·Oscillate 2·Linear 3·Constant 4 |
| `ERichCurveCompressionFormat` | `PackedCurveFormat` | Empty 0·Constant 1·Linear 2·Cubic 3·Mixed 4·Weighted 5 |
| `ERichCurveKeyTimeCompressionFormat` | `PackedKeyTimeFormat` | Quantized16 0·Float32 1 |
| `UE::Curves::EvalForTwoKeys`·`BezierInterp`·`WeightedEvalForTwoKeys`·`FRealCurve::CycleTime` | `math::EvalCurveSegment`·`math::BezierCurveValue`·`math::WeightedCurveSegment`·`math::CycleCurveTime` | Core 수학 층 핵심 식 |

enum 번호는 UE와 같게 둔다(golden 시험의 비교를 단순하게 하려는 것이고 UE 파일과의 호환 목적은 아니다). UE의 숨김 열거자 `RCIM_None`·`RCTM_None`·`RCCE_None`과 `RCTM_SmartAuto`는 두지 않는다(D4·D5).

## 3. 모듈 배치

- **Core 수학 층** — `Core/Source/Runtime/Core/Public/Core/Math/KeyCurveMath.h`: 키 쌍과 시간을 받아 값을 내는 순수 함수(5절), 시간 순환(5.1), 자동 접선 식(6절), 가중 접선의 3차 방정식 풀이. 할당·리플렉션 의존이 없어 `InterpCurve<T>`처럼 다른 Core 수학 코드와 결정성 수학 시험이 바로 쓸 수 있다. 3차 방정식은 이미 있는 `PolynomialRoots.h`와 목적이 다르다(UE Cardano 닫힌 해와 비트를 맞춘다 — 5.4절). 
- **커브 값 타입** — 새 모듈 `Core/Source/Runtime/Curves`(의존: Core·ObjectRuntime, Name 계단 커브 때문에 Core Name). 여기에 2절의 `ROCKS_STRUCT`들과 편집 연산·압축이 산다. ObjectRuntime에 넣지 않는 것은 그 모듈이 객체·리플렉션 기반이고 커브는 그 위의 데이터라서다.
- **Engine 층** — 없음. 커브를 소비하는 에셋 타입(13절)과 애니메이션·머티리얼·게임 코드는 각자 이 모듈을 의존한다.
- 편집기 전용 키 핸들 표(`CurveKeyHandleTable`)도 `Curves` 모듈에 둔다. 런타임 평가 경로에서 쓰지 않을 뿐 편집 도구·undo가 모두 쓰므로 별 모듈로 나눌 만큼 크지 않다(D3).

## 4. 데이터 정의

시간과 값은 모두 **float**다(UE `FRichCurveKey::Time`·`Value`가 float — 비트 대조의 전제, D1). 기본값·"없음" 표지는 sentinel 대신 `Optional`이다(D6).

```cpp
ROCKS_STRUCT()
struct ScalarCurveBase {
    ROCKS_PROPERTY() Optional<float> defaultValue;              // 키가 없을 때의 값(UE DefaultValue = MAX_flt는 "없음")
    ROCKS_PROPERTY() CurveExtrapolation preExtrapolation = CurveExtrapolation::Constant;
    ROCKS_PROPERTY() CurveExtrapolation postExtrapolation = CurveExtrapolation::Constant;
};

ROCKS_STRUCT(Atomic)
struct TangentCurveKey {
    ROCKS_PROPERTY() float time = 0;
    ROCKS_PROPERTY() float value = 0;
    ROCKS_PROPERTY() CurveInterp interp = CurveInterp::Linear;
    ROCKS_PROPERTY() CurveTangentMode tangentMode = CurveTangentMode::Auto;
    ROCKS_PROPERTY() CurveTangentWeight tangentWeight = CurveTangentWeight::None;
    ROCKS_PROPERTY() float arriveTangent = 0;
    ROCKS_PROPERTY() float arriveTangentWeight = 0;
    ROCKS_PROPERTY() float leaveTangent = 0;
    ROCKS_PROPERTY() float leaveTangentWeight = 0;
};

ROCKS_STRUCT()
struct TangentCurve : ScalarCurveBase {
    ROCKS_PROPERTY() Array<TangentCurveKey> keys;   // 시간 오름차순(D7)
};

ROCKS_STRUCT(Atomic)
struct PlainCurveKey { ROCKS_PROPERTY() float time = 0; ROCKS_PROPERTY() float value = 0; };

ROCKS_STRUCT()
struct PlainCurve : ScalarCurveBase {
    ROCKS_PROPERTY() CurveInterp interp = CurveInterp::Linear;   // Linear·Constant만(Cubic은 InvalidArgument)
    ROCKS_PROPERTY() Array<PlainCurveKey> keys;
};

ROCKS_STRUCT() struct IntegerStepCurve {
    ROCKS_PROPERTY() Optional<int32> defaultValue;
    ROCKS_PROPERTY() bool useDefaultBeforeFirstKey = false;   // UE bUseDefaultValueBeforeFirstKey
    ROCKS_PROPERTY() Array<IntegerStepKey> keys;
};
// NameStepCurve·StringStepCurve도 같은 모양(defaultValue는 Optional<Name>·Optional<String>, 6.3절의 평가 규칙)
```

- 키 struct는 `Atomic`이다. 키의 칸은 서로 맞물려 있어(접선은 보간 방식, 가중치는 가중 방식에 따라 뜻이 바뀐다) 차이 저장·override를 키 단위 통째로 하는 것이 맞다.
- 값 타입은 이동·복사 가능하고(배열 소유), 동기화는 없다. 바뀌지 않는 동안의 const 평가는 여러 스레드가 함께 해도 된다(UE와 달리 const 함수가 내부 상태를 고치지 않는다 — 핸들 표가 값 밖에 있으므로).
- 키 배열은 `Array<Key>`(RecordList wire)로 둔다. 키 하나에 Record 머리말이 붙어 UE의 수동 27바이트 직렬화보다 크다(키 9필드 기준 대략 2–3배). 저작 데이터는 편집·텍스트·차이 저장의 편의가 크기보다 중요하고, 런타임에 많이 읽는 데이터는 압축 형태(11절)를 쓰므로 받아들인다(D8).

## 5. 평가 의미

`TangentCurve::Evaluate(float time, float fallback = 0) const`가 UE `FRichCurve::Eval(InTime, InDefaultValue)`다. 모든 산술은 float이고 아래 식의 연산 순서를 그대로 지킨다(`-ffp-contract=off` — InterpCurve와 같은 빌드 규칙). 가중 접선의 3차 방정식만 double이다.

### 5.1 시간 재배치(외삽 순환) — UE `RemapTimeValue`(RC 1104–1161)·`CycleTime`(`RealCurve.cpp` 7–34)

키가 2개 미만이면 하지 않는다. `time <= keys[0].time`이고 앞 외삽이 Cycle·CycleWithOffset·Oscillate면(Linear·Constant가 아니면), 그리고 대칭으로 `time >= keys[N−1].time`이고 뒤 외삽이 그 셋이면 아래를 한다(경계 포함 비교도 UE와 같다).

```
min = keys[0].time; max = keys[N-1].time; duration = max - min
if time > max:  cycles = FloorToInt((max - time) / duration);  time = time + duration * cycles
elif time < min: cycles = FloorToInt((time - min) / duration);  time = time - duration * cycles
if time == max && 원래 time < min: time = min
if time == min && 원래 time > max: time = max
cycles = |cycles|
CycleWithOffset: 앞쪽은 offset = (keys[0].value - keys[N-1].value) * cycles, 뒤쪽은 (keys[N-1].value - keys[0].value) * cycles
Oscillate: cycles가 홀수면 time = min + (max - time)
```

`FloorToInt`는 UE `FloorToInt32`(`I = TruncToInt32(F); I -= ((float)I > F)`)와 같은 결과를 내는 Rocks 함수로 계산한다. **UE와 다른 점**: `duration == 0`(시간이 모두 같은 키 2개 이상)이면 UE는 0으로 나눠 정의되지 않은 정수 변환을 하지만 Rocks는 재배치를 하지 않는다(14절 1). ±Inf 입력도 UE는 정의되지 않은 변환이지만 Rocks는 재배치하지 않고 아래 5.3의 외삽 값으로 간다(14절 1).

### 5.2 구간 선택 — RC 1163–1253

```
value = defaultValue가 있으면 그 값, 없으면 fallback
N == 0:                         value
N == 1 또는 time < keys[0].time: 앞 외삽(5.5)
time <= keys[N-1].time:          k = upper_bound(keys[1..N-1], time) — 첫 keys[k].time > time인 k(없으면 N-1)
                                 value = EvalCurveSegment(keys[k-1], keys[k], time)
그 밖:                           뒤 외삽(5.5)
결과 = value + offset(5.1)
```

upper_bound는 UE와 같은 이진 탐색(`first = 1, count = N − 2`, `time >= keys[middle].time`이면 오른쪽)이다. 그래서 키 시간에 정확히 맞으면 그 키에서 시작하는 구간(비율 0)이고, 마지막 키 시간이면 마지막 구간의 비율 1이다. 같은 시간 키가 여럿이면 그 시간의 **마지막** 키에서 시작하는 구간을 쓴다.

### 5.3 두 키 사이 — `math::EvalCurveSegment`(UE `EvalForTwoKeys`, CE 205–250)

```
diff = k2.time - k1.time
diff <= 0:                       k1.value
k1.interp == Constant:           time < k2.time ? k1.value : k2.value
alpha = (time - k1.time) / diff
k1.interp == Linear:             Lerp(k1.value, k2.value, alpha)
k1.interp == Cubic, 비가중:       p1 = p0 + (k1.leaveTangent * diff) * (1/3)
                                 p2 = p3 - (k2.arriveTangent * diff) * (1/3)
                                 BezierCurveValue(p0, p1, p2, p3, alpha)
k1.interp == Cubic, 가중:         WeightedCurveSegment(5.4)
```

- 보간 방식은 앞 키의 것이다. 접선은 앞 키의 출발 접선과 뒤 키의 도착 접선이다.
- `1/3`은 `1.0f / 3.0f`를 한 번 계산한 float이고 곱셈 순서는 `(접선 × diff) × 1/3`이다.
- `BezierCurveValue`는 De Casteljau다(UE `BezierInterp`, `CurveEvaluation.h` 25–36): `p01 = Lerp(p0,p1,α)`, `p12`, `p23`, `p012 = Lerp(p01,p12,α)`, `p123`, 결과 `Lerp(p012,p123,α)`.
- `Lerp`는 Rocks `math::Lerp`다. 비율이 정확히 0·1이면 끝 값을 그대로 내고 (0, 1) 안은 UE `A + α·(B − A)`와 비트가 같다(14절 2 — InterpCurve와 같은 끝점 수정).
- "비가중"은 UE `IsItNotWeighted`와 같다: `(k1.tangentWeight ∈ {None, Arrive}) && (k2.tangentWeight ∈ {None, Leave})`. 둘 중 하나라도 가중 쪽이면 5.4로 간다.
- 같은 수학을 `InterpCurve`는 Hermite 다항식(`CubicInterp`)으로, `TangentCurve`는 Bezier로 계산하므로 두 커브의 비트 결과는 다르다. 각자 UE의 해당 타입과 비트를 맞춘다.

### 5.4 가중 접선 — `math::WeightedCurveSegment`(UE `WeightedEvalForTwoKeys`, CE 93–191)

뉴턴 반복이 아니라 Cardano 닫힌 해다. 가중치는 시간·값 평면에서 키에서 제어점까지의 유클리드 거리다.

```
diff = t2 - t1;  alpha = (time - t1) / diff;  third = 1.0f / 3.0f;  x = t2 - t1
angle = Atan(leave1); SinCos → (s, c)
leaveWeight = k1.tangentWeight ∈ {None, Arrive} ? Sqrt(x*x + (leave1*x)^2) * third : k1.leaveTangentWeight
tan1x = c * leaveWeight + t1;   tan1y = s * leaveWeight + v1
angle = Atan(arrive2); SinCos → (s, c)
arriveWeight = k2.tangentWeight ∈ {None, Leave} ? Sqrt(x*x + (arrive2*x)^2) * third : k2.arriveTangentWeight
tan2x = -c * arriveWeight + t2; tan2y = -s * arriveWeight + v2
n1 = (tan1x - t1) / x;  n2 = (tan2x - t1) / x
멱급수 계수(double): BezierToPower(0, n1, n2, 1) — a = n1, b = n2 - n1, c = 1 - n2, d = b - a
    t³: c - b - d,  t²: 3d,  t¹: 3a,  t⁰: 0  → t⁰ 항에서 alpha를 뺀다
근 = SolveCubic(계수)   // Graphics Gems Cardano, 아래
근이 1개면 그 근, 아니면 [0, 1](허용오차 1e-8 포함) 안의 가장 큰 근, 없으면 0
결과 = BezierCurveValue(v1, tan1y, tan2y, v2, (float)근)
```

`SolveCubic`(CE 10–79): 최고차 계수가 0이면 1e-16으로 나누고, `A,B,C`로 정규화한 뒤 `P = (−A²/3 + B)/3`, `Q = (2A³/27 − AB/3 + C)/2`, `D = Q² + P³`. `|D| <= 1e-8`이면 `|Q| <= 1e-8`일 때 근 0 하나, 아니면 `u = cbrt(−Q)`로 `2u, −u` 둘. `D < 0`이면 세 실근 `φ = acos(−Q/√(−P³))/3`, `T = 2√(−P)`, `T cos φ`, `−T cos(φ + π/3)`, `−T cos(φ − π/3)`. 그 밖은 `cbrt(√D − Q) − cbrt(√D + Q)` 하나. 끝으로 모든 근에서 `A/3`을 뺀다. `cbrt(x)`는 `x > 0 ? pow(x, 1/3) : x < 0 ? −pow(−x, 1/3) : 0`이다.

**UE와 다른 점**: UE는 근이 2개일 때 채워지지 않은 세 번째 칸까지 읽는다(CE 167 — 정의되지 않은 값). Rocks는 실제 근 개수만큼만 본다(14절 3). 근이 1개일 때 [0, 1] 범위를 보지 않는 것은 UE와 같다(결함은 아니고 해가 1개면 그것이 곡선의 유일한 매개변수다).

`Atan`·`SinCos`·`Sqrt`·`pow`·`acos`·`cos`는 결정성 수학(DeterministicMath)의 함수를 쓰지 않고 UE와 같은 표준 수학 함수를 쓴다. 이 경로는 UE와 비트를 맞출 수 있는 범위가 플랫폼 libm에 달려 있다(15절).

### 5.5 범위 밖 — 앞·뒤 외삽(RC 1178–1252)

- 키 1개: 외삽과 무관하게 그 키 값.
- Linear(키 2개 이상): 기울기는 **끝의 두 키 값 차 ÷ 시간 차**다(키 접선을 쓰지 않는다 — Cubic 끝 키여도). 앞쪽 `dt = t1 − t0`, `|dt| <= 1e-8`이면 `v0`, 아니면 `(v1 − v0)/dt × (time − t0) + v0`. 뒤쪽 `dt = t(N−2) − t(N−1)`, 같은 꼴로 `(v(N−2) − v(N−1))/dt × (time − t(N−1)) + v(N−1)`.
- Constant·Cycle·CycleWithOffset·Oscillate: 끝 키 값(순환 계열은 5.1에서 이미 시간을 키 범위로 옮겼다).

### 5.6 특수 입력

- 키 0개: 기본값 또는 fallback. 키가 있으면 기본값은 쓰지 않는다.
- NaN 시간: **UE와 다르다**. UE는 비교가 모두 거짓이라 뒤 외삽 갈래로 가 마지막 키 값(Linear면 NaN)을 낸다. Rocks는 키가 있으면 NaN을 낸다(G01 "잘못된 입력을 조용히 바꾸지 않는다", InterpCurve와 같은 규약 — 14절 4).
- ±Inf 시간: 순환 외삽이면 재배치 없이 끝 키 값, Linear면 ±Inf 또는 NaN(산술 결과 그대로), 그 밖은 끝 키 값.
- 비유한 키 시간은 편집 연산이 InvalidArgument로 거부한다(7절). 비유한 값·접선은 받아서 결과로 전파한다.

### 5.7 `PlainCurve` — `FSimpleCurve::Eval`(SC 645–794)

시간 재배치·외삽은 5.1·5.5와 같다. 구간 선택의 경계만 반대다: 앞 갈래가 `time <= keys[0].time`, 안쪽이 `time < keys[N−1].time`이다(키 시간에서의 결과는 같고 끝 키 시간에서만 갈래가 갈린다). 두 키 사이는 `diff > 0`이고 커브 보간이 Constant가 아니면 `Lerp(v1, v2, (time − t1)/diff)`, 아니면 `v1`이다(UE `FSimpleCurve::EvalForTwoKeys`, SC 493–509 — Constant여도 `time < t2` 비교를 하지 않는다). UE는 Cubic을 ensure 후 무시하고 저장된 Cubic 값은 Linear로 평가하지만, Rocks는 `interp`에 Cubic을 받지 않는다(설정·decode 모두 InvalidArgument — 14절 5).

### 5.8 계단 커브

- `IntegerStepCurve::Evaluate(time, fallback)`(UE `FIntegralCurve::Evaluate`, `IntegralCurve.cpp` 7–40): 기본값(없으면 fallback)으로 시작해, 키가 없거나 `useDefaultBeforeFirstKey`이고 `time < 첫 키`면 기본값, 키 1개이거나 `time < 첫 키`면 첫 키 값, `time < 마지막 키`면 "시간이 time 이하인 마지막 키" 값, 그 밖은 마지막 키 값.
- `StringStepCurve::Evaluate`(UE `FStringCurve::Eval`, `StringCurve.cpp` 57–90): 첫 키 이전은 **늘** 기본값이다(Integer와 다르다 — UE 그대로).
- `NameStepCurve::Evaluate`: UE `FNameCurve`에는 평가 함수가 없다. Rocks는 String과 같은 규칙으로 둔다(D9).
- UE는 "time 이하인 마지막 키"를 선형 탐색으로 찾는다. 결과가 같으므로 Rocks는 이진 탐색이다.

## 6. 접선 자동 계산 — `TangentCurve::AutoSetTangents(float tension = 0)`(RC 680–755)

키마다 다음을 한다(가중치는 건드리지 않는다).

- 첫 키: 키가 2개 이상이고 접선 방식이 Auto면 출발 접선 = 0(보간 방식과 무관).
- 안쪽 키: 보간이 Cubic이고 접선 방식이 Auto면 `ComputeCurveTangent(prev, this, next, tension)`을 도착·출발 접선 둘 다에 넣는다. 아니고 앞 키나 이 키의 보간이 Constant면 출발 접선 = 0, 앞 키가 Cubic이 아니면 도착 접선도 0.
- 마지막 키: 보간이 Cubic이고 Auto면 도착 접선 = 0.
- `ComputeCurveTangent`(UE `InterpCurvePoint.h` 240–301의 clamp 없는 경로) = `((1 − tension) × ((v − v_prev) + (v_next − v))) / max(1e-4, t_next − t_prev)`. clamp(`ClampFloatTangent`)는 `FRichCurve`가 쓰지 않는다.

`SmartAuto`는 UE에서 MovieScene 채널 전용이고 `FRichCurve`는 계산하지 않으므로 두지 않는다(D5). UE는 `AddKey`·`UpdateOrAddKey`·`SetKeyTime` 뒤에는 접선을 다시 계산하지 않고 `DeleteKey`·`SetKeys`·`SetKeyValue` 등 일부 뒤에는 한다. Rocks 값 타입의 편집 연산은 **어느 것도 접선을 몰래 다시 계산하지 않는다** — 자동 접선이 필요한 편집 도구가 편집 묶음 끝에 `AutoSetTangents`를 한 번 부른다(D10). 그래야 같은 편집이 연산 순서와 무관하게 같은 접선을 낸다.

## 7. 키 편집 연산

값 타입의 공개 편집 API는 키 index와 `Result`로 동작한다(키 핸들은 8절의 표가 index 위에 얹는다).

| 연산 | 규칙(UE 대응) |
|---|---|
| `TryAddKey(time, value, unwindRotation = false) -> Result<usize>` | 같은 시간 키가 있으면 그 키들 **앞**에 넣는다(UE `AddKey` — 첫 `keys[i].time >= time` 앞). 새 키는 Linear·Auto·가중 없음. `unwindRotation`이면 이전 키 값 기준 ±180° 안으로 360°씩 옮긴다(도 단위 — UE와 같다, 12절). 비유한 시간은 InvalidArgument |
| `TryUpdateOrAddKey(time, value, unwind, tolerance = 1e-4f)` | `|키 시간 − time| <= tolerance`인 첫 키의 값만 바꾸고(unwind는 새 키에만 — UE와 같다) 없으면 추가 |
| `RemoveKey(index)` | 지운다(접선 재계산 없음 — D10) |
| `TrySetKeyTime(index, time) -> Result<usize>` | 키의 모든 칸을 유지한 채 새 시간 자리로 옮기고 새 index를 돌려준다(같은 시간 키 앞) |
| `SetKeyValue`·`SetKeyInterp`·`SetKeyTangentMode`·`SetKeyTangentWeight`·`SetKeyTangents` | 칸만 바꾼다 |
| `TrySetKeys(Span<const Key>)` | 정렬된 입력만 받는다(정렬 위반은 InvalidArgument — UE는 정렬을 기대만 하고 확인하지 않는다) |
| `FindKey(time, tolerance = 1e-4f) -> usize` | 이진 탐색, 없으면 `IndexNone`. 같은 시간 키가 여럿이면 **가장 앞** 키(UE는 어느 것인지 정해져 있지 않다 — 14절 6) |
| `IsConstant(tolerance = 1e-8f)` | 키 ≤ 1이거나, 모든 키 값이 첫 키와 tolerance 안이고 Cubic 키가 없음(UE와 같다) |
| `TryGetTimeRange() -> Result<Interval<float>>` | 키가 없으면 NotFound(UE는 (0, 0)) |
| `TryGetValueRange()` | 키 값과 Cubic 구간의 극값을 **해석적으로** 구한다(UE는 표본 재귀 `FeaturePointMethod`) — D11 |
| `ShiftTime(delta)`·`ScaleTime(origin, factor)` | 모든 키 시간을 바꾼다(factor > 0만 — 순서 유지, 0·음수는 InvalidArgument). UE는 키마다 지우고 다시 넣는다(O(n²)) |
| `TryBake(interval, first, last)` | `first + interval`부터 `last` 미만까지 간격 `interval`로 Evaluate한 값을 `TryUpdateOrAddKey`로 넣는다. 시간은 누적 덧셈이 아니라 `first + i × interval`로 계산한다(UE는 float 누적 — 14절 7). 이름도 UE `SampleRate`(실제로는 간격)를 `interval`로 바로잡는다 |
| `RemoveRedundantKeys(tolerance, first, last, sampleRate)` | UE `RemoveRedundantKeysInternal`(RC 966–1102)의 알고리즘을 옮기되 범위판의 비교 기준을 시작 키에서 시작한다(UE는 index 0 — 14절 8) |
| `Reset()` | 키만 비운다(기본값·외삽 유지 — UE와 같다) |

`ReadjustTimeRange`(UE 시퀀서 시간 범위 맞춤)는 에셋·도구 층의 일이라 값 타입에 두지 않는다(D12). 계단 커브의 `RemoveRedundantKeys`는 UE와 달리 남은 키 하나를 기본값으로 바꾸지 않는다(평가 결과가 바뀌는 UE 결함 — 14절 9).

## 8. 키 핸들

UE 사실(`KeyHandle.h`·`KeyHandle.cpp`): `FKeyHandle`은 32비트이고 전역 `std::atomic<uint32>` 카운터에서 발급되며 0이 무효다(첫 값 2). 핸들 표는 커브 안의 `UPROPERTY(transient) mutable` 칸이라 디스크에 저장되지 않고 undo 버퍼에만 기록된다. 핸들은 필요할 때 늦게 만들어지고, 키 배열을 직접 고치면 표와 어긋나 개수만 맞춘다. 커브를 복사하면 표가 비고(새 핸들), **대입하면 대상의 옛 표가 그대로 남아** 옛 핸들이 새 커브의 같은 index 키를 가리킨다(KH 74–76 — 결함). `const` 조회가 `mutable` 표를 고쳐 const 동시 조회가 경쟁한다.

Rocks:

- 키 핸들은 **값의 일부가 아니다**. 값 타입(`TangentCurve` 등)은 핸들을 갖지 않고, 복사·비교·직렬화·차이 저장에 나타나지 않는다(D3).
- `CurveKeyHandleTable`은 커브 하나와 짝을 이루는 편집 상태다(편집 도구·undo 기록이 쥔다). 표는 키 index 순서의 핸들 배열 하나와 핸들 → index 찾기 색인을 갖고, 이 표를 거치는 편집 함수(`AddKey`·`RemoveKey`·`SetKeyTime` 등의 표 판)가 커브와 표를 함께 갱신한다. 표를 거치지 않고 커브를 고치면 표의 `Revalidate`가 개수를 맞추는 대신 **표 전체를 무효로 표시**하고 다음 조회가 StaleHandle을 낸다(UE처럼 끝 쪽만 맞춰 다른 키를 가리키게 두지 않는다).
- 핸들 값은 `uint32`이고 표마다 1부터 단조 증가하며 0은 무효(`CurveKeyHandle::Invalid`)다. 전역 카운터를 쓰지 않는다 — 핸들은 그 표 안에서만 뜻이 있고, 표마다 세면 결정적이다(시험·재실행에서 같은 값). 표의 카운터가 `NumericLimits<uint32>::Max()`에 닿으면 새 핸들 발급이 Overflow다(UE는 쿡 중에 0을 건너뛰며 순환).
- 표 복사는 핸들까지 그대로 복사한다(복제된 편집 세션이 같은 키를 같은 핸들로 가리킨다). 대입도 마찬가지로 원본 표로 완전히 바뀐다.
- 표는 저장하지 않는다(UE와 같다). undo 기록은 표의 직렬 형태를 따로 쓴다(CU4 Undo 설계와 함께 정한다 — 20절 열린 문제는 아니고 그 단계의 일).

## 9. 리플렉션·wire·JSON

| B9 기능 | 쓰는 곳 |
|---|---|
| struct 상속(B9k) | `TangentCurve`·`PlainCurve`가 `ScalarCurveBase`를 상속(기본값·외삽 필드 ID는 base 소유) |
| `InstancedStruct<Base>`(B9j·B9k) | 에셋 층의 커브 표 행 `InstancedStruct<ScalarCurveBase>`(UE `UCurveTable`의 `FRealCurve*` 다형 맵 대응 — 13절) |
| Optional(B9e) | `defaultValue`(UE의 `MAX_flt`·`MAX_int32`·빈 문자열 "없음" 표지 대신) |
| Name(B9c) | `NameStepKey::value`·`NameStepCurve::defaultValue` |
| enum underlying(B9a2) | 커브 enum은 `uint8` underlying(UE `TEnumAsByte`와 같은 1바이트) |
| Atomic(B9b) | 키 struct |
| Text·bitfield·WeakRef·ClassRef·Map | 쓰지 않는다 |

- wire는 리플렉션 그대로다: 스칼라 커브는 Record(필드: base의 기본값·외삽 + 키 RecordList), 키는 Record. UE의 수동 이진 키 직렬화(키당 27바이트)는 따르지 않는다(D8).
- JSON은 ReflectionText 규칙 그대로다. 예:

```json
{"defaultValue": null, "preExtrapolation": "Constant", "postExtrapolation": "Cycle",
 "keys": [{"time": 0, "value": 1, "interp": "Cubic", "tangentMode": "Auto", "tangentWeight": "None",
           "arriveTangent": 0, "arriveTangentWeight": 0, "leaveTangent": 0, "leaveTangentWeight": 0}]}
```

- 키 시간 정렬은 불변식이지만 리플렉션 경로(경로 쓰기 `keys[3].time=…`, JSON import, 그래프 로드)는 struct의 의미를 모른다. 그래서 struct 검증 hook이 필요하다: `ROCKS_STRUCT`가 `static Result<void> Validate(const S&) noexcept`를 가지면 MetaCompiler가 `Migrate`처럼 감지해 `RuntimeStructOps::validate`에 잇고, decode 2단계 게시 전·대입 묶음 게시 전·경로 쓰기 게시 전에 부른다(위반 InvalidArgument — 값은 그대로). 이것은 리플렉션 기능 추가라 범위가 커서 D7로 따로 묻는다.
- 같음(대입 묶음의 무동작 판정·차이 저장)은 리플렉션 wire 기준이다. UE `FRichCurveKey::operator==`는 가중치를 비교하지 않고(RC 222–229), `FRichCurve::operator==`는 기본값을 비교하지 않으며(RC 1256–1277), `FStringCurveKey`는 대소문자를 무시한다 — Rocks는 모두 모든 칸·byte로 비교한다(14절 10).

## 10. 직렬화와 버전

- 각 struct는 리플렉션 버전 1에서 시작한다. 하위호환이 없는 초기 단계라 필드 의미를 바꾸면 버전을 올리고 이전 데이터는 버린다(마이그레이션을 쓰지 않는다).
- UE 에셋 파일의 직접 가져오기는 범위 밖이다(Rocks 에셋은 Rocks 형식으로 저작한다). UE 값과의 대조는 시험의 참조 구현으로 한다(15절).
- 키 핸들 표는 저장하지 않는다(8절).

## 11. 압축 커브 `PackedTangentCurve`

UE `FCompressedRichCurve`(RichCurve.h 362–463, RC 1279–2530)의 형식 결정·키 시간 양자화·키 데이터 배치를 따른다. 쿡된 애니메이션 커브처럼 많이 읽는 런타임 데이터용이고 저작 데이터는 `TangentCurve`다.

- 형식 결정(UE `FindRichCurveCompressionFormat`, RC 1279–1330): 키 없음 → Empty, `IsConstant()` → Constant, Cubic이면서 가중 키가 있으면 Weighted, 보간이 섞였으면 Mixed, 그 밖은 첫 키 보간에 따라 Linear·Cubic. 모든 키가 Constant여도 Mixed다(UE와 같다). UE `RCIM_None` 갈래는 없다(D4).
- 키 시간 형식(RC 1332–1475): 기본 오차 1e-4·표본 120 Hz로 16비트 양자화가 오차 안이고 크기가 작으면 Quantized16, 아니면 Float32. 양자화 식 `q = RoundHalfFromZero(Clamp((t − min) × (1/Δ), 0, 1) × 65535)`, 복원 `q × (1/65535) × Δ + min`. 표본 검사에서 UE는 표본마다 키 index를 한 칸만 전진해 촘촘한 키에서 판정이 틀린다(RC 1399–1426) — Rocks는 표본 시간이 들어 있는 구간을 이진 탐색으로 찾는다(14절 11).
- 바이트 배치: 보간 배열(Mixed는 키당 1바이트, Weighted는 키당 보간·가중 2바이트) → 키 시간(Quantized16은 uint16 × N 뒤 4바이트 정렬 후 min·Δ float, Float32는 float × N) → 키 데이터(키별 교차 배치 float, 키당 Linear 1·Cubic 3·Mixed 3·Weighted 5개). 정렬 틈은 0이다. **Rocks는 모든 수를 리틀 엔디언으로 명시해 쓴다**(UE는 원시 메모리 그대로 — 14절 12).
- 리플렉션 표현: `ROCKS_STRUCT` 하나에 `format`·`keyTimeFormat`·앞·뒤 외삽(enum), `keyCount`(uint32), `constantValue`(float — UE의 union 대신 두 칸), `packed`(Array<uint8>)를 둔다. 기본 외삽은 Constant다(UE `FCompressedRichCurve`만 None인 것과 맞추지 않는다 — Rocks에는 None이 없다).
- 평가: 압축 형태를 풀어 키 쌍을 만든 뒤 **5절과 같은 `math::EvalCurveSegment`를 부른다**. 그래서 Float32 시간 형식의 압축 커브는 원본 `TangentCurve`와 비트가 같다(UE는 제어점 곱셈 순서·앞 경계 비교·가중 경로의 오프셋 누락으로 원본과 다를 수 있다 — RC 2077–2105·2061·2114). Quantized16은 복원된 시간으로 평가하므로 그 시간 오차만큼 다르다.
- UE 결함 수정: Weighted 형식의 가중 판정에 키 index 대신 데이터 위치를 넘겨 엉뚱한 바이트를 읽는 것(RC 2077–2105), 가중 경로가 순환 오프셋을 더하지 않고 바로 반환하는 것, 풀기(`PopulateCurve`)가 사용자 접선을 Auto로 바꿔 접선을 다시 계산해 버리는 것과 Empty 형식에서 union을 키 수로 읽어 약 21억 번 도는 것(RC 2441–2452) — 14절 13.
- 풀기 `TryUnpack(TangentCurve&)`는 접선 방식을 User(Weighted는 가중 포함)로 복원하고, 외삽·기본값도 옮긴다(UE는 옮기지 않는다).

## 12. 좌표·단위·각도

- 스칼라 커브 자체에는 단위가 없다. 커브를 묶는 에셋·애니메이션 타입이 UE 5.8 규약을 그대로 따른다(절대 규칙): 벡터 커브는 성분 X·Y·Z(cm 등 UE와 같은 단위), 변환 커브의 회전 성분은 **도 단위 오일러이고 X = Roll, Y = Pitch, Z = Yaw**(UE `FTransformCurve` — `AnimCurveTypes.cpp` 293–299, 평가는 `FRotator(Pitch = Y, Yaw = Z, Roll = X)` → quaternion), 색 커브는 RGBA 4성분이다. Rocks `Rotator`의 필드·적용 순서가 이미 UE와 같다.
- `unwindRotation`(7절)은 도 단위 ±180° 보정이다(UE와 같다, RC 319–334).
- 이 값 타입 층에는 벡터·회전 커브가 없다. 그 묶음 타입은 13절의 에셋 층에서 만든다.

## 13. 에셋 층 경계(범위 밖)

A1 에셋 시스템 뒤 A 단계에서 아래를 만든다. 이 문서는 그것들이 이 층에 기대는 방식만 정한다.

| UE 5.8 | Rocks에서 이 층과의 연결 |
|---|---|
| `UCurveFloat`(`FRichCurve FloatCurve`) | 에셋 객체가 `TangentCurve` 필드 하나를 갖는다 |
| `UCurveVector`·`UCurveLinearColor` | `TangentCurve` 성분 3·4개(12절 규약) |
| `FRuntimeFloatCurve`(인라인 커브 + 외부 에셋 참조) | 인라인 `TangentCurve` + `SoftObjectRef` 또는 `ObjectRef` 하나 — 참조를 고르는 규칙은 에셋 층이 정한다 |
| `UCurveTable`(`TMap<FName, FRealCurve*>`, Simple·Rich 모드) | `OrderedMap<Name, InstancedStruct<ScalarCurveBase>>` — 평가는 담긴 타입(`TangentCurve`·`PlainCurve`)으로 고르는 자유 함수 `EvaluateScalarCurve`(virtual 없음) |
| `FCurveTableRowHandle` | 표 참조 + 행 Name |
| `FVectorCurve`·`FTransformCurve`(애니메이션) | 애니메이션 단계 |

## 14. UE 5.8과 다른 점

기본은 UE와 같은 의미·같은 비트다. 아래는 결함 수정(★)이거나 Rocks 규약(◇)이다.

1. ★ 순환 외삽에서 키 시간이 모두 같거나(길이 0) 시간이 ±Inf면 UE는 0 나눗셈·Inf를 정수로 바꾸는 정의되지 않은 동작이다(`RealCurve.cpp` 7–34). Rocks는 재배치하지 않는다.
2. ★ Linear·Bezier의 `Lerp` 끝점: 비율 0·1에서 끝 값을 정확히 낸다(InterpCurve 4절 5와 같은 수정). (0, 1) 안은 비트가 같다.
3. ★ 가중 접선 3차 방정식에서 근이 2개일 때 초기화되지 않은 세 번째 칸을 읽지 않는다(CE 167).
4. ◇ NaN 시간은 NaN을 낸다(UE는 마지막 키 값). 빈 커브는 NaN이어도 기본값이다.
5. ◇ `PlainCurve`는 Cubic을 받지 않는다(UE는 ensure 후 저장값을 Linear로 평가).
6. ◇ `FindKey`는 같은 시간 키 중 가장 앞 키를 돌려준다(UE는 정해지지 않음).
7. ★ Bake 시간은 누적 덧셈 대신 `first + i × interval`(UE float 누적 오차와 `Reserve` 음수 인자).
8. ★ 범위판 `RemoveRedundantKeys`의 비교 기준을 시작 키로 둔다(UE `MostRecentKeepKeyIndex = 0`, RC 1017).
9. ★ 계단 커브 `RemoveRedundantKeys`가 마지막 남은 키를 기본값으로 바꾸지 않는다(UE는 첫 키 이전 평가 결과가 바뀐다 — `StringCurve.cpp` 232–237, `IntegralCurve.cpp` 251–256).
10. ◇ 같음은 모든 칸·byte 비교다(UE는 가중치·기본값을 빼고, String 키는 대소문자를 무시한다).
11. ★ 압축 키 시간 판정의 표본 구간 탐색(RC 1399–1426).
12. ◇ 압축 바이트는 리틀 엔디언 명시(UE 원시 메모리).
13. ★ 압축 Weighted 평가의 가중 판정 위치·순환 오프셋 누락, 풀기의 접선 손실·Empty 무한 반복(RC 2077–2105, 2441–2452).
14. ◇ 키 핸들은 값 밖의 표이고 표마다 결정적으로 발급한다(UE 전역 atomic). 표 대입이 옛 핸들을 남기지 않는다(KH 74–76).
15. ◇ 숨김 열거자(`RCIM_None`·`RCTM_None`·`RCCE_None`)와 `SmartAuto`가 없다. UE `RCCE_None`은 실제로 Cycle처럼 동작한다(RC 1115·1139).
16. ◇ 편집 연산이 접선을 몰래 다시 계산하지 않는다(6절).
17. ◇ 기본값 "없음"은 Optional(UE `MAX_flt`·`MAX_int32`·빈 문자열).
18. ◇ 값 범위는 해석적 극값(UE 표본 재귀).

## 15. 시험 계획

`Core/Source/Runtime/Core/Tests/MathKeyCurve*Tests.cpp`(수학 층)와 `Core/Source/Runtime/Curves/Tests/`(값 타입). 무작위 사례는 고정 seed이고 타이밍에 기대는 시험은 없다.

- **UE 식 참조 구현 대조(golden)**: 시험 안에 UE 5.8 식을 그대로 옮긴 참조(`RemapTimeValue`·`CycleTime`·`Eval`·`EvalForTwoKeys`·`BezierInterp`·`WeightedEvalForTwoKeys`·`SolveCubic`·`AutoSetTangents`·`ComputeCurveTangent`, `FSimpleCurve::Eval`, Integral·String 평가, 압축 형식·양자화)를 두고 14절의 수정을 하나씩 켜고 끌 수 있게 한다.
  - Rocks 의미(수정을 켠 참조): 모든 커브·모든 입력에서 **비트 단위로 같다**.
  - UE 그대로(수정을 끈 참조): 비율이 0·1이 아닌 입력, 길이가 0이 아닌 구간, 근이 2개가 아닌 가중 구간, 유한 입력에서 비트 단위로 같다 — 이것이 UE 비트 대조의 실제 범위다. 가중 경로는 libm(`atan`·`sin`·`cos`·`pow`·`acos`)이 같은 플랫폼 안에서만 비트 대조한다.
  - 반례로 UE 결함을 확인한다: 길이 0 순환, 근 2개 가중, 압축 Weighted의 구간 1 이상, 범위판 중복 제거, 계단 커브의 마지막 키 기본값 변환.
- **경계**: 빈 커브(기본값 있음·없음), 키 하나(모든 외삽), 같은 시간 키(삽입 위치·평가 구간·FindKey), 키 시간 정확히·끝 키 시간·바로 앞뒤, 외삽 6가지 조합(앞·뒤 × 5종)과 순환 횟수 경계(정확히 n 주기), CycleWithOffset 누적, Oscillate 짝·홀수, Linear 외삽의 두 키 기울기·`|dt| <= 1e-8`, NaN·±Inf.
- **보간·접선**: Constant 구간의 `time < t2` 비교, Linear, Cubic 비가중 = 독립 double Bezier 참조와 상대 오차, 가중 4방식 × 좌우 조합, 자동 접선(tension 0·0.5·1, Constant 이웃, 첫·끝 키), User·Break 보존.
- **편집**: 삽입 위치, unwind ±180° 경계, UpdateOrAdd 허용오차, SetKeyTime 칸 유지, SetKeys 정렬 거부, Shift·Scale 순서 유지·factor 거부, Bake 시간 식, RemoveRedundantKeys(표본 있음·없음·범위판), 할당 실패 주입 뒤 커브 불변.
- **키 핸들 표**: 발급 단조·0 무효·Overflow, 삽입·삭제·이동 뒤 핸들 → index, 표를 거치지 않은 편집 뒤 StaleHandle, 표 복사·대입이 같은 핸들.
- **리플렉션**: 각 struct 그래프 왕복과 다시 저장 byte 같음, 상속 필드 ID(base 소유), `InstancedStruct<ScalarCurveBase>`에 두 파생 담기·무관 struct 거부, 키 Atomic 차이 단위, JSON 왕복, (D7 채택 시) 정렬 위반 decode·경로 쓰기·JSON import 거부.
- **압축**: 형식 결정 표, 양자화 경계(오차 직전·직후), 키 데이터 배치 golden byte, Float32 압축 평가 = 원본 비트 같음, Quantized16 오차 ≤ threshold, 풀기 왕복(접선·외삽·기본값 보존).

## 16. 구현 단계

단계 기호는 CV(CurVe)다. 리플렉션 확장 문서의 CU(UI·Undo 소비자 단계)와 겹치지 않게 했다.

| 단계 | 범위 | 선행 | 규모 |
|---|---|---|---|
| CV1 | Core 수학 층 `KeyCurveMath.h`: `CycleCurveTime`·`EvalCurveSegment`·`BezierCurveValue`·`WeightedCurveSegment`(Cardano)·`ComputeCurveTangent`, UE 식 참조 구현과 golden 시험 | — | M |
| CV2 | 리플렉션 struct 검증 hook(D7 채택 시 — MetaCompiler 감지·`RuntimeStructOps::validate`·decode·대입 묶음·경로 쓰기 호출, RMC 진단) | — | M |
| CV3 | `Curves` 모듈, `ScalarCurveBase`·`TangentCurve`·`PlainCurve`와 키, 평가·자동 접선·편집 연산·값 범위, 리플렉션 왕복·JSON 시험 | CV1(CV2) | L |
| CV4 | 계단 커브 셋(Integer·Name·String) | CV3 | S |
| CV5 | `CurveKeyHandleTable`과 표 판 편집 함수 | CV3 | S |
| CV6 | `PackedTangentCurve` 형식 결정·양자화·배치·평가·풀기, golden byte 시험 | CV3 | M |
| CV7 | 문서 반영(에셋 시스템 Curve 행·Core 상세 G09·구현 문서), 교차검증 | CV1–CV6 | S |

## 17. 결정 목록

권장안으로 확정하는 것이 전제다. ▲는 원칙 충돌이나 범위가 커서 따로 확인이 필요한 항목이다.

- **D1 시간·값 정밀도** — float(UE와 같음). double 커브는 소비자가 생길 때 템플릿으로 연다.
- **D2 다형성** — virtual 기반 없이 상속 + 자유 함수. 공통 칸은 리플렉션 base `ScalarCurveBase`, 다형 저장은 `InstancedStruct<ScalarCurveBase>`, 평가 분기는 담긴 타입으로 고르는 자유 함수.
- **D3 키 핸들** — 값 밖의 편집 표(`CurveKeyHandleTable`), 표마다 1부터 단조 uint32·0 무효·Overflow 거부, 저장 안 함, 복사·대입은 핸들 그대로.
- **D4 숨김 열거자** — `RCIM_None`·`RCTM_None`·`RCCE_None`을 두지 않는다(번호 3·3·5는 비워 둔다).
- **D5 SmartAuto** — 두지 않는다(UE `FRichCurve`가 계산하지 않는 MovieScene 전용 모드). 시퀀서 채널을 만들 때 그 쪽에서 연다.
- **D6 "없음" 기본값** — `Optional<T>`(UE sentinel 대신).
- **D7 ▲ 정렬 불변식과 struct 검증 hook** — 리플렉션에 `ROCKS_STRUCT`의 `Validate` hook을 더해 decode·대입 묶음·경로 쓰기 게시 전에 정렬 위반을 InvalidArgument로 거부한다. 리플렉션 기능 추가라 범위가 크다(CV2). 대안은 평가 전에 정렬을 확인해 위반이면 기본값을 내는 것이지만 잘못된 데이터를 조용히 받아들이므로 권하지 않는다.
- **D8 wire 크기** — 저작 데이터는 리플렉션 그대로(키 Record의 RecordList), 크기가 중요한 런타임 데이터는 `PackedTangentCurve`. 별도 수동 이진 키 codec은 두지 않는다.
- **D9 Name 계단 커브 평가** — UE에는 없지만 String과 같은 규칙으로 둔다(첫 키 이전은 기본값).
- **D10 접선 재계산 시점** — 편집 연산은 몰래 다시 계산하지 않고 `AutoSetTangents`를 명시적으로 부른다.
- **D11 값 범위** — Cubic 구간 극값을 해석적으로(Bezier 도함수 2차 방정식의 근) 구한다. 가중 구간은 같은 방식으로 매개변수 공간에서 구한다. UE 표본 재귀와 결과가 다를 수 있다(더 정확하다).
- **D12 ReadjustTimeRange** — 값 타입에 두지 않는다(시퀀서·도구 층의 시간 범위 맞춤 기능).
- **D13 압축 바이트 순서** — 리틀 엔디언 명시, union 없이 두 칸.
- **D14 압축 평가 식** — 원본과 같은 핵심 식을 써서 Float32 압축이 원본과 비트 같게 한다(UE 압축 평가의 곱셈 순서를 따르지 않는다).
- **D15 모듈** — 수학 핵심은 Core, 값 타입은 새 Core 런타임 모듈 `Curves`, Engine 층에는 두지 않는다.

## 18. 확인하지 못한 것

- UE `RCTM_SmartAuto`의 세부 식(시퀀서 CVar별 분기) — D5로 범위 밖이라 확인하지 않았다.
- UE `FRichCurveKey`의 실제 메모리 크기(28바이트로 보이지만 static_assert로 확인하지 않았다) — Rocks 설계에 영향 없음.
- 가중 경로의 UE 비트 대조가 실제로 어느 플랫폼까지 맞는지 — libm 구현에 달려 있어 CV1 시험에서 확인한다.
- UE 압축 Weighted 결함(11절)이 UE 쿡 데이터에서 실제로 드러나는지(애니메이션 코덱이 Weighted를 고르는 빈도) — Rocks 설계는 결함을 고치는 쪽이라 확인하지 않아도 된다.
