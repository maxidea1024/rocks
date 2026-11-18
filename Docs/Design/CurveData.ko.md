# 커브 데이터 계층 설계

개정 12(CV10 — Curve 단계 CV1–CV10 완료, 다른 문서 반영·남은 일 17.9절), 기준일 2026-11-18. 이 문서는 Curve 단계(리플렉션 B9 직후, A0-G 앞 — 2026-11-15 사용자 결정)의 **값 타입 층** 설계다. 근거는 [에셋 시스템](AssetSystem.ko.md) 1절 Curve 행, [Core 상세 명세](CoreDetailedSpec.ko.md) G09, 이미 구현된 수학 층 [키 커브](../Implementation/InterpCurve.ko.md)(`InterpCurve<T>`)다. UE 대응 대상은 UE 5.8.4의 `Engine/Source/Runtime/Engine/Classes/Curves`(`FRealCurve`·`FRichCurve`·`FSimpleCurve`·`FIndexedCurve`·`FKeyHandle`·`FKeyHandleMap`·`FIntegralCurve`·`FNameCurve`·`FStringCurve`·`FCompressedRichCurve`)와 평가 함수 `Public/Curves/CurveEvaluation.h`(`UE::Curves::EvalForTwoKeys`·`BezierInterp`·`WeightedEvalForTwoKeys`), 그리고 G09가 이 단계로 미룬 `FInterpCurve<T>`의 리플렉션·직렬화·`CalcBounds`(12절)다. 아래에서 `RC` = `Engine/Source/Runtime/Engine/Private/Curves/RichCurve.cpp`, `CE` = 같은 폴더 `CurveEvaluation.cpp`, `SC` = `SimpleCurve.cpp`, `KH` = `Classes/Curves/KeyHandle.h`, `UMU` = `Core/Public/Math/UnrealMathUtility.h`, `UPM` = `Core/Public/Math/UnrealPlatformMathSSE.h`다. 레거시·deprecated 경로(옛 버전 키 직렬화 보정, `UE_DEPRECATED` 함수, `FInterpCurvePoint` → `FRichCurveKey` 변환 생성자)는 대응하지 않는다.

에셋 층(`UCurveFloat`·`UCurveVector`·`UCurveLinearColor`·`UCurveTable`·`FRuntimeFloatCurve`·`FCurveTableRowHandle`)은 A1 에셋 시스템 뒤 A 단계의 일이라 범위 밖이다. 이 문서는 그 층이 기댈 경계(14절)만 정한다.

## 1. 요약

- 만드는 것은 스칼라 키 커브 둘(접선 있는 커브, 접선 없는 커브), 계단 커브 셋(정수·Name·String), 압축된 접선 커브 하나, 편집기·도구가 쓰는 키 핸들 표, 그리고 이미 있는 `InterpCurve<T>`의 리플렉션 미러와 `TryCalcBounds`다.
- 비트 기준은 **Windows x64에서 UE와 같은 비트**다(D17). 평가 식은 UE `FRichCurve::Eval`과 같은 식을 같은 연산 순서·같은 정밀도(float, 그리고 UE가 double을 섞는 곳은 double)로 옮긴다. UE `FMath::SinCos(float)` 다항식은 그대로 옮긴다. `atanf`와 double libm 함수는 UE와 같은 함수를 부르므로 플랫폼 간 비트 결정성은 UE와 같은 수준까지만 보장한다. 다르게 정한 곳은 UE 결함이나 Rocks 규약 때문인 곳뿐이고 15절에 모두 적는다.
- 값 타입은 처음부터 리플렉션 struct(`ROCKS_STRUCT`)로 만든다. B9에서 연 struct 상속(B9k)·Optional(B9e)·Name(B9c)·enum underlying(B9a2)·Atomic(B9b)을 쓴다. 새 값 타입에는 미러(`Binds`)를 쓰지 않는다. `InterpCurve<T>`는 Core 타입이라 미러로 리플렉션한다(12절).
- 리플렉션 기반을 셋 보강한다: struct 검증 hook(D7), `Array<uint8>`의 Bytes 블록 wire(D18), 미러의 private 멤버 접근(D19). 셋 다 [리플렉션 보강](ReflectionExtensions.ko.md)에 넣을 변경이다.
- 평가 핵심 식(두 키 사이 보간, Bezier, 가중 접선 3차 방정식, 시간 순환)은 Core 수학 층(`Core/Math/KeyCurveMath.h`)에 둔다. 리플렉션 값 타입은 새 Core 런타임 모듈 `Curves`에 둔다. 프레임 단위 시간(`FrameRate`·`FrameNumber`·`FrameTime`)은 Core에 최소 타입으로 둔다(D20). Engine 층에는 아무것도 두지 않는다.
- 키 배열은 private이고 정렬 불변식은 C++ API와 검증 hook이 함께 지킨다. 키 핸들은 값의 일부가 아니다. 값 밖의 편집용 표(`CurveKeyHandleTable`)가 따로 쥐고, 값 타입의 공개 API는 키 index로 동작한다.

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
| `FNameCurve` | `NameStepCurve` | Name 값 계단 커브(UE에 없는 기본값·평가를 더한다 — D9) |
| `FNameCurveKey` | `NameStepKey` | 시간·Name 값 |
| `FStringCurve` | `StringStepCurve` | String 값 계단 커브 |
| `FStringCurveKey` | `StringStepKey` | 시간·String 값 |
| `FCompressedRichCurve` | `PackedTangentCurve` | `TangentCurve`의 압축 저장 형태(쿡 데이터용) |
| `FKeyHandle` | `CurveKeyHandle` | 키 하나를 가리키는 편집용 핸들 |
| `FKeyHandleMap` | `CurveKeyHandleTable` | 키 index ↔ 핸들 표(값 밖의 편집 상태) |
| `ERichCurveInterpMode` | `CurveInterp` | Linear 0·Constant 1·Cubic 2 |
| `FSimpleCurve::InterpMode`의 허용 값 | `PlainCurveInterp` | Linear 0·Constant 1(Cubic을 표현할 수 없게 따로 둔다 — D7) |
| `ERichCurveTangentMode` | `CurveTangentMode` | Auto 0·User 1·Break 2 |
| `ERichCurveTangentWeightMode` | `CurveTangentWeight` | None 0·Arrive 1·Leave 2·Both 3 |
| `ERichCurveExtrapolation` | `CurveExtrapolation` | Cycle 0·CycleWithOffset 1·Oscillate 2·Linear 3·Constant 4 |
| `ERichCurveCompressionFormat` | `PackedCurveFormat` | Empty 0·Constant 1·Linear 2·Cubic 3·Mixed 4·Weighted 5 |
| `ERichCurveKeyTimeCompressionFormat` | `PackedKeyTimeFormat` | Quantized16 0·Float32 1 |
| `UE::Curves::EvalForTwoKeys`·`BezierInterp`·`WeightedEvalForTwoKeys`·`SolveCubic`·`FRealCurve::CycleTime`·`FMath::SinCos(float)` | `math::EvalCurveSegment`·`math::BezierCurveValue`·`math::WeightedCurveSegment`·`math::SolveCurveCubic`·`math::CycleCurveTime`·`math::CurveSinCos` | Core 수학 층 핵심 식 |
| `FFrameRate`·`FFrameNumber`·`FFrameTime`(`Core/Public/Misc/FrameRate.h`·`FrameNumber.h`·`FrameTime.h`) | `FrameRate`·`FrameNumber`·`FrameTime` | 프레임 단위 시간의 최소 타입(업계 공용어라 이름을 그대로 둔다 — D20) |
| `FInterpCurveFloat`·`FInterpCurveVector2D`·`FInterpCurveVector`·`FInterpCurveQuat`·`FInterpCurveLinearColor`와 각 `FInterpCurvePoint*` | `InterpCurve<float>`·`InterpCurve<Vec2d>`·`InterpCurve<Vec3d>`·`InterpCurve<Quatd>`·`InterpCurve<LinearColor>`와 각 `InterpCurvePoint<T>`의 미러 | 이미 있는 Core 타입의 리플렉션(12절, D19) |
| `FInterpCurve::CalcBounds` | `InterpCurve<T>::TryCalcBounds` | 값 범위(12절) |

enum 번호는 UE와 같게 둔다(golden 시험의 비교를 단순하게 하려는 것이고 UE 파일과의 호환 목적은 아니다). UE의 `RCIM_None`·`RCTM_None`(둘 다 `UMETA(Hidden)`), `RCCE_None`(숨김이 아니라 표시 이름 "None"인 보이는 열거자이고 동작은 Cycle과 같다 — RC 1115·1139), `RCTM_SmartAuto`는 두지 않는다(D4·D5).

## 3. 모듈 배치

- **Core 수학 층** — `Core/Source/Runtime/Core/Public/Core/Math/KeyCurveMath.h`: 키 쌍과 시간을 받아 값을 내는 순수 함수(5절), 시간 순환(5.1절), 자동 접선 식(6절), UE `SinCos` 다항식, 가중 접선의 3차 방정식 풀이. 할당·리플렉션 의존이 없다. 3차 방정식은 이미 있는 `PolynomialRoots.h`와 목적이 다르다(UE Cardano 닫힌 해와 비트를 맞춘다 — 5.4절). 자동 접선의 시간 차 하한은 이미 있는 `math::CurveTangentTimeDifference`(`InterpCurveMath.h`)를 그대로 쓴다(같은 기능을 새로 만들지 않는다).
- **프레임 시간** — `Core/Source/Runtime/Core/Public/Core/Time/FrameRate.h`(D20, 7.2절).
- **커브 값 타입** — 새 모듈 `Core/Source/Runtime/Curves`(의존: Core·ObjectRuntime). 여기에 2절의 `ROCKS_STRUCT`들과 편집 연산·압축·키 핸들 표가 산다. ObjectRuntime에 넣지 않는 것은 그 모듈이 객체·리플렉션 기반이고 커브는 그 위의 데이터라서다. 설정(`Config`)·추적 설정(`TraceControl`)이 이미 자기 모듈에 `ROCKS_STRUCT`를 두는 선례가 있다.
- **InterpCurve 미러** — 기존 `CoreTypeBindings`(`CoreValueTypes.h`와 같은 모듈의 새 헤더 `CoreCurveTypes.h`).
- **Engine 층** — 없음. 커브를 소비하는 에셋 타입(14절)과 애니메이션·머티리얼·게임 코드는 각자 이 모듈을 의존한다.
- 편집기 전용 키 핸들 표(`CurveKeyHandleTable`)도 `Curves` 모듈에 둔다. 런타임 평가 경로에서 쓰지 않을 뿐 편집 도구·undo가 모두 쓰므로 별 모듈로 나눌 만큼 크지 않다(D3).

## 4. 데이터 정의

시간과 값은 모두 **float**다(UE `FRichCurveKey::Time`·`Value`가 float — 비트 대조의 전제, D1). 기본값·"없음" 표지는 sentinel 대신 `Optional`이다(D6). 키 배열은 private PROPERTY이고 C++ API는 접근자로만 읽는다(D7). 도구·JSON 이름은 `Name="keys"`로 둔다(필드 `Name` 지정자 — ReflectionFoundations 136행, Level `actors_`와 같은 방식).

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
    ROCKS_GENERATED_BODY()
public:
    Span<const TangentCurveKey> Keys() const noexcept;
    // 편집 연산은 7절. 검증 hook은 9.1절(정렬·유한 시간).
    static Result<void> Validate(const TangentCurve& curve) noexcept;
private:
    ROCKS_PROPERTY(Name = "keys") Array<TangentCurveKey> keys_;   // 시간 오름차순·유한(D7)
};

ROCKS_STRUCT(Atomic)
struct PlainCurveKey { ROCKS_PROPERTY() float time = 0; ROCKS_PROPERTY() float value = 0; };

ROCKS_STRUCT()
struct PlainCurve : ScalarCurveBase {
    ROCKS_GENERATED_BODY()
public:
    ROCKS_PROPERTY() PlainCurveInterp interp = PlainCurveInterp::Linear;   // Cubic은 표현할 수 없다
    Span<const PlainCurveKey> Keys() const noexcept;
    static Result<void> Validate(const PlainCurve& curve) noexcept;
private:
    ROCKS_PROPERTY(Name = "keys") Array<PlainCurveKey> keys_;
};

ROCKS_STRUCT()
struct IntegerStepCurve {
    ROCKS_GENERATED_BODY()
public:
    ROCKS_PROPERTY() Optional<int32> defaultValue;
    ROCKS_PROPERTY() bool useDefaultBeforeFirstKey = false;   // UE bUseDefaultValueBeforeFirstKey
    static Result<void> Validate(const IntegerStepCurve& curve) noexcept;
private:
    ROCKS_PROPERTY(Name = "keys") Array<IntegerStepKey> keys_;
};
// StringStepCurve: defaultValue는 Optional<String>, useDefaultBeforeFirstKey 칸이 없다(UE FStringCurve도 없다).
// NameStepCurve: defaultValue는 Optional<Name>, useDefaultBeforeFirstKey 칸이 없다. UE FNameCurve에는
// 기본값 칸 자체가 없다 — Rocks가 더한다(D9).
```

- 키 struct는 `Atomic`이다. 키의 칸은 서로 맞물려 있어(접선은 보간 방식, 가중치는 가중 방식에 따라 뜻이 바뀐다) 통째 단위가 맞다. 다만 Array 원소의 대입·override는 지금 컨테이너 통째 단위라(B9i "컨테이너 교체") 실제 효과는 키 struct를 단독 필드로 쓸 때와 앞으로 원소 단위 차이를 열 때에 한정된다.
- 값 타입은 이동 가능하고 복사는 `TryCopyFrom`(할당 실패를 Result로)이다. 동기화는 없다. 바뀌지 않는 동안의 const 평가는 여러 스레드가 함께 해도 된다(UE와 달리 const 함수가 내부 상태를 고치지 않는다 — 핸들 표가 값 밖에 있으므로).
- C++ 쪽 불변식은 API가 지키고(키 배열이 private이라 정렬을 깨는 직접 수정이 없다 — `InterpCurve<T>`와 같은 선택, InterpCurve.ko.md 2절), 리플렉션 쪽(decode·JSON·경로 쓰기·대입 묶음)은 검증 hook이 지킨다(9.1절).
- 키 배열의 wire 크기는 9.2절(D8).

## 5. 평가 의미

`TangentCurve::Evaluate(float time, float fallback = 0) const`가 UE `FRichCurve::Eval(InTime, InDefaultValue)`다. 아래 식의 연산 순서와 정밀도를 그대로 지킨다(`-ffp-contract=off` — InterpCurve와 같은 빌드 규칙). 기본은 float이고, UE가 double을 섞는 곳(5.4절 가중 접선의 시간 칸, 3차 방정식)은 같은 자리에서 double이다.

### 5.1 시간 재배치(외삽 순환) — UE `RemapTimeValue`(RC 1104–1161)·`CycleTime`(`RealCurve.cpp` 7–34), D16

키가 2개 미만이면 하지 않는다. `time <= keys[0].time`이고 앞 외삽이 Cycle·CycleWithOffset·Oscillate면(Linear·Constant가 아니면), 그렇지 않고 `time >= keys[N−1].time`이고 뒤 외삽이 그 셋이면 아래를 한다(경계 포함 비교와 앞 갈래 우선은 UE와 같다).

```
min = keys[0].time; max = keys[N-1].time; duration = max - min          (float)
duration <= 0·비유한 또는 time 비유한(±Inf·NaN): 재배치하지 않는다(15절 1, 17.1절 K3)
time > max: ratio = (max - time) / duration      (float, UE와 같은 식)
time < min: ratio = (time - min) / duration
|ratio| < 2^30 → cycles = math::TryFloorToInt32((double)ratio)(int32) — UE와 같은 경로:
    time > max: time = time + duration * (float)cycles
    time < min: time = time - duration * (float)cycles
    if time == max && 원래 time < min: time = min
    if time == min && 원래 time > max: time = max
    cycles = |cycles|
    (곱·합의 float 반올림으로 time이 키 범위 끝을 벗어날 수 있다 — |ratio|가 클수록 크고 약 2^22를
     넘으면 한 주기 길이에 이를 수 있다, UE와 같다. Evaluate는 그 time을 떨어진 쪽의 외삽 갈래로
     받는다 — 그쪽이 Linear 외삽이면 값이 크게 달라질 수 있고, UE와 비트가 같게 자르지 않는다)
|ratio| ≥ 2^30 → double 재배치(D16, 끝 바로잡기 없음):
    d = (double)원래 time - (double)min; r = Fmod(d, (double)duration)(정확); q = Floor((d - r) / (double)duration + 0.5)
    r < 0이면 r += duration, q -= 1   (r ≠ 0이면 |r| > duration·2^-24라 r + duration이 duration으로 반올림되지 않는다 — 17.1절 K3)
    time < min: time = (float)((double)min + r), cycles = |q|
    time > max: r == 0이면 time = max, cycles = q - 1(UE가 정확한 배수에서 max로 가는 것과 같게), 아니면 time = (float)((double)min + r), cycles = q
    cycles는 double로 둔다
CycleWithOffset: 앞쪽 offset = (keys[0].value - keys[N-1].value) * cycles, 뒤쪽은 (keys[N-1].value - keys[0].value) * cycles
                 (int32 경로는 float 곱 — UE와 같다. double 경로는 double 곱 뒤 float로 반올림)
Oscillate: cycles가 홀수면 time = min + (max - time)   (double 경로의 홀짝은 Fmod(cycles, 2))
```

- Rocks `math::FloorToInt32`는 범위 밖·NaN에서 Fatal이므로(MathUtility.h 543–545) 이 경로에서 쓰지 않는다. 범위 판정은 double 판 `math::TryFloorToInt32`(MathUtility.h 585)다. float 몫을 double로 넓히는 것은 정확하므로 범위 안에서는 floor 결과가 같다. **Fatal로 가는 경로가 없다.**
- UE의 Windows x64 `FMath::FloorToInt(float)`는 일반식(`TruncToInt32` 뒤 보정)이 아니라 SSE `_mm_cvt_ss2si(F + F − 0.5f) >> 1`이다(UPM 86–91, `MicrosoftPlatformMath.h`가 `TUnrealPlatformMathSSE4Base`로 상속). 이 식은 |F| < 2^30에서 floor와 같고 그 밖에서는 `2F`가 int32를 넘어 무의미한 값(대개 −2^30)을 낸다. 그래서 UE와 비트가 같은 범위는 |ratio| < 2^30이고 Rocks는 그 밖을 모두 double 재배치로 보낸다(int32에 들어가는 [2^30, 2^31)도 — int32 경로의 float 식은 그 크기에서 범위 밖 시간을 내고 정확한 배수의 주기 수가 double 규칙과 어긋난다, 15절 1).

### 5.2 구간 선택 — RC 1163–1253

```
value = defaultValue가 있으면 그 값, 없으면 fallback
N == 0:                         value
N == 1 또는 time < keys[0].time: 앞 외삽(5.5)
time <= keys[N-1].time:          k = upper_bound(keys[1..N-2], time) — 첫 keys[k].time > time인 k(없으면 N-1)
                                 value = EvalCurveSegment(keys[k-1], keys[k], time)
그 밖:                           뒤 외삽(5.5)
결과 = value + offset(5.1)
```

upper_bound는 UE와 같은 이진 탐색(`first = 1, count = N − 2`, `time >= keys[middle].time`이면 오른쪽)이다. 그래서 키 시간에 정확히 맞으면 그 키에서 시작하는 구간(비율 0)이고, 마지막 키 시간이면 마지막 구간의 비율 1이다. 같은 시간 키가 여럿이면 그 시간의 **마지막** 키에서 시작하는 구간을 쓴다. 단 마지막 키 시간이 중복이면 탐색 범위가 N−2까지라 구간은 (N−2, N−1)이고 길이 0이라 결과는 `keys[N−2].value`다(그 시간보다 조금 뒤는 뒤 외삽 `keys[N−1].value` — UE와 같은 불연속).

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
- `1/3`은 `1.0f / 3.0f`를 한 번 계산한 float이고 곱셈 순서는 `(접선 × diff) × 1/3`이다(CE 226–227).
- `BezierCurveValue`는 De Casteljau다(UE `BezierInterp`, `CurveEvaluation.h` 25–36): `p01 = Lerp(p0,p1,α)`, `p12`, `p23`, `p012 = Lerp(p01,p12,α)`, `p123`, 결과 `Lerp(p012,p123,α)`. α는 float다.
- `Lerp`는 Rocks `math::Lerp`다(MathUtility.h 116–136). 비율이 정확히 0·1이면 끝 값을 그대로 낸다. (0, 1) 안은 UE `A + α·(B − A)`(UMU 1123–1126)와 비트가 같다. 예외는 유한한 두 끝의 차가 넘치는 경우로, UE는 Inf를 내고 Rocks는 `a(1 − t) + bt`로 유한한 값을 낸다(15절 2).
- "비가중"은 UE `IsItNotWeighted`(CE 193–197)와 같다: `(k1.tangentWeight ∈ {None, Arrive}) && (k2.tangentWeight ∈ {None, Leave})`. 둘 중 하나라도 가중 쪽이면 5.4로 간다. 이 판정은 키의 보간 방식과 무관하게 구간 단위다 — 뒤 키가 Linear여도 도착 가중이 있으면 그 Cubic 구간은 가중이다(11절 형식 결정이 이것을 따른다).
- 같은 수학을 `InterpCurve`는 Hermite 다항식(`CubicInterp`)으로, `TangentCurve`는 Bezier로 계산하므로 두 커브의 비트 결과는 다르다. 각자 UE의 해당 타입과 비트를 맞춘다.

### 5.4 가중 접선 — `math::WeightedCurveSegment`(UE `WeightedEvalForTwoKeys`, CE 93–191)

뉴턴 반복이 아니라 Cardano 닫힌 해다. 가중치는 시간·값 평면에서 키에서 제어점까지의 유클리드 거리다. 형식 표기: `(f)`는 float 저장, `(d)`는 double 계산이다. 키 시간 둘은 double로 넓혀 쓰므로(CE 101–102) 시간 칸의 덧셈·뺄셈은 double로 계산한 뒤 float로 저장한다. 이 식들은 모두 float 값 둘의 연산 한 번이라 double 결과를 float로 반올림한 값이 float 연산 결과와 늘 같다(double 53비트 ≥ 2·24 + 2 — 이중 반올림이 무해한 조건). Rocks는 UE와 줄 단위로 대조하기 쉽게 double 꼴을 그대로 둔다(17.1절 K5).

```
diff(f) = t2 - t1;  alpha(f) = (time - t1) / diff;  third(f) = 1.0f / 3.0f
T1(d) = t1;  T2(d) = t2
x(f) = (float)(T2 - T1)
angle(f) = atanf(leave1);  (s, c) = CurveSinCos(angle)            // UE SinCos 다항식(아래)
leaveWeight(f) = k1.tangentWeight ∈ {None, Arrive} ? Sqrt(x*x + y*y) * third (y(f) = leave1 * x)
                                                    : k1.leaveTangentWeight
tan1x(f) = (float)((double)(c * leaveWeight) + T1);   tan1y(f) = s * leaveWeight + v1
angle(f) = atanf(arrive2);  (s, c) = CurveSinCos(angle)
arriveWeight(f) = k2.tangentWeight ∈ {None, Leave} ? Sqrt(x*x + y*y) * third (y(f) = arrive2 * x)
                                                    : k2.arriveTangentWeight
tan2x(f) = (float)((double)((-c) * arriveWeight) + T2);  tan2y(f) = (-s) * arriveWeight + v2
rangeX(f) = (float)(T2 - T1)
dx1(f) = (float)((double)tan1x - T1);  dx2(f) = (float)((double)tan2x - T1)
n1(f) = dx1 / rangeX;  n2(f) = dx2 / rangeX
BezierToPower(0, n1, n2, 1)(d): a = n1 - 0, b = n2 - n1, c' = 1 - n2, d' = b - a
    coeff[3] = c' - b - d',  coeff[2] = 3.0 * d',  coeff[1] = 3.0 * a,  coeff[0] = 0
coeff[0] = coeff[0] - alpha
(근 개수, 근) = SolveCurveCubic(coeff)
근이 1개면 그 근, 아니면 실제 근을 차례로 보며 [0, 1](허용오차 1e-8 포함 — IsNearlyEqual double 기본값) 안이고 "지금 고른 값이 음수이거나 이 근이 더 크면" 고른다(UE 순서 그대로 — 대개 가장 큰 근), 없으면 0
결과 = BezierCurveValue(v1, tan1y, tan2y, v2, (float)근)
```

`CurveSinCos`는 UE `FMath::SinCos(float)`(UMU 760–798)의 범위 축소(`quotient = (1/(2π))·x`를 `int64`로 반올림, `y = x − 2π·quotient`, ±π/2 접기)와 11차·10차 minimax 다항식을 계수·연산 순서 그대로 옮긴 것이다. libm이 아니므로 `-ffp-contract=off` 아래에서 플랫폼과 무관하게 결정적이다. `atanf`는 UE `FMath::Atan(float)`(`GenericPlatformMath.h` 542 — `atanf`)과 같은 C 런타임 함수이고 `Sqrt`(sqrtf)는 IEEE 정확 반올림이다.

`SolveCurveCubic`(CE 10–79)은 UE 식을 연산 순서 그대로 옮긴다(수학적으로 같은 다른 순서는 비트가 다르다 — `x/3`과 `(1.0/3)·x`는 다르다):

```
den = coeff[3] != 0 ? coeff[3] : 1e-8 * 1e-8
A = coeff[2] / den;  B = coeff[1] / den;  C = coeff[0] / den
SqOfA = A * A
P = (1.0/3) * ((-1.0/3) * SqOfA + B)
Q = (1.0/2) * (((2.0/27) * A) * SqOfA - ((1.0/3) * A) * B + C)
CubeOfP = (P * P) * P;  D = Q * Q + CubeOfP
|D| <= 1e-8:  |Q| <= 1e-8이면 근 {0}, 아니면 U = Cbrt(-Q), 근 {2 * U, -U}
D < 0:        Phi = (1.0/3) * acos(-Q / sqrt(-CubeOfP)); T = 2 * sqrt(-P)
              근 {T * cos(Phi), -T * cos(Phi + π/3), -T * cos(Phi - π/3)}   (π/3 = UE_DOUBLE_PI / 3)
그 밖:        SqrtD = sqrt(D); 근 {Cbrt(SqrtD - Q) + (-Cbrt(SqrtD + Q))}
모든 근에서 Sub = (1.0/3) * A를 뺀다
Cbrt(x) = x > 0 ? pow(x, 1.0/3.0) : x < 0 ? -pow(-x, 1.0/3.0) : 0
```

**UE와 다른 점**: UE는 근이 2개일 때 채워지지 않은 세 번째 칸까지 읽는다(CE 167 — 초기화되지 않은 값). Rocks는 실제 근 개수만큼만 본다(15절 3). 근이 1개일 때 [0, 1] 범위를 보지 않는 것은 UE와 같다(x(0) = 0, x(1) = 1이므로 실근이 하나면 그것이 [0, 1] 안의 유일한 해다).

`acos`·`cos`·`sqrt`·`pow`(double)와 `atanf`는 UE와 같은 표준 함수를 부른다. 이 다섯은 C 런타임 구현에 따라 마지막 비트가 달라질 수 있으므로 가중 경로의 비트 결정성은 **Windows x64(MSVC 런타임)에서 UE와 같은 것**까지만 보장하고 플랫폼 간 비트 동일성은 보장하지 않는다(UE와 같은 수준 — D17).

### 5.5 범위 밖 — 앞·뒤 외삽(RC 1178–1252)

- 키 1개: 외삽과 무관하게 그 키 값.
- Linear(키 2개 이상): 기울기는 **끝의 두 키 값 차 ÷ 시간 차**다(키 접선을 쓰지 않는다 — Cubic 끝 키여도). 앞쪽 `dt = t1 − t0`, `|dt| <= 1e-8`이면 `v0`, 아니면 `(v1 − v0)/dt × (time − t0) + v0`. 뒤쪽 `dt = t(N−2) − t(N−1)`, 같은 꼴로 `(v(N−2) − v(N−1))/dt × (time − t(N−1)) + v(N−1)`.
- Constant·Cycle·CycleWithOffset·Oscillate: 끝 키 값(순환 계열은 5.1에서 이미 시간을 키 범위로 옮겼다).

### 5.6 특수 입력

- 키 0개: 기본값 또는 fallback. 키가 있으면 기본값은 쓰지 않는다.
- NaN 시간: **UE와 다르다**. UE는 비교가 모두 거짓이라 뒤 외삽 갈래로 가 마지막 키 값(Linear면 NaN)을 낸다. Rocks는 키가 있으면 NaN을 낸다(G01 "잘못된 입력을 조용히 바꾸지 않는다", InterpCurve와 같은 규약 — 15절 4).
- ±Inf 시간: 순환 외삽이면 재배치 없이 끝 키 값, Linear면 ±Inf 또는 NaN(산술 결과 그대로), 그 밖은 끝 키 값.
- 비유한 키 시간은 편집 연산이 InvalidArgument로 거부하고 리플렉션 경로는 검증 hook이 거부한다(9.1절). 비유한 값·접선은 받아서 결과로 전파한다.

### 5.7 `PlainCurve` — `FSimpleCurve::Eval`(SC 704–794)

시간 재배치·외삽은 5.1·5.5와 같다. 구간 선택의 경계만 반대다: 앞 갈래가 `time <= keys[0].time`(SC 719), 안쪽이 `time < keys[N−1].time`(SC 743)이다. 그래서 결과가 `TangentCurve`의 Linear 커브와 갈리는 곳은 다음 둘이다.

- 첫 키 시간이 중복이면 `PlainCurve`는 앞 갈래로 가 `keys[0].value`를 내고, `TangentCurve`는 그 시간의 마지막 키 구간(5.2절)을 쓴다.
- 끝 키 시간에서 `PlainCurve`는 뒤 갈래로 가 `keys[N−1].value`(Linear 외삽이어도 `slope × 0 + v`)를 내고, `TangentCurve`는 마지막 구간의 비율 1이다(Rocks `Lerp`는 끝 값을 그대로 내므로 마지막 키 시간이 중복이 아니면 같은 값이다).

두 키 사이는 `diff > 0`이고 `interp`가 Linear면 `Lerp(v1, v2, (time − t1)/diff)`, 아니면 `v1`이다(UE `FSimpleCurve::EvalForTwoKeys`, SC 493–509 — Constant여도 `time < t2` 비교를 하지 않는다). UE는 Cubic을 ensure 후 무시하고 저장된 Cubic 값은 Linear로 평가하지만(`SimpleCurve.h` 147–153), Rocks는 `PlainCurveInterp`에 Cubic이 없어 표현할 수 없다(15절 5).

### 5.8 계단 커브

- `IntegerStepCurve::Evaluate(time, fallback)`(UE `FIntegralCurve::Evaluate`, `IntegralCurve.cpp` 7–40): 기본값(없으면 fallback)으로 시작해, 키가 없거나 `useDefaultBeforeFirstKey`이고 `time < 첫 키`면 기본값, 키 1개이거나 `time < 첫 키`면 첫 키 값, `time < 마지막 키`면 "시간이 time 이하인 마지막 키" 값, 그 밖은 마지막 키 값.
- `StringStepCurve::Evaluate`(UE `FStringCurve::Eval`, `StringCurve.cpp` 57–90): 첫 키 이전은 **늘** 기본값이다(Integer와 다르다 — UE 그대로).
- `NameStepCurve::Evaluate`: UE `FNameCurve`에는 평가 함수도 기본값 칸도 없다. Rocks는 기본값 칸을 두고 String과 같은 규칙으로 평가한다(D9).
- UE는 "time 이하인 마지막 키"를 선형 탐색으로 찾는다. 결과가 같으므로 Rocks는 이진 탐색이다.

## 6. 접선 자동 계산 — `TangentCurve::AutoSetTangents(float tension = 0)`(RC 680–755)

키마다 다음을 한다(가중치는 건드리지 않는다).

- 첫 키: 키가 2개 이상이고 접선 방식이 Auto면 출발 접선 = 0(보간 방식과 무관).
- 안쪽 키: 보간이 Cubic이고 접선 방식이 Auto면 `ComputeCurveTangent(prev, this, next, tension)`을 도착·출발 접선 둘 다에 넣는다. 아니고 앞 키나 이 키의 보간이 Constant면(접선 방식과 무관 — User·Break 키도) 출발 접선 = 0, 앞 키가 Cubic이 아니면 도착 접선도 0.
- 마지막 키: 보간이 Cubic이고 Auto면 도착 접선 = 0.
- `ComputeCurveTangent`(UE `InterpCurvePoint.h` 240–301의 clamp 없는 경로) = `((1 − tension) × ((v − v_prev) + (v_next − v))) / CurveTangentTimeDifference(t_prev, t_next)`(하한 1e-4 — UE `FMath::Max<float>(UE_KINDA_SMALL_NUMBER, …)`). 괄호 안은 `v_next − v_prev`로 줄이지 않는다(UE `AutoCalcTangent` 순서, 비트가 다르다). clamp(`ClampFloatTangent`)는 `FRichCurve`가 쓰지 않는다.

`SmartAuto`는 UE에서 MovieScene 채널(`MovieSceneCurveChannelImpl.cpp`)만 계산하고 `FRichCurve`는 계산하지 않으므로 두지 않는다(D5).

UE의 재계산 시점: `AddKey`·`UpdateOrAddKey` 뒤에는 하지 않는다. `DeleteKey`(RC 370)·`SetKeys`(RC 361)·`RemoveRedundantKeys`(RC 1094)와 `bAutoSetTangents`가 참인 `SetKeyValue`·`SetKeyInterpMode` 등 뒤에는 한다. `SetKeyTime`은 안에서 `DeleteKey`를 부르므로(RC 409) 옮기기 전 상태(그 키를 뺀 배열)로 한 번 재계산하고, 옮긴 키는 옛 접선을 그대로 갖는다. `ShiftCurve`·`ScaleCurve`도 키마다 `SetKeyTime`이라(IndexedCurve.cpp 113) 같은 일이 키 수만큼 일어난다.

Rocks 값 타입의 편집 연산은 **어느 것도 접선을 몰래 다시 계산하지 않는다** — 자동 접선이 필요한 편집 도구가 편집 묶음 끝에 `AutoSetTangents`를 한 번 부른다(D10). 그래야 같은 편집이 연산 순서와 무관하게 같은 접선을 낸다. UE와 결과를 맞추려면 호출자가 UE가 재계산하는 지점에서 `AutoSetTangents`를 부른다(golden 시험이 이렇게 대조한다 — 16절).

## 7. 키 편집 연산

값 타입의 공개 편집 API는 키 index와 `Result`로 동작한다(키 핸들은 8절의 표가 index 위에 얹는다). 모든 연산은 실패하면 커브를 바꾸지 않는다.

### 7.1 연산 표

| 연산 | 규칙(UE 대응) |
|---|---|
| `TryAddKey(time, value, unwindRotation = false) -> Result<usize>` | 같은 시간 키가 있으면 그 키들 **앞**에 넣는다(UE `AddKey` RC 303–339 — 첫 `keys[i].time >= time` 앞). 새 키는 Linear·Auto·가중 없음. `unwindRotation`이면 이전 키 값 기준으로 차이가 180°를 넘는 동안 360°씩 옮긴다(도 단위 — UE와 같다, 13절). 비유한 시간은 InvalidArgument |
| `TryUpdateOrAddKey(time, value, unwind, tolerance = 1e-4f)` | `|키 시간 − time| <= tolerance`인 첫 키의 값만 바꾸고(unwind는 새 키에만 — UE와 같다, RC 376–400) 없으면 추가 |
| `TryRemoveKey(index)` | 지운다(접선 재계산 없음 — D10) |
| `TrySetKeyTime(index, time) -> Result<usize>` | 키의 모든 칸을 유지한 채 새 시간 자리로 옮기고 새 index를 돌려준다(같은 시간 키 앞). 다른 키의 접선도 바꾸지 않는다 |
| `SetKeyValue`·`SetKeyInterp`·`SetKeyTangentMode`·`SetKeyTangentWeight`·`SetKeyTangents` | 칸만 바꾼다(index 범위 밖은 InvalidArgument인 `Try*` 판) |
| `TrySetKeys(Span<const Key>)` | 정렬·유한 시간 입력만 받는다(위반은 InvalidArgument — UE는 정렬을 기대만 하고 확인하지 않는다) |
| `FindKey(time, tolerance = 1e-4f) -> usize` | 이진 탐색, 없으면 `IndexNone`. 허용오차 안의 키가 여럿이면 **가장 앞** 키(UE `GetKeyIndex` RC 431–457은 탐색 경로에 따라 정해진다 — 15절 6) |
| `IsConstant(tolerance = 1e-8f)` | 키 ≤ 1이거나, 모든 키 값이 첫 키와 tolerance 안이고 Cubic 키가 없음(UE와 같다, RC 485–502) |
| `TryGetTimeRange() -> Result<Interval<float>>` | 키가 없으면 NotFound(UE는 (0, 0)) |
| `TryGetValueRange() -> Result<Interval<float>>` | 키 범위 안에서 Evaluate가 내는 값을 모두 담는 범위를 구간마다 **해석적으로** 구한다(UE는 표본 재귀 `FeaturePointMethod`, RC 604–670) — 7.3절, D11. 키가 없으면 NotFound, 범위가 유한하지 않으면 InvalidArgument |
| `ShiftTime(delta)`·`TryScaleTime(origin, factor)` | 모든 키 시간을 바꾼다(factor > 0만 — 순서 유지, 0·음수·비유한은 InvalidArgument, 결과 시간이 비유한이면 InvalidArgument). UE는 키마다 지우고 다시 넣는다(O(n²)) |
| `TryShiftKeyTimes(Span<const usize> indices, delta)` | 고른 키만 옮긴다(UE `ShiftCurve(Delta, KeyHandles)` 대응 — 편집기 선택 이동). 고른 키를 빼고, 시간을 바꿔, 원래 index 순서대로 "같은 시간 키 앞" 규칙으로 다시 넣는다. UE는 `TSet` 순회 순서라 결과 순서가 정해지지 않는다. 중복·범위 밖 index는 InvalidArgument |
| `TryBake(interval, first, last)` | `first + interval`부터 `last` 미만까지 간격 `interval`로 Evaluate한 값을 `TryUpdateOrAddKey`로 넣는다. 시간은 누적 덧셈이 아니라 `first + i × interval`로 계산한다(UE는 float 누적, 그리고 `first == last`면 `Reserve(−1)`이 fatal — RC 900–908, 15절 7). 이름도 UE `SampleRate`(실제로는 간격)를 `interval`로 바로잡는다 |
| `TryRemoveRedundantKeys(tolerance, Optional<FrameRate> sampleRate)`·범위판 `(tolerance, first, last, Optional<FrameRate>)` | 7.2절 |
| `Reset()` | 키만 비운다(기본값·외삽 유지 — UE와 같다) |

`ReadjustTimeRange`(UE 시퀀서 시간 범위 맞춤)는 에셋·도구 층의 일이라 값 타입에 두지 않는다(D12). 계단 커브의 `RemoveRedundantKeys`는 UE와 달리 남은 키 하나를 기본값으로 바꾸지 않는다(15절 9).

### 7.2 중복 키 제거 — UE `RemoveRedundantKeysInternal`(RC 966–1102), D20

- 알고리즘은 UE를 옮긴다: 시작·끝 보존 키 사이의 키마다, 표본판이면 직전 보존 키와 다음 키 사이를 `sampleRate`의 프레임마다 평가해 "그 키를 뺀 값"과 "있는 값"의 차가 tolerance를 넘으면 남긴다(RC 1024–1051). 표본판이 아니면 세 키가 모두 User·Break가 아닐 때만 그 키 시간에서 비교한다(RC 1053–1067).
- 표본 시간은 UE와 같은 `FrameRate::AsFrameTime(double)`(내림 + subframe, UE `FrameRate.h` 240–255)으로 시작해 frame 번호를 1씩 올리고 `AsSeconds`(같은 파일 232–238)를 float로 바꿔 평가한다. 그래서 D20의 `FrameRate`·`FrameNumber`·`FrameTime`이 선행이다. UE `FFrameRate(0, 0)`(무효 = 표본 없음)은 `Optional<FrameRate>`의 unset이다.
- 범위판의 비교 기준(UE `MostRecentKeepKeyIndex`)은 시작 보존 키에서 시작한다(UE는 index 0, RC 1017 — 15절 8).
- UE는 제거 뒤 `AutoSetTangents`를 부른다(RC 1094). Rocks는 D10에 따라 부르지 않는다. 그래서 Auto 키의 접선은 지운 이웃을 기준으로 계산된 옛 값이 남고, UE와 결과를 맞추려면 호출자가 이어서 `AutoSetTangents`를 부른다. UE 알고리즘이 Auto 키를 지워도 된다고 보는 것도 이 재계산을 전제한 판단이므로 편집 도구는 두 호출을 한 묶음으로 쓴다.

### 7.3 값 범위 — D11

- 구간마다 Evaluate가 그 구간에서 실제로 내는 값(구간 끝은 극한)을 넣는다: Linear는 두 끝, Constant는 앞 키 값(마지막 구간은 끝 키 값도), Cubic은 극값과 쓰이는 끝, 길이 0 구간은 마지막 구간일 때만 앞 키 값이다. 쓰이지 않는 키 값은 넣지 않는다. 외삽은 보지 않는다(UE와 같다).
- 비가중 Cubic: Bezier 값 다항식 y(s)의 도함수(2차)의 (0, 1) 안 근에서 `BezierCurveValue`로 평가한다.
- 가중 Cubic: 시간 축 x(s)와 값 축 y(s)가 모두 Bezier다. Evaluate가 쓰는 매개변수는 "x(s) = α의 [0, 1] 안 가장 큰 근"이므로 범위는 그 근들이 지나는 매개변수 집합 S 위의 y 극값이다.
  - x가 [0, 1]에서 단조(x′의 근이 (0, 1)에 없음)면 S = [0, 1].
  - 일반적으로 S = {s ∈ [0, 1] : x(s) ≥ 0이고 모든 u ∈ (s, 1]에서 x(u) > x(s)}(α = 1인 s = 1은 마지막 구간에서만 쓰인다)다. x′의 (0, 1) 안 근으로 나눈 조각 중 x가 증가하는 조각에서, 오른쪽 조각들의 x 최솟값보다 작은 부분이다. 두 근 s₁ < s₂(극대 x(s₁), 극소 x(s₂))이면 S = {s ∈ [0, s₁] : 0 ≤ x(s) < x(s₂)} ∪ {s ∈ [s₂, 1] : x(s) ≥ 0}이 된다. 조각 경계(뛰는 점)에서는 두 쪽 극한을 모두 넣는다.
  - Evaluate의 3차 방정식 풀이(UE `SolveCubic`, CE 32–35)는 판별식 |D| ≤ 1e-8이면 근을 겹근으로 근사해 S 밖의 매개변수를 고를 수 있다(얕은 접힘은 실근 셋인 α 구간 전체가 이 띠에 든다 — 무작위 탐색에서 수학적 범위를 폭의 2%까지 넘었다). 그래서 범위는 그 띠의 α에서 풀이가 실제로 고르는 매개변수도 넣는다(17.4절 C10).
  - S의 각 조각 안에서 y′의 근과 조각 끝에서 y를 평가한다. 근 계산은 double이다.
- 결과는 Evaluate가 키 범위 안에서 내는 값을 모두 담는다(float 반올림 여유만큼 — 계약 주석에 적는다). UE 표본 재귀와 결과가 다를 수 있다(15절 18).

## 8. 키 핸들

UE 사실(`KeyHandle.h`·`KeyHandle.cpp`): `FKeyHandle`은 32비트이고 전역 `std::atomic<uint32>` 카운터에서 발급되며 0이 무효다(첫 값 2). 핸들 표는 커브 안의 `UPROPERTY(transient) mutable` 칸이라(`IndexedCurve.h` 98–99) 디스크에 저장되지 않고 undo 버퍼에만 기록된다(`KeyHandle.cpp` 144–154). 핸들은 필요할 때 늦게 만들어지고, 키 배열을 직접 고치면 표와 어긋나 개수만 맞춘다. 커브를 복사하면 표가 비고(새 핸들), **대입하면 대상의 옛 표가 그대로 남아** 옛 핸들이 새 커브의 같은 index 키를 가리킨다(KH 74–76 — 결함). `const` 조회가 `mutable` 표를 고쳐 const 동시 조회가 경쟁한다.

Rocks(D3):

- 키 핸들은 **값의 일부가 아니다**. 값 타입(`TangentCurve` 등)은 핸들을 갖지 않고, 복사·비교·직렬화·차이 저장에 나타나지 않는다.
- **표와 커브의 연결**: 표는 커브를 가리키는 포인터·참조를 쥐지 않는다(커브는 객체 필드·배열 원소 안에서 옮겨 다니므로 주소를 붙잡을 수 없다). 표의 모든 함수가 커브를 인자로 받는다: `table.TryAddKey(curve, time, value)`, `table.TryFindIndex(curve, handle) -> Result<usize>`, `table.TryRemoveKey(curve, handle)`, `table.TrySetKeyTime(curve, handle, time)` 등. 표를 거치는 편집 함수가 커브와 표를 함께 갱신한다.
- **표를 거치지 않은 수정의 감지**: 표는 마지막으로 맞춘 키 배열 전체의 사본(키 struct 그대로, 비트 비교)을 쥔다. 표의 모든 함수는 먼저 인자로 받은 커브의 키 배열을 사본과 비교하고(O(N)), 다르면 표 전체를 무효로 표시하고 StaleHandle을 낸다. 무효가 된 표는 `TryRebuild(curve)`가 새 핸들을 다시 발급할 때까지 모든 조회가 StaleHandle이다(UE처럼 끝 쪽만 맞춰 다른 키를 가리키게 두지 않는다). 리플렉션 경로 쓰기·대입 묶음·undo가 값을 바꿔도 같은 방법으로 잡힌다. 해시가 아니라 사본 비교라 충돌로 놓치는 일이 없다. 비용은 편집기에서만 든다(메모리는 키 배열 하나 더, 시간은 표 호출마다 O(N)).
- 핸들 값은 `uint32`이고 표마다 1부터 단조 증가하며 0은 무효(`CurveKeyHandle::Invalid`)다. 전역 카운터를 쓰지 않는다 — 핸들은 그 표 안에서만 뜻이 있고, 표마다 세면 결정적이다(시험·재실행에서 같은 값). 표의 카운터가 `NumericLimits<uint32>::Max()`에 닿으면 새 핸들 발급이 Overflow다(UE는 쿡 중에 0을 건너뛰며 순환).
- 표 복사는 핸들·사본까지 그대로 복사한다(복제된 편집 세션이 같은 키를 같은 핸들로 가리킨다). 대입도 마찬가지로 원본 표로 완전히 바뀐다.
- 표는 저장하지 않는다(UE와 같다). undo 기록은 표의 직렬 형태를 따로 쓴다(CU4 Undo 설계와 함께 정한다 — 그 단계의 일).

## 9. 리플렉션·wire·JSON

| B9 기능 | 쓰는 곳 |
|---|---|
| struct 상속(B9k) | `TangentCurve`·`PlainCurve`가 `ScalarCurveBase`를 상속(기본값·외삽 필드 ID는 base 소유) |
| Optional(B9e) | `defaultValue`(UE의 `MAX_flt`·`MAX_int32`·빈 문자열 "없음" 표지 대신), `PackedTangentCurve`의 Empty 기본값 |
| Name(B9c) | `NameStepKey::value`·`NameStepCurve::defaultValue` |
| enum underlying(B9a2) | 커브 enum은 `uint8` underlying(UE `TEnumAsByte`와 같은 1바이트 — wire는 I64) |
| Atomic(B9b) | 키 struct |
| private 필드·필드 `Name`(B2) | `keys_`(`Name = "keys"`) |
| 이 문서가 보강하는 것 | struct 검증 hook(9.1절, D7), `Array<uint8>`의 Bytes 블록 wire(11절, D18), 미러의 private 멤버 접근(12절, D19) |
| InstancedStruct·Map·Text·bitfield·WeakRef·ClassRef | 이 층에서는 쓰지 않는다(InstancedStruct·Map은 에셋 층이 고를 수 있다 — 14절) |

- JSON은 ReflectionText 규칙 그대로다. 예:

```json
{"defaultValue": null, "preExtrapolation": "Constant", "postExtrapolation": "Cycle",
 "keys": [{"time": 0, "value": 1, "interp": "Cubic", "tangentMode": "Auto", "tangentWeight": "None",
           "arriveTangent": 0, "arriveTangentWeight": 0, "leaveTangent": 0, "leaveTangentWeight": 0}]}
```

- 같음(대입 묶음의 무동작 판정·차이 저장)은 리플렉션 값 기준이다. UE `FRichCurveKey::operator==`는 가중치를 비교하지 않고 접선은 Cubic 키에서만 비교하며(RC 222–229), `FRichCurve::operator==`는 기본값을 비교하지 않고(RC 1256–1277), `FStringCurveKey`는 `FString ==`라 대소문자를 무시한다(`StringCurve.cpp` 11–14). Rocks는 모든 칸을 비교한다. 실수는 비트로, String은 byte로 비교한다. Name은 Rocks Name의 같음 규칙(ASCII 대소문자 무시 — `Name.h` 89행)을 따르므로 `NameStepKey`는 대소문자만 다른 값을 같다고 본다(UE `FName`도 같다). 표시 철자는 wire에 보존된다(15절 10).

### 9.1 struct 검증 hook — D7

정렬 불변식과 유한 키 시간, `PackedTangentCurve`의 칸 사이 정합(11.4절)은 리플렉션 경로(decode, JSON·명령행 import, 대입 묶음, 경로 쓰기 `keys[3].time=…`)가 struct의 의미를 모른 채 바꿀 수 있다. 그래서 리플렉션 기반에 struct 검증 hook을 더한다([리플렉션 보강](ReflectionExtensions.ko.md)에 넣을 변경 — CV3).

- **선언**: `ROCKS_STRUCT`가 `static Result<void> Validate(const S&) noexcept`를 가지면 MetaCompiler가 `Migrate`처럼 감지해 `RuntimeStructOps`의 새 칸 `validate`에 잇는다. 시그니처가 다르면 RMC 진단이다(번호는 CV3에서 15절 진단 표에 더한다). Binds 미러는 미러 struct에 `static Result<void> Validate(const 실제타입&) noexcept`를 둔다(12절).
- **계약**: noexcept, 할당하지 않는다(Allocator를 받지 않는다), 전역 상태·Registry를 읽지 않는 순수 함수, 아무 스레드에서나 불릴 수 있다(decode가 worker에서 돌 수 있다 — AssetSystem 341행). 결과는 Success 또는 InvalidArgument다. 다른 오류 코드를 내면 InvalidArgument로 다룬다. 값을 고치지 않는다(정렬해 주지 않는다 — G01).
- **부르는 때 — 게시 전 1회**: 바뀐 값이 객체에 게시되기 전, 그 값 안의 hook 있는 struct 인스턴스마다 한 번 부른다. 실패하면 게시하지 않고 값은 그대로다(진단 경로는 그 struct의 위치).
  - graph decode: 2단계에서 struct 값을 다 채운 뒤 게시 전. 마이그레이션(`Migrate`)이 먼저다.
  - JSON·명령행 import와 대입 묶음: 준비한 사본(상자)에 모든 대입을 적용한 뒤 Publish 전.
  - 경로 쓰기: 쓰기 대상을 담은 hook 있는 struct를 사본에 적용해 검증한 뒤 게시한다. 제자리 쓰기 경로는 이 단계에서 사본 경로로 바꾼다(hook 없는 struct는 지금 경로 그대로).
- **중첩 위치**: 바뀐 위치를 감싸는 hook 있는 struct 전부를 안쪽부터 부른다(경로 쓰기는 경로 위의 조상 struct들, decode·import는 값 트리 안의 모든 hook 있는 인스턴스). 위치는 struct 필드·struct 멤버·Array·StaticArray 원소·Optional 내부·Map 값·InstancedStruct 내용 전부다. InstancedStruct 내용은 담긴 실제 타입의 hook을 부른다.
- **부르지 않는 때**: C++가 직접 만든 값(API가 불변식을 지킨다), encode(게시된 값은 이미 검증되었다), 같은 값의 무동작 대입.
- **비용**: hook은 값 크기에 선형이다. 큰 커브의 키 하나를 경로로 쓰면 그 커브 전체를 다시 본다(편집기 쓰기 빈도에서 문제 없다).
- PlainCurve의 Cubic 금지는 hook이 아니라 타입으로 막는다(`PlainCurveInterp`에 Cubic이 없다 — 미선언 enum 값은 이미 decode가 거부한다).

### 9.2 wire 크기 — D8

키 배열은 `Array<Key>`(RecordList wire)다. 키 하나는 RecordList 원소 길이(u64) 8 byte + Record 머리말 44 byte + 필드 TLV 머리 필드마다 28 byte(Guid 16 + wire 4 + 길이 8 — `Record.h` 68–75) + 페이로드(float 6개 24 byte, enum 3개 I64 24 byte — `FieldValueWire.h` 29–46)로 약 350 byte다. UE의 수동 이진 키 직렬화(키당 27 byte — RC 163–181)의 약 13배다(개정 1의 "2–3배"는 틀렸다). 기본값과 같은 필드를 생략하는 인코딩이 있으면 줄지만 머리말만으로 52 byte다.

- 저작 데이터는 편집·텍스트·차이 저장의 편의를 위해 리플렉션 wire를 쓰고, 런타임에 많이 읽는 데이터는 압축 형태(11절)를 쓴다는 방향은 유지한다.
- **CV5 실측(17.4절 C14)**: 키당 `TangentCurveKey` 352 byte(원소 길이 8 + Record 머리말 44 + 필드 9 × 28 + 페이로드 48 — 위 추정과 같다), `PlainCurveKey` 116 byte(8 + 44 + 2 × 28 + 8)다. UE 수동 이진 키(27·8 byte)의 약 13·15배이고 1,000키 `TangentCurve`는 약 344 KiB다.
- **판단(권장안으로 확정)**: 저작 wire는 지금의 RecordList를 유지한다. 편집·텍스트·차이 저장·검증 hook이 키 단위 리플렉션에 기대고, 런타임에 많이 읽는 데이터는 `PackedTangentCurve`(11절, CV8)가 맡기 때문이다. 키마다 같은 머리말·필드 ID가 반복되므로 에셋 압축이 잘 들을 것으로 보지만 실측하지 않았다. 에셋 층(A 단계)에서 실제 커브 에셋의 크기가 문제로 측정되면 열 단위 배열(SoA)이나 리플렉션 일반의 기본값 필드 생략 인코딩을 다시 본다. 그때까지 별도 수동 codec을 두지 않는다.

## 10. 직렬화와 버전

- 각 struct는 리플렉션 버전 1에서 시작한다. 하위호환이 없는 초기 단계라 필드 의미를 바꾸면 버전을 올리고 이전 데이터는 버린다(마이그레이션을 쓰지 않는다).
- UE 에셋 파일의 직접 가져오기는 범위 밖이다(Rocks 에셋은 Rocks 형식으로 저작한다). UE 값과의 대조는 시험의 참조 구현으로 한다(16절).
- 키 핸들 표는 저장하지 않는다(8절).

## 11. 압축 커브 `PackedTangentCurve`

UE `FCompressedRichCurve`(RichCurve.h 362–463, RC 1279–2530)의 형식 결정·키 시간 양자화·키 데이터 배치를 따른다. 쿡된 애니메이션 커브처럼 많이 읽는 런타임 데이터용이고 저작 데이터는 `TangentCurve`다. 만드는 길은 `TryPack(const TangentCurve&, errorThreshold = 1e-4f, sampleRateHz = 120)`과 decode뿐이다(칸은 private PROPERTY).

### 11.1 형식 결정 — UE `FindRichCurveCompressionFormat`(RC 1279–1330)을 고친 것(D14)

- 키 없음 → Empty. 기본값을 `Optional<float>`로 옮긴다(UE는 `ConstantValue`에 `MAX_flt` sentinel — RC 1490·2144).
- **Constant**: 키가 1개이거나, Cubic 키가 없고 모든 키 값이 첫 키와 **비트까지 같은 유한 값**이며 앞·뒤 외삽이 모두 Linear가 아니면 Constant다(외삽·유한 조건은 17.7절 P2). UE는 `IsConstant(1e-8)` 허용오차로 판정하고 `keys[0].value` 하나만 남긴다(RC 1286·1495). 그러면 1e-8 안의 값 차이가 사라지고, Linear 외삽의 기울기와 CycleWithOffset의 누적 오프셋은 시간·주기에 비례해 커지는 차이가 된다. Rocks는 비트 같음으로 판정해 원본과 결과가 같게 한다(15절 20).
- **Weighted**: 가중 구간이 하나라도 있으면 Weighted다. 가중 구간은 `keys[i].interp == Cubic`이고 `IsItNotWeighted(keys[i], keys[i+1])`가 거짓인 (i, i+1)이다(5.3절). UE는 "Cubic 키의 가중 방식이 None이 아님"만 보아(RC 1296–1302) Linear·Constant 키의 도착 가중이 앞 Cubic 구간을 가중으로 만드는 경우를 놓친다 — 원본은 가중 경로(CE 223)로 평가하는데 압축은 Mixed·Cubic 형식이 되어 가중을 버린다(RC 1849–1852). Rocks는 구간 단위로 판정한다(15절 21). 마지막 키의 가중 방식처럼 어느 구간에도 쓰이지 않는 칸은 판정에 쓰지 않는다.
- 그 밖: 보간이 섞였으면 Mixed, 아니면 첫 키 보간에 따라 Linear·Cubic. 모든 키가 Constant여도 Mixed다(UE와 같다, RC 1320–1321). UE `RCIM_None` 갈래는 없다(D4).

### 11.2 키 시간 형식 — RC 1332–1475

기본 오차 1e-4·표본 120 Hz(`RichCurve.h` 334)로 16비트 양자화가 오차 안이고 크기가 작으면(`SizeUInt16 < SizeFloat32`, RC 1464–1467) Quantized16, 아니면 Float32. 양자화 식 `q = RoundHalfFromZero(Clamp((t − min) × (1/Δ), 0, 1) × 65535)`(RC 1634–1635), 복원 `(q × (1/65535)) × Δ + min`(RC 1720–1721). 표본 검사에서 UE는 표본마다 키 index를 한 칸만 전진해 표본 간격보다 촘촘한 키에서 판정이 틀린다(RC 1399–1426) — Rocks는 원본 쪽과 손실 쪽을 모두 `Evaluate`와 같은 평가(같은 구간 탐색·외삽 틀)로 비교한다(15절 11, 17.7절 P4). 시간이 모두 같은 키 2개 이상(Δ = 0)은 Float32로 둔다(UE는 `1/0`과 NaN Clamp로 판정한다).

### 11.3 바이트 배치

보간 배열 → 키 시간 → 키 데이터 순서다. 정렬 틈은 0이다. **모든 수는 리틀 엔디언으로 명시해 쓴다**(UE는 원시 메모리 그대로 — 15절 12).

- 보간 배열: Mixed는 키당 1 byte, Weighted는 키당 2 byte(보간·가중 방식). 보간 byte는 `CurveInterp` 값이 아니라 **형식 값**이다: Linear → 2, Cubic → 3, Constant → 1(UE `RCCF_*` — RC 1577–1588·1598–1611). 가중 byte는 `CurveTangentWeight` 값(0–3)이다.
- 키 시간: Quantized16은 2 byte 정렬 뒤 uint16 × N, 다시 4 byte 정렬 뒤 min·Δ float 두 개. Float32는 4 byte 정렬 뒤 float × N.
- 키 데이터: 키별 교차 배치 float. 키당 Linear 1(값)·Cubic 3(값·도착·출발)·Mixed 3(모든 키에 접선 — UE `MIXEDKEY_STRIPS_TANGENTS` 0)·Weighted 5(값·도착·출발·도착 가중치·출발 가중치).

### 11.4 리플렉션 표현과 정합 — D18, D21

- `ROCKS_STRUCT` 하나에 private PROPERTY `format`(`PackedCurveFormat`)·`keyTimeFormat`·앞·뒤 외삽(enum), `keyCount`(uint32), `constantValue`(float — Constant 형식), `emptyDefault`(`Optional<float>` — Empty 형식), `packed`(`Array<uint8>`)를 둔다. UE의 union 대신 칸을 나눈다(D13). 기본 외삽은 Constant다(UE `FCompressedRichCurve`만 `RCCE_None`인 것과 맞추지 않는다 — Rocks에는 None이 없다).
- `packed`는 D18에 따라 wire Bytes 블록 하나(u64 길이 + byte)로 저장된다(CV4 — 17.3절). 길이 한도는 `MaxElements = 1048576`(MaxArrayElements)이다.
- **정합 규칙**(`ExpectedPackedSize(format, keyTimeFormat, keyCount)`가 11.3절 배치로 기대 크기를 계산한다):
  - Empty: keyCount 0, packed 비어 있음. Constant: keyCount 0, packed 비어 있음, constantValue 사용.
  - Linear·Cubic·Mixed·Weighted: keyCount ≥ 2, `packed.Size() == ExpectedPackedSize(…)`(크기 계산의 넘침은 불일치로 본다).
  - enum 칸은 선언 값만(decode가 이미 거부한다).
- **검증 hook(Validate)**: 위 정합에 더해 보간 byte ∈ {1, 2, 3}, 가중 byte ∈ {0..3}, 키 시간 비내림차순·유한, Quantized16의 min·Δ 유한·Δ ≥ 0을 본다. 위반은 InvalidArgument다.
- **평가 진입의 O(1) 대조**: `Evaluate`는 매번 enum 범위와 `packed.Size() == ExpectedPackedSize(…)`(Linear 이상은 keyCount ≥ 2 포함)를 확인한다. 불일치면 **quiet NaN**을 낸다(조용한 대체값을 내지 않는다 — G01). `TryEvaluate`는 같은 경우 InvalidArgument를 낸다. hook·private 칸 때문에 정상 경로에서는 일어나지 않고, 메모리 밖 읽기를 막는 방어선이다. 정렬되지 않은 키 시간은 O(1)로 볼 수 없지만 이진 탐색이 범위 안 index만 쓰므로 메모리 안전하다(hook이 decode에서 거부한다).

### 11.5 평가와 풀기

- 평가: 압축 형태를 풀어 키 쌍을 만든 뒤 **5절과 같은 `math::EvalCurveSegment`를 부른다**. 11.1절의 형식 결정 수정과 함께 Float32 시간 형식의 압축 커브는 원본 `TangentCurve`와 비트가 같다(D14). UE 압축 평가는 제어점 곱셈 순서 `tangent × (diff × 1/3)`(RC 2108–2112)와 가중 경로의 순환 오프셋 누락(RC 2118 — 바로 반환) 때문에 원본과 다를 수 있다. Quantized16은 복원된 시간으로 평가하므로 그 시간 오차만큼 다르다.
- UE 결함 수정: Weighted 형식의 가중 판정에 키 index 대신 데이터 위치(index × 5)를 넘겨 엉뚱한 byte를 읽는 것(RC 2077–2105), 가중 경로가 순환 오프셋을 더하지 않는 것, 풀기(`PopulateCurve`)가 Cubic·Mixed 키를 Auto 접선으로 만들고 `SetKeys`의 `AutoSetTangents`로 접선을 다시 계산해 버리는 것, Empty 형식에서 union을 키 수로 읽어 기본값이 `MAX_flt`(비트 0x7F7FFFFF)면 약 21억 번 도는 것(RC 2441–2452), Constant 형식을 풀지 않는 것(RC 2443) — 15절 13.
- 풀기 `TryUnpack(TangentCurve&)`: 접선 방식은 User(Weighted는 가중 포함)로 복원하고 외삽·기본값도 옮긴다(UE는 옮기지 않는다). Constant 형식은 그 값의 키 하나(시간 0, Linear)로, Empty는 키 없음 + 기본값으로 푼다. 원래 접선 방식(Auto·Break)은 압축에서 사라진다.

## 12. InterpCurve 미러와 값 범위 — D19

G09와 [키 커브](../Implementation/InterpCurve.ko.md) 2절(`operator<<` → "리플렉션 미러·직렬화 단계", `CalcBounds` → 후속)이 이 단계로 미룬 일이다.

### 12.1 미러

- 대상은 UE에 USTRUCT가 있는 다섯 형식이다: `InterpCurve<float>`(UE `FInterpCurveFloat`), `InterpCurve<Vec2d>`(`FInterpCurveVector2D` — UE 5의 `FVector2D`는 double), `InterpCurve<Vec3d>`(`FInterpCurveVector`), `InterpCurve<Quatd>`(`FInterpCurveQuat`), `InterpCurve<LinearColor>`(`FInterpCurveLinearColor`)와 각 `InterpCurvePoint<T>`. `FInterpCurveTwoVectors`는 Rocks 값 형식이 없어 두지 않는다. 다른 형식(`double`·float 벡터 등)은 소비자가 생길 때 미러를 더한다.
- `InterpCurvePoint<T>`는 멤버가 public(`inValue`·`outValue`·`arriveTangent`·`leaveTangent`·`mode`)이라 지금 Binds 규칙("공개 데이터 멤버를 같은 이름·형식·선언 순서로" — ReflectionExtensions 3.2절)으로 미러한다. `Atomic`이다(키 struct와 같은 이유). `InterpCurveMode` enum의 미러가 필요하다(Core enum Binds는 없어서 CV9에서 더했다 — 17.8절 I2).
- `InterpCurve<T>`는 키 배열 `points_`·`looped_`·`loopKeyOffset_`이 private이다(정렬 불변식). 접근자로 미러하지 않고 **private 멤버 접근을 기반에 보강**한다: Core 쪽 `InterpCurve<T>`가 `template <class> friend struct ::rocks::GeneratedType;`을 선언한다(Core는 `GeneratedType`을 앞 선언만 하므로 의존이 생기지 않는다 — `ROCKS_GENERATED_BODY`가 넣는 friend와 같은 것, `Meta/Annotations.h` 101–105). MetaCompiler는 Binds 미러 멤버가 실제 타입의 private 멤버여도 이 friend를 통해 접근하는 생성 코드를 낸다. 미러 멤버 이름은 C++ 이름(`points_` 등)이고 도구·JSON 이름은 `Name="points"`·`"looped"`·`"loopKeyOffset"`이다.
- 미러는 `Validate`를 둔다(9.1절): 키 입력값 비내림차순·유한, `loopKeyOffset` 유한, `looped`이면 키가 있고 offset > 0이다(17.8절 I4).
- 생성·복사: `InterpCurve<T>`는 `Allocator&` 생성자와 `TryCopyFrom`이 있어 `RuntimeStructOps`의 construct·copy를 채울 수 있다. 같음은 리플렉션 값 기준이다(UE `operator==`는 루프가 꺼져 있으면 offset을 비교하지 않는다 — 리플렉션 같음은 모든 칸).

### 12.2 `TryCalcBounds`

- `Result<CurveBounds<T>> TryCalcBounds() const`(최소·최대 한 쌍). 키가 없으면 NotFound(UE는 인자로 받은 기본값 — `InterpCurve.h` 746–754).
- float·double·`Vec2/2d`·`Vec3/3d`·`LinearColor`(성분별)만 지원하고 quaternion·`Vec4`는 컴파일 시점에 막는다. UE도 일반 형식(`FQuat` 등)은 아무것도 하지 않는 빈 함수다(`InterpCurvePoint.h` 406–408).
- 구간마다 UE `FindBounds`(`Core/Private/Math/UnrealMath.cpp` 1512–1573)를 같은 식·같은 순서로 옮긴다: 두 끝 값의 min·max, 곡선 키면 접선에 구간 길이를 곱해 Hermite 도함수 계수 `a = 6P0 + 3T0 + 3T1 − 6P1`, `b = −6P0 − 4T0 − 2T1 + 6P1`, `c = T0`, 근이 구간 안(`t > StartT && t < EndT`)이면 `CubicInterp` 값을 넣는다.
- **UE와 다른 점**: UE는 루프 커브의 닫는 구간(마지막 키 → 첫 키)에 첫 키의 입력값을 끝 시간으로 넘겨(`InterpCurve.h` 765–770) 구간 길이가 음수가 되고 극값을 보지 않는다. Rocks는 닫는 구간의 끝 시간을 `마지막 키 + loopKeyOffset`(InterpCurve 평가와 같은 값)으로 둔다(15절 22).

## 13. 좌표·단위·각도

- 스칼라 커브 자체에는 단위가 없다. 커브를 묶는 에셋·애니메이션 타입이 UE 5.8 규약을 그대로 따른다(절대 규칙): 벡터 커브는 성분 X·Y·Z(cm 등 UE와 같은 단위), 변환 커브의 회전 성분은 **도 단위 오일러이고 X = Roll, Y = Pitch, Z = Yaw**(UE `FTransformCurve` — `AnimCurveTypes.cpp` 283·294–297, 평가는 `FRotator(Pitch = Y, Yaw = Z, Roll = X)` → quaternion), 색 커브는 RGBA 4성분이다. Rocks `Rotator`의 필드·적용 순서가 이미 UE와 같다.
- `unwindRotation`(7절)은 도 단위 ±180° 보정이다(UE와 같다, RC 319–334).
- `InterpCurve<Vec3d>`·`InterpCurve<Quatd>` 미러(12절)는 UE `FInterpCurveVector`·`FInterpCurveQuat`와 같은 좌표·단위를 그대로 담는다(변환 없음).
- 이 값 타입 층에는 벡터·회전 키 커브 묶음이 없다. 그 묶음 타입은 14절의 에셋 층에서 만든다.

## 14. 에셋 층 경계(범위 밖)

A1 에셋 시스템 뒤 A 단계에서 아래를 만든다. 이 문서는 그것들이 이 층에 기대는 방식만 정한다.

| UE 5.8 | Rocks에서 이 층과의 연결 |
|---|---|
| `UCurveFloat`(`FRichCurve FloatCurve`) | 에셋 객체가 `TangentCurve` 필드 하나를 갖는다 |
| `UCurveVector`·`UCurveLinearColor` | `TangentCurve` 성분 3·4개(13절 규약) |
| `FRuntimeFloatCurve`(인라인 커브 + 외부 에셋 참조) | 인라인 `TangentCurve` + `SoftObjectRef` 또는 `ObjectRef` 하나 — 참조를 고르는 규칙은 에셋 층이 정한다 |
| `UCurveTable`(`TMap<FName, FRealCurve*>`, 표 전체가 Simple·Rich 모드 하나) | 저장 형태는 에셋 층이 정한다(D2). 후보는 표 단위 모드 + 타입이 정해진 맵 하나(`OrderedMap<Name, TangentCurve>` 또는 `OrderedMap<Name, PlainCurve>`)와 행마다 `InstancedStruct<ScalarCurveBase>`다. UE 표는 행 타입이 섞이지 않으므로 앞쪽이 lease·힙 블록·Record 머리말 비용이 없다 |
| `FCurveTableRowHandle` | 표 참조 + 행 Name |
| `FVectorCurve`·`FTransformCurve`(애니메이션) | 애니메이션 단계 |

다형 저장을 고르는 경우의 평가 규칙(D2): 자유 함수 `TryEvaluateScalarCurve(const InstancedStruct<ScalarCurveBase>&, time, fallback) -> Result<float>`가 담긴 타입이 `TangentCurve`·`PlainCurve`면 평가하고, 비어 있으면 NotFound, `ScalarCurveBase` 자체나 이 함수가 모르는 파생 타입(게임 모듈이 만든 파생 등)이면 Unsupported다. 기본값이나 0으로 조용히 대신하지 않는다. virtual이 없으므로 새 파생 타입을 평가하려면 이 함수의 분기를 넓힌다.

## 15. UE 5.8과 다른 점

기본은 UE와 같은 의미·같은 비트(Windows x64 기준 — D17)다. 아래는 결함 수정(★)이거나 Rocks 규약(◇)이다.

1. ★ 순환 외삽: 키 시간이 모두 같거나(길이 0) 시간이 비유한이면 재배치하지 않는다. UE는 0 나눗셈과 Inf·범위 밖 값의 정수 변환이다(`RealCurve.cpp` 7–34, x64에서는 `cvtss2si` 무효값 → cycle 수 −2^30). cycle 몫이 2^30 이상이면 Rocks는 double 재배치(D16), UE는 SSE 식의 무의미한 값(UPM 86–91 — 예: 몫 −(2^30 + 128)에서 시간 −128)이다.
2. ★ Linear·Bezier의 `Lerp` 끝점: 비율 0·1에서 끝 값을 정확히 낸다(InterpCurve 4절 5와 같은 수정). Lerp·Bezier에 실제로 넣는 비율(가중 경로는 3차 방정식 근을 float로 바꾼 값 — 입력 비율이 0·1이 아니어도 근이 float 1.0이 될 수 있다)이 0·1이 아니면 비트가 같고, 유한한 끝의 차가 넘치는 경우만 Rocks가 유한한 값을 낸다(MathUtility.h 127–131).
3. ★ 가중 접선 3차 방정식에서 근이 2개일 때 초기화되지 않은 세 번째 칸을 읽지 않는다(CE 167).
4. ◇ NaN 시간은 NaN을 낸다(UE는 마지막 키 값). 빈 커브는 NaN이어도 기본값이다.
5. ◇ `PlainCurve`는 Cubic을 표현할 수 없다(`PlainCurveInterp`, UE는 ensure 후 저장값을 Linear로 평가).
6. ◇ `FindKey`는 허용오차 안의 키 중 가장 앞 키를 돌려준다(UE는 탐색 경로에 따라 정해진다).
7. ★ Bake 시간은 누적 덧셈 대신 `first + i × interval`(UE float 누적 오차, `first == last`면 `Reserve(−1)` fatal — RC 900).
8. ★ 범위판 `RemoveRedundantKeys`의 비교 기준을 시작 보존 키로 둔다(UE `MostRecentKeepKeyIndex = 0`, RC 1017).
9. ★ 계단 커브 `RemoveRedundantKeys`가 마지막 남은 키를 기본값으로 바꾸지 않는다(UE `StringCurve.cpp` 232–237·`IntegralCurve.cpp` 251–256). String은 첫 키 이전 평가 결과가 늘 바뀌고, Integer는 `useDefaultBeforeFirstKey`일 때 바뀐다.
10. ◇ 같음은 모든 칸 비교다(실수 비트·String byte·Name은 Name 규칙 — 계단 커브 중복 제거는 표시 철자까지, 17.5절 S5). UE는 가중치·비Cubic 접선·기본값을 빼고, String 키는 대소문자를 무시한다.
11. ★ 압축 키 시간 판정의 표본 구간 탐색(RC 1399–1426).
12. ◇ 압축 바이트는 리틀 엔디언 명시(UE 원시 메모리).
13. ★ 압축 Weighted 평가의 가중 판정 위치·순환 오프셋 누락, 풀기의 접선 손실·Empty 무한 반복·Constant 미처리(RC 2077–2105, 2118, 2441–2452).
14. ◇ 키 핸들은 값 밖의 표이고 표마다 결정적으로 발급한다(UE 전역 atomic). 표 대입이 옛 핸들을 남기지 않고(KH 74–76), 표를 거치지 않은 수정은 표 전체를 무효로 한다.
15. ◇ 숨김 열거자 `RCIM_None`·`RCTM_None`, 보이는 열거자 `RCCE_None`(동작은 Cycle — RC 1115·1139), `SmartAuto`가 없다.
16. ◇ 편집 연산이 접선을 몰래 다시 계산하지 않는다(6절 — `SetKeyTime`·`DeleteKey`·`SetKeys`·`RemoveRedundantKeys` 뒤 결과가 UE와 다르고, 호출자가 `AutoSetTangents`를 부르면 같다).
17. ◇ 기본값 "없음"은 Optional(UE `MAX_flt`·`MAX_int32`·빈 문자열). Name 계단 커브에 기본값이 있다(UE에 없다).
18. ◇ 값 범위는 해석적 극값(UE 표본 재귀).
19. ◇ 리플렉션 경로의 정렬·유한 시간·압축 정합 위반은 InvalidArgument다(UE는 확인하지 않는다).
20. ★ 압축 Constant 형식은 비트 같은 값만(UE 허용오차 1e-8 — RC 1286).
21. ★ 압축 Weighted 형식은 구간 단위 가중 판정(UE는 Cubic 키의 가중 방식만 — RC 1296–1302).
22. ★ `InterpCurve` 값 범위의 루프 닫는 구간에 루프 끝 시간을 쓰고, 키 하나의 루프 커브도 그 닫는 구간을 본다(UE는 첫 키 입력값을 끝 시간으로 넘기고 키 하나면 구간을 보지 않는다 — UE `InterpCurve.h` 746–772).
23. ◇ 허용오차 인자(UpdateOrAdd·중복 제거)가 NaN·음수면 InvalidArgument, `FindKey`는 `IndexNone`이다(UE는 음수를 그대로 써 늘 불일치·늘 보존 — 17.4절 C5).
24. ★ 회전 풀기는 UE가 끝나는 입력에서 비트가 같고, 360을 더하고 빼도 값이 그대로인 큰 값(UE는 끝나지 않는다)·비유한 값은 double 나머지로 옮기거나 그대로 둔다(17.4절 C6).
25. ★ Bake는 표본 2^24개 상한(CapacityExceeded), 표본판 중복 제거는 프레임 번호 int32 끝에서 멈추고 호출 하나 표본 2^24개 상한(CapacityExceeded)이다(UE는 감싸 돌아 끝나지 않거나 상한이 없다 — 17.4절 C9).
26. ★ 중복 제거가 차가 NaN인 키(NaN 값·접선)를 남긴다(UE `> tolerance` 비교는 지운다).
27. ◇ `TrySetKeys`는 정렬·유한 시간을 확인한다(UE `SetKeys`는 확인하지 않는다).
28. ◇ `TryGetTimeRange`·`TryGetValueRange`는 키가 없으면 NotFound다(UE는 (0, 0)).
29. ◇ `TryShiftTime`·`TryScaleTime`은 키 순서를 그대로 둔다(UE `ShiftCurve`·`ScaleCurve`는 키마다 `SetKeyTime`이라 같은 시간이 된 키의 순서가 바뀔 수 있다).
30. ★ 압축 키 시간 판정: 표본 시간은 누적하지 않고(`min + k·(1/rate)` — UE float 누적, RC 1435), errorThreshold·sampleRateHz가 NaN·Inf이거나 표본이 2^24개를 넘으면 Float32다(UE는 0 이하만 보고 표본 수 상한이 없다). 시간이 모두 같은 키(Δ = 0)도 Float32다(17.7절 P4).
31. ◇ 압축 풀기는 Linear 형식의 접선, Linear·Cubic·Mixed 형식의 가중 칸을 0·None으로 푼다(압축이 담지 않는다 — UE도 같다). 압축 칸은 정규형이다(Empty만 기본값, Constant만 상수 칸 — 17.7절 P5).
32. ★ 압축 키 시간 판정(UE `FindRichCurveKeyFormat`)의 손실 키 전진 뒤 범위 검사가 `CurrentLossyKey >= NumKeys`라, 복원한 마지막 키 시간이 max보다 작으면(1 ulp — 예: Linear 키 (−3, 0)·(6.4, 1), 1e-4·120 Hz) `Keys[NumKeys]`를 읽는다(RC 1413–1421 — 배열 밖 읽기, check 빌드는 assert). Rocks는 표본을 `Evaluate`와 같은 평가로 보므로 해당 없고, 시험 참조는 `CurrentLossyKey + 1 >= NumKeys`로 막아 옮겼다(17.7절 P10).
33. ◇ 압축 키 시간 양자화의 Clamp는 UE `FMath::Clamp`(Max(Min(X, 1), 0) — NaN이면 1)와 같은 선택이다(17.7절 P4). Rocks `math::Clamp`(NaN 그대로)를 쓰지 않는다.

## 16. 시험 계획

`Core/Source/Runtime/Core/Tests/MathKeyCurve*Tests.cpp`(수학 층), `Core/.../Tests/FrameRateTests.cpp`, `Core/Source/Runtime/Curves/Tests/`(값 타입), 리플렉션 보강은 ObjectRuntime·ObjectSerialization·ReflectionText·ToolTests의 해당 시험 파일. 무작위 사례는 고정 seed이고 타이밍에 기대는 시험은 없다.

- **UE 식 참조 구현 대조(golden)**: 시험 안에 UE 5.8 식을 그대로 옮긴 참조(`RemapTimeValue`·`CycleTime`·`Eval`·`EvalForTwoKeys`·`BezierInterp`·`WeightedEvalForTwoKeys`·`SolveCubic`·`FMath::SinCos(float)`·`AutoSetTangents`·`ComputeCurveTangent`·`RemoveRedundantKeysInternal`, `FSimpleCurve::Eval`, Integral·String 평가, 압축 형식·양자화·압축 평가, `FInterpCurve::CalcBounds`·`FindBounds`)를 두고 15절의 수정을 하나씩 켜고 끌 수 있게 한다.
  - 참조의 `FloorToInt`는 Windows x64 UE와 같은 SSE 식 `cvt(F + F − 0.5f) >> 1`(UPM 86–91)로 옮긴다(일반식이 아니다). 참조의 `SinCos`는 UMU 760–798 다항식이다.
  - Rocks 의미(수정을 켠 참조): 모든 커브·모든 입력에서 **비트 단위로 같다**.
  - UE 그대로(수정을 끈 참조): Lerp·Bezier에 넣는 비율(가중 경로는 float로 바꾼 근)이 0·1이 아닌 입력, 길이가 0이 아닌 구간, 근이 2개가 아닌 가중 구간, |cycle 몫| < 2^30, 끝의 차가 넘치지 않는 유한 입력에서 비트 단위로 같다 — 이것이 UE 비트 대조의 실제 범위다. 가중 경로의 `atanf`·`pow`·`acos`·`cos`·`sqrt`는 Windows x64(MSVC 런타임)에서만 비트 대조하고, 다른 플랫폼은 상대 오차로 본다(D17).
  - 반례로 UE 결함을 확인한다: 길이 0 순환, 근 2개 가중, 압축 Weighted의 구간 1 이상, 범위판 중복 제거, 계단 커브의 마지막 키 기본값 변환, Linear 키 도착 가중의 압축 형식, 1e-8 안 값 차의 Constant 압축, 루프 InterpCurve의 닫는 구간 범위.
- **독립 고정 벡터(손 계산)**: 참조와 구현이 같은 오독을 공유하는 것을 막으려고 참조 코드를 거치지 않은 기대값 표를 둔다. 각 갈래마다 몇 개씩 — 외삽 5종의 대표 시간, 2키 Linear·Constant·Cubic 비가중(손으로 Bezier 전개), 가중 구간의 단순한 경우(접선 0·가중치 1/3 거리 — 비가중과 같은 곡선이 되는 사례와 손으로 푼 3차식 근), `SinCos` 다항식의 몇 각도(계수 대입 손 계산), 자동 접선 3키, 양자화 경계, 압축 byte 배치(손으로 쓴 byte 열), `FrameRate::AsFrameTime`의 subframe 경계. 값은 문서·주석에 계산 과정과 함께 적는다.
- **참조 코드 원본 대조 리뷰**: 시험의 UE 참조 코드는 구현자가 아닌 리뷰어 1명이 UE 원본(위 파일·줄)과 줄 단위로 대조한다(CV1·CV5·CV8·CV9 단계 리뷰에 포함). 대조표(참조 함수 ↔ UE 파일:줄)를 시험 파일 머리 주석에 둔다.
- **경계**: 빈 커브(기본값 있음·없음), 키 하나(모든 외삽), 같은 시간 키(삽입 위치·평가 구간·FindKey, 첫 키·마지막 키 시간 중복), 키 시간 정확히·끝 키 시간·바로 앞뒤, 외삽 조합(앞·뒤 × 5종)과 순환 횟수 경계(정확히 n 주기 — int32 경로와 double 경로 모두에서 max로 가는 규칙), CycleWithOffset 누적, Oscillate 짝·홀수, Linear 외삽의 두 키 기울기·`|dt| <= 1e-8`, NaN·±Inf, **큰 시간·작은 간격의 cycle 몫 넘침**(몫 2^30 직전·직후, 2^31 직전·직후, 1e30 — Fatal 없음과 double 재배치 결과), `PlainCurve` 첫·끝 키 시간 갈래.
- **보간·접선**: Constant 구간의 `time < t2` 비교, Linear, `Lerp` 끝의 차 넘침, Cubic 비가중 = 독립 double Bezier 참조와 상대 오차, 가중 4방식 × 좌우 조합과 뒤 키가 Linear인 도착 가중 구간, 가중 경로의 double 시간 혼용(float로만 계산한 결과와 늘 같음을 확인 — 17.1절 K5), 자동 접선(tension 0·0.5·1, Constant 이웃의 User 키, 첫·끝 키), User·Break 보존.
- **편집**: 삽입 위치, unwind ±180° 경계, UpdateOrAdd 허용오차, SetKeyTime 칸 유지, SetKeys 정렬·비유한 거부, Shift·Scale 순서 유지·factor 거부, 선택 키 이동의 결과 순서, Bake 시간 식과 `first == last`, RemoveRedundantKeys(표본 있음·없음·범위판, 뒤이은 `AutoSetTangents`와 UE 결과 비교), 값 범위(비가중·가중 단조·비단조 — 비단조는 촘촘한 Evaluate 표본의 최소·최대가 결과 범위 안이고 그 끝에 닿는지), 할당 실패 주입 뒤 커브 불변.
- **FrameRate**: `AsFrameTime`·`AsSeconds` UE 식 대조(subframe 반올림·`MaxSubframe` 상한·int32 범위 clamp), 0 분모 거부.
- **키 핸들 표**: 발급 단조·0 무효·Overflow, 삽입·삭제·이동 뒤 핸들 → index, 표를 거치지 않은 편집(C++ API·경로 쓰기·대입 묶음) 뒤 StaleHandle과 `TryRebuild`, 표 복사·대입이 같은 핸들.
- **리플렉션**: 각 struct 그래프 왕복과 다시 저장 byte 같음, 상속 필드 ID(base 소유), private `keys_`의 도구 이름 `keys`, 키 Atomic, JSON 왕복, 정렬·비유한 시간 위반의 decode·경로 쓰기·대입 묶음·JSON import 거부(값 불변), 중첩 위치(Array·Optional·Map 값·InstancedStruct 안 커브)의 hook 호출, hook을 worker 스레드 decode에서 부르는 경로, `PlainCurveInterp` 미선언 값(2) decode 거부, Name 계단 커브의 대소문자만 다른 값 같음, 저작 키 wire 크기 실측(9.2절).
- **바이트 값 종류(D18)**: `Array<uint8>` 필드·멤버·원소 자리의 Bytes wire 왕복, 길이 0, 잘린 입력, 크기(원소당 1 byte), 경로·JSON 표기.
- **압축**: 형식 결정 표(구간 단위 가중, 비트 같음 Constant), 양자화 경계(오차 직전·직후), 키 데이터 배치 golden byte(보간 byte = 형식 값), Float32 압축 평가 = 원본 비트 같음(가중 포함), Quantized16 오차 ≤ threshold, 풀기 왕복(접선·외삽·기본값 보존, Constant·Empty), **변조된 packed**(keyCount·크기 불일치, 잘못된 보간 byte, 비정렬 시간)의 decode 거부와 평가 진입 대조(NaN·InvalidArgument, 메모리 밖 읽기 없음 — Asan 시험 대상에 넣는다).
- **InterpCurve 미러·범위**: 다섯 형식 그래프·JSON 왕복, 루프 칸, 정렬 위반 거부, `TryCalcBounds` UE 식 대조(곡선·Linear·Constant 키, 루프 닫는 구간 수정), quaternion `TryCalcBounds` 컴파일 거부(ToolTests).

## 17. 구현 단계

단계 기호는 CV(CurVe)다. 리플렉션 확장 문서의 CU(UI·Undo 소비자 단계)와 겹치지 않게 했다. **교차검증은 단계마다** 리뷰어 1명이 한다(구현 → Debug·format·policy·바뀐 부분 시험 → 리뷰 → 반영). 스펙·문서 관점 리뷰는 묶음 끝 CV10에서 한 번 한다.

| 단계 | 범위 | 선행 | 규모 | 상태 |
|---|---|---|---|---|
| CV1 | Core 수학 층 `KeyCurveMath.h`: `CycleCurveTime`(D16)·`EvalCurveSegment`·`BezierCurveValue`·`CurveSinCos`·`WeightedCurveSegment`·`SolveCurveCubic`·`ComputeCurveTangent`, UE 식 참조 구현·독립 고정 벡터·golden 시험, 참조 원본 대조 리뷰 | — | M | 완료 `b82396df`·리뷰 `bdbf2569` |
| CV2 | Core `FrameRate`·`FrameNumber`·`FrameTime`(D20) | — | S | 완료 `b82396df` |
| CV3 | 리플렉션 struct 검증 hook(D7): MetaCompiler 감지·RMC 진단, `RuntimeStructOps::validate`, graph decode·InstancedStruct·Optional·Map·배열 codec, 대입 묶음·JSON import, 경로 쓰기의 사본 적용, ReflectionExtensions 개정 | — | L | 완료 `a06c0b76` |
| CV4 | 리플렉션 `Array<uint8>` Bytes 블록 wire(D18): Serialization·codec·경로·JSON 표기·성질 표, ReflectionExtensions 개정 | — | M | 완료 `797bb1f9` |
| CV5 | `Curves` 모듈, `ScalarCurveBase`·`TangentCurve`·`PlainCurve`와 키, 평가·자동 접선·편집 연산·중복 제거·값 범위, 리플렉션 왕복·JSON·hook 시험, 키 wire 실측(D8) | CV1·CV2·CV3 | L | 완료 `518a7f97` |
| CV6 | 계단 커브 셋(Integer·Name·String) | CV5 | S | 완료 `ed0e2b8b` |
| CV7 | `CurveKeyHandleTable`과 표 판 편집 함수 | CV5 | S | 완료 `1f36afed` |
| CV8 | `PackedTangentCurve` 형식 결정·양자화·배치·평가·풀기, 정합 hook과 평가 진입 대조(D21), golden byte 시험 | CV4·CV5 | M | 완료 `b958ed24` |
| CV9 | `InterpCurve<T>` 미러 다섯 형식(미러 private 멤버 접근 보강, `InterpCurveMode` enum 미러), `TryCalcBounds`(D19) | CV3 | M | 완료 `c257e5f4` |
| CV10 | 문서 반영(에셋 시스템 Curve 행·Core 상세 G09·InterpCurve 구현 문서·ReflectionExtensions 정리·이 문서의 구현 결과), 묶음 끝 스펙·문서 관점 리뷰 | CV1–CV9 | S | 완료(개정 12) — 묶음 끝 리뷰 대기 |

### 17.1 구현(CV1·CV2)에서 정한 점

CV1(`Core/Math/KeyCurveMath.h`)·CV2(`Core/Time/FrameRate.h`)를 구현하며 정했거나 문서와 다르게 둔 점이다. 시험은 `Core/Tests/MathKeyCurveGoldenTests.cpp`(UE 참조 대조 — 머리 주석에 참조 함수 ↔ UE 파일:줄 대조표), `MathKeyCurveTests.cpp`(독립 고정 벡터·경계), `FrameRateTests.cpp`다.

- **K1 키 형식** — 수학 층은 키 struct를 새로 만들지 않고 concept `math::CurveSegmentKey`로 받는다: float 칸 `time`·`value`·`arriveTangent`·`arriveTangentWeight`·`leaveTangent`·`leaveTangentWeight`와 enum 칸 `interp`·`tangentWeight`. enum 값은 번호로 비교한다(`math::CurveInterpLinear 0·Constant 1·Cubic 2`, `CurveWeightNone 0·Arrive 1·Leave 2·Both 3` — UE와 같은 번호, 2절). CV5의 `TangentCurveKey`(4절 칸 이름)와 압축 커브를 푼 키가 변환 없이 이 형식이다(CV5에서 enum 번호를 static_assert로 묶는다). Core에 `CurveInterp`와 같은 enum을 하나 더 두지 않으려는 선택이다.
- **K2 순환 API** — `CycleCurveTime(min, max, time) -> CurveCycle{time, cycleCount(double), wide}`와 `CurveCycleValueOffset(cycle, valueDelta)`·`IsOddCurveCycle(cycle)`로 나눴다. 외삽 방식을 보고 앞·뒤를 고르는 `RemapTimeValue`(RC 1104–1161)는 외삽 enum이 있는 CV5 값 타입에 둔다. 오프셋은 int32 경로면 UE와 같은 `delta × (float)주기`, double 경로면 double 곱의 float 반올림이다. 주기 수는 늘 double이라 int32 최솟값 몫의 절댓값(2^31)도 넘치지 않는다(UE `FMath::Abs(int)`는 감싸 돈다).
- **K3 double 재배치 식(5.1절 보강)** — 나머지는 `Fmod`(정확)로 구한다: `r = Fmod(d, 길이)`, `q = Floor((d − r)/길이 + 0.5)`, `r < 0`이면 `r += 길이, q −= 1`. 개정 2의 `r = d − q × 길이`는 큰 몫에서 정확하지 않아 5.1절을 이렇게 고쳤다. UE의 끝 바로잡기(앞쪽에서 온 max는 min, 뒤쪽에서 온 min은 max)는 int32 경로에만 한다(단계 리뷰 반영 — double 경로에서 반올림으로 max가 된 앞쪽 시간을 min으로 바꾸면 Cycle·Oscillate 결과가 뒤집힌다. 예: min = 2 − 2^-23, max = 2 + 2^-21, 시간 −2052). 처음 구현의 "반올림으로 `r ≥ 길이`" 갈래는 지웠다: double 경로는 d ≥ 길이·2^30이라 d의 double 간격이 길이의 float 간격 이상이고 나머지는 그 간격의 배수라 0이 아니면 |r| > 길이·2^-24다 — 음수 나머지에 길이를 더해도 길이가 되지 않는다. int32 경로는 |몫| < 2^30으로 좁혔다(단계 리뷰 반영 — [2^30, 2^31)에서 float 식이 범위 밖 시간을 내고, 정확한 배수의 주기 수가 double 규칙과 어긋나 Oscillate 홀짝·CycleWithOffset이 달라졌다. 예: (0, 0.75, 805306432) → 시간 −64, (0, 1, 2147483520) → 주기 2147483520 대신 2147483519). 재배치하지 않는 입력에 **길이가 비유한인 경우**(키 시간이 ±3e38처럼 멀어 차가 넘침)를 더했다.
- **K4 가중 근 고르기** — 5.4절의 "가장 큰 근"은 대략이고, 구현은 UE 조건 `(근 ∈ [0, 1] ± 1e-8) && (고른 값 < 0 || 근 > 고른 값)`을 그대로 쓴다(5.4절 문구를 고쳤다). 근이 2개면 두 칸만 본다(15절 3). 이 반례는 시간 제어점 (0, 1.5, −0.5, 1)·α = 극댓값 x(s₁) ≈ 0.5945로 근 2개를 만들어 시험한다(UE가 세 번째 칸 0.999를 고르는 경우).
- **K5 double 시간 혼용** — 개정 2의 "float끼리 계산하면 드물게 비트가 달라진다"는 틀렸다. 해당 식(`c·w + t1`, `t2 − t1`, `tanX − t1`)은 모두 float 두 값의 연산 한 번이라 double로 계산해 float로 반올림한 값이 float 연산과 늘 같다(이중 반올림 무해 조건 53 ≥ 2·24 + 2). 구현은 UE와 대조하기 쉽게 double 꼴을 유지하고, 16절의 "갈리는 입력을 찾아 고정" 시험은 "같음을 확인"으로 바꿨다(무작위 5만 사례, 큰 시간·짧은 구간 포함).
- **K6 `CurveSinCos`의 정수 변환** — UE의 `(int64)` 변환은 범위 밖·NaN에서 C++ 미정의 동작이다. Rocks는 x64 `cvttss2si`가 내는 정수 무효값(int64 최솟값)을 명시적으로 내서 UE Windows x64와 비트를 맞춘다. 그래서 NaN은 NaN이지만 ±Inf는 NaN이 아니라 의미 없는 값(+Inf 등)이다(UE와 같다). 가중 경로는 `atanf` 각도(|x| ≤ π/2)만 넣으므로 영향이 없다.
- **K7 공개 함수 경계** — `WeightedCurveSegment`도 길이가 0 이하·NaN이면 앞 키 값을 낸다(UE 함수는 확인하지 않지만 `EvalCurveSegment`가 먼저 걸러 결과는 같다). `SolveCurveCubic`은 `CurveCubicRoots{roots[3], count}`를 돌려주고 쓰지 않은 칸은 0이다. `ComputeCurveTangent(previousTime, previousValue, currentValue, nextTime, nextValue, tension)`은 clamp 없는 경로에서 쓰지 않는 현재 키 시간을 받지 않는다. `IsCurveSegmentUnweighted`를 공개해 CV8 압축 형식 결정(11.1절)이 같은 판정을 쓴다.
- **K8 UE 참조의 범위** — 시험 참조의 "수정 켬" 순환은 몫이 2^30 미만인 경우만 옮겼다. double 재배치는 참조 대신 손 계산 벡터(2^33 시간·길이 0.75 → 시간 0.5·주기 11453246122, 몫 2^30 바로 앞(−(2^30 − 64) → int32 경로)·2^31 − 128·2^31·2^31 ± 256 경계, 1e30·길이 2^-20, 위 리뷰 반례 둘)로 확인하고, int32 경로 결과는 곱이 정확한 입력(2^-6 격자)에서 늘 키 범위 안이고 |몫| ≤ 2^22에서 벗어남이 |time|·2^-21 안인지 무작위로 단언한다. 반례: 몫 −(2^30 + 128)에서 UE는 시간 −128(범위 밖)이고 Rocks는 double 경로로 min·주기 2^30 + 128, 길이 0에서 UE는 시간은 그대로지만 주기 2^30이다.
- **K9 FrameRate 최소 범위(D20)** — `FrameRate`(`TryCreate(분자, 분모)` — 둘 다 양수만, 기본 60000/1은 UE와 같다, `AsInterval`·`AsDecimal`·`AsSeconds`·`TryAsFrameTime`·`AsFrameTime`), `FrameNumber`(int32, `+`·`−`·`++`는 넘치면 Fatal, `TryAdd`·`TrySubtract`는 Overflow — TimeSpan과 같은 규약), `FrameTime`(프레임 + subframe, 생성자는 UE와 같은 `(s + 0.5) − 0.5` 뒤 [0, MaxSubframe] clamp — NaN은 UE Clamp처럼 MaxSubframe, `SetFrame`·`AsDecimal`·비교)만 두었다. UE의 `AsFrameNumber`·`TransformTime`·`Snap`·`FrameTime` 산술(`+`·`−`·`%`·배율)·`FromDecimal`·표시 문자열은 소비자가 생길 때 더한다.
- **K10 `AsFrameTime`의 비정상 입력** — NaN은 `TryAsFrameTime`이 InvalidArgument(`AsFrameTime`은 Fatal — 검증된 키 시간용), ±Inf는 int32 끝 프레임 + subframe 0이다(UE는 NaN subframe을 정수로 바꿔 의미 없는 값). subframe 올림(float 반올림으로 비율 1)은 |프레임| < 2^28에서만 생기므로 int32 끝에서 넘치지 않는다. int32 밖 시간의 subframe은 UE처럼 원래 비율이다(3e9 + 0.25초 → 프레임 int32 최댓값, subframe 0.25).
- **K11 FrameTime 비교** — UE와 같이 프레임 다음 subframe 순이다. subframe은 생성자에서 NaN이 될 수 없어 비교가 늘 정의된다.
- **K12 19절 1번 확인 결과** — `atanf`·`acos`·`cos`·`pow`·`sqrt`·`sqrtf`를 무작위 비트 입력 각 200만 개로 MSVC 14.29(`/O2`·`/Od`, `/fp:precise`·`/fp:fast`, `/arch:AVX2` 유무)와 Rocks clang-cl(`/O2`·`/Od`·AVX2, `-ffp-contract=off`)에서 실행해 결과 비트를 비교했다. 릴리스 C 런타임(`/MD` — `ucrtbase.dll`)이면 모든 조합이 **비트까지 같다**(두 컴파일러 모두 같은 `ucrtbase.dll` 함수를 import한다). 그래서 D17의 Windows x64 비트 목표를 가중 경로에서도 유지한다. 조건:
  - 디버그 C 런타임(`/MDd` — `ucrtbased.dll`)은 다른 구현이다: `acos` 824건·`pow` 3,761건(200만 중)이 1ulp 다르다(MSVC·clang 모두). Rocks Debug 구성은 `/MDd`라 Debug 실행 결과는 UE(릴리스 런타임)와 가중 경로에서 1ulp 다를 수 있다. golden 시험은 참조와 구현이 같은 런타임을 쓰므로 구성과 무관하게 비트가 같다.
  - UE 게임 타깃의 기본 부동소수점 의미는 `/fp:fast`다(`VCToolChain.cs` 1313–1345 — Editor·Program 타깃만 `/fp:precise`). `/fp:fast`는 식의 재배열·축약을 허용하므로 UE 게임 빌드 자체가 소스 순서와 비트가 다를 수 있다. Rocks 비트 기준은 UE `/fp:precise` 빌드(편집기)다.
  - `ucrtbase.dll`은 CPU에 따라 FMA3 경로를 고를 수 있어 기계 사이의 비트는 보장하지 않는다(같은 기계에서는 두 빌드가 같은 경로를 탄다 — UE와 같은 수준).
  - UE 5.8은 VS2022 컴파일러지만 수학 함수 구현은 컴파일러가 아니라 OS의 `ucrtbase.dll`이 정하므로 VS2019 14.29로 한 확인이 유효하다고 본다(컴파일러 내장 대체가 없음을 import 표로 확인).

- **K13 단계 리뷰 반영(2026-11-18)** — 위 K3·K8의 순환 수정 외에: `BezierCurveValue` 주석과 15절 2·16절의 "UE와 비트가 같은 범위"를 입력 비율이 아니라 Lerp·Bezier에 실제로 넣는 비율(가중 경로는 (float)근)로 고쳤다(golden `InsideUeBitRange`가 참조의 실제 비율로 가른다, 고정 사례 k1(0, 0.3, 출발 0.5, Both, 0.2)·k2(1, 0.0012345, 도착 −2, Both, 3.3333333)·시간 0.99999994 — 근 0.99999998667이 float 1.0). `EvalCurveSegment`의 NaN 주석을 고쳤다: Constant 구간의 NaN 시간은 비교가 거짓이라 k2.value다(UE와 같다) — 5.6절 NaN 규칙은 CV5 Evaluate가 구간을 고르기 전에 확인한다. 시험 공백을 메웠다: 가중 경로의 NaN·±Inf 시간·접선·가중치, 가중 방식 4 × 4 조합이 모두 나왔는지 단언, 대조표의 `(int64)` 위치(UMU 766·770). golden 참조 이름공간을 `ue58curve`로 바꿨다: InterpCurve golden(`MathInterpCurveGoldenTests.cpp`)의 `rocks::test::ue58`과 이름이 겹쳐 `Options`가 서로 다른 정의를 가진 ODR 위반이었고(링커가 inline 함수 한 정의를 고른다), `Options`에 칸을 더하자 가중 시험이 segfault로 드러났다(CV1 커밋에 잠복).

### 17.2 구현(CV3)에서 정한 점

struct 검증 hook(9.1절, D7)을 리플렉션 기반에 넣었다. 기반 쪽 요약은 [리플렉션 보강](ReflectionExtensions.ko.md) 23절이고, 시험은 `ObjectSerialization/Tests/ValidateHookGraphTests.cpp`(decode)·`ValidateHookPathTests.cpp`(경로 쓰기·Attach)·`ReflectionText/Tests/TextValidateHookTests.cpp`(대입 묶음·JSON import)·ToolTests `Meta.ValidateHook*`(생성·RMC2309), fixture는 `ReflectionTestFixtures/ValidateFixture.h`다.

- **V1 선언과 진단(RMC2309)** — `ROCKS_STRUCT`의 `static Result<void> Validate(const S&) noexcept` 하나를 MetaCompiler(Clang AST)가 감지한다. 함수 템플릿·overload(두 번째 `Validate` 선언)·비정적·시그니처가 다른 선언(인자·반환·noexcept)·`ROCKS_GENERATED_BODY` 없는 private/protected는 RMC2309다(조용히 hook 없이 등록되지 않는다). 생성 코드도 `static_assert`로 정확한 함수 형식을 대조한다. Binds 미러의 `Validate`는 CV9(InterpCurve 미러)까지 RMC2309로 거부한다(12.1절 — 그 단계에서 실제 타입 인자 hook을 연다).
- **V2 hook struct는 참조 없는 값이다** — 강한 참조·Registry 컨테이너를 담은 struct의 hook은 RMC2309, 상속한 hook(가져온 base 포함)은 생성 `static_assert`(강한 참조 없음·`ValueCopyable`), 수동 등록은 TypeRegistry가 InvalidArgument로 막는다(`validate`가 있으면 `validateTree`·`copy`·`swap`도 있어야 한다). 9.1절 계약상 hook은 Registry를 읽지 못하므로 참조는 검증 대상이 될 수 없고, 이 제약 덕분에 대입 묶음의 값 단위(첫 복사 가능 단계)가 늘 hook struct를 통째로 담는다 — 상자 검증만으로 "바뀐 위치를 감싼 hook 전부"가 덮인다.
- **V3 런타임 표** — `RuntimeStructOps::validate`는 자기 수준 hook(base 사슬의 `Validate` 다음 자기 `Validate` — 파생 struct의 decode가 base 불변식도 본다), `validateTree`는 값 트리 전체(base 트리 → PROPERTY 멤버 순서(bitfield 제외) → 자기 `Validate`, 트리에 hook이 없으면 null). `RuntimeValueOps::validateTree`(상자·필드 값용)와 `detail::ValueValidate<T>` 특성(Array·OrderedMap의 키와 값·OrderedSet·Optional·정적 배열·InstancedStruct는 담긴 타입의 등록 `validateTree`)을 더했다. 생성 thunk가 hook의 다른 오류 코드를 InvalidArgument로 바꾼다(`detail::StructValidationResult`). NATIVE 멤버는 리플렉션 경로가 바꾸지 않으므로 보지 않는다.
- **V4 graph decode** — `DecodeStructValue`(모든 struct 자리 — 필드·멤버·Array·정적 배열·StructRefArray 원소·Optional 내부·Map 값·InstancedStruct 내용이 지나는 단일 지점)가 적용 단계(2단계 — construction·값이 있다)에서 마이그레이션·멤버 해석 뒤 `validate`를 부른다. 멤버 struct가 먼저 해석되므로 안쪽부터다. 거부하면 복원 전체가 InvalidArgument이고 트랜잭션이 되돌린다(대상 Registry에 아무것도 게시되지 않는다). 1단계는 값이 없고(참조도 token이다) 2단계는 진단을 버리므로 진단 경로는 남기지 않는다(복원 오류 코드뿐).
- **V5 대입 묶음·JSON import** — `Publish`가 게시 자리를 찾기 전에(단계 0) StructRefArray 추가 원소(블록에 바로 적용된다 — `PreparedStructElements::ValidateAdded`)를 보고, 값 단위 상자는 `PathPrepareAttach`가 상자 값 트리를 검증한다. 거부는 진단 `InvalidStructValue`(경로 = 단위 경로 — 그 struct를 담은 필드, 위치 없음)와 InvalidArgument이고 아무 단위도 게시하지 않는다. 검증 범위는 단위 상자 트리 전체다(바뀌지 않은 형제 hook struct도 본다 — 9.1절 범위의 상위 집합이고, 그 값들은 C++ API나 앞선 검증이 지킨 값이라 통과한다). 같은 값의 무동작 단위는 게시하지 않으므로 hook도 부르지 않는다. 묶음 안에서 잠시 깨졌다가 끝에 맞는 대입(low를 먼저 올림)은 통과한다.
- **V6 경로 쓰기** — 공개 경로 변경 연산(`PathWrite`·`PathInsertElement`·`PathRemoveElement`·`PathClear`·`PathEmplaceOptional`·`PathResetOptional`·`PathInsertKey`·`PathEraseKey`·`PathSetInstancedType`·`PathClearInstanced`)이 객체(게시·staged) 시작 위치에서 hook struct 안을 바꾸면, 실행기가 해석만 하고 가장 바깥 hook struct 단계 H를 돌려준다. 진입점은 H 단계까지의 경로(H의 선택자는 뗀다 — 원소·Map 값이면 그 컨테이너 필드 통째)를 상자로 분리하고, 상자 위에서 같은 연산을 한 뒤, `PathPrepareAttach`(상자 트리 검증)를 거쳐 게시한다(게시된 객체는 묶음 게시 구간 — 변경 표지·관찰자 규칙은 대입 묶음과 같다, staged는 바로 맞바꾼다). 거부는 InvalidArgument(`PropertyPathError::StructValidation`)이고 객체는 그대로다. 상자 위 연산의 실패 단계는 원래 경로 단계로 옮겨 알린다. 비용: hook struct 안 원소 하나를 써도 그 컨테이너 필드를 통째로 복사한다(편집기 쓰기 빈도에서 문제 없다 — 9.1절 비용). 분리된 값(struct 값·상자)과 StructRefArray 추가 원소 블록 시작 위치의 쓰기는 게시가 아니므로 검증하지 않는다(묶음 `Publish`가 본다).
- **V7 `PathPrepareAttach`** — 객체(게시·staged) 자리에 놓는 상자는 상자 값 트리를 검증한다. 자리 위(바깥)에 hook struct가 있으면 상자만으로 감싼 hook을 검증할 수 없으므로 Unsupported(`PropertyPathError::InsideValidatedStruct` — 그 hook struct 단계)다. 대입 묶음 단위와 V6의 분리 자리는 언제나 가장 바깥 hook 이상이라 걸리지 않는다. `DetachedValue::ValidateTree()`를 공개했다(묶음이 진단용으로도 쓸 수 있다).
- **V8 시험 관찰** — fixture hook은 호출 순서 시험을 위해 스레드별 기록(`CurrentValidateLog`)에 글자를 남긴다(계약상 실제 hook은 전역 상태를 쓰지 않는다 — 시험 관찰용 예외).

- **V9 단계 리뷰 반영 — 값 대상·Attach·준비물(2026-11-18)** — 리뷰 항목마다 시험으로 먼저 재현한 뒤(수정을 되돌린 빌드에서 새 시험이 실패) 고쳤다.
  - (1, 재현) struct 값 대상(`AssignmentTarget::Value` — 설정 층·콘솔 명령 인자)의 대입 묶음·JSON import가 hook을 부르지 않았다. `PathPrepareAttach`가 struct 값 시작 위치에서도 상자 트리를 검증하고 자리 위 hook을 거부한다. 루트 struct 자신에 hook이 있으면 Publish가 루트의 깊은 사본(대상 값의 allocator)에 모든 단위를 놓고 사본 트리를 검증한 뒤 루트와 통째로 맞바꾼다(거부는 진단 `InvalidStructValue`, 경로 빈 문자열 — 대상 그대로). 설정 층 적용(`SectionApplier::FinishPrepared`)은 게시 때의 진단도 설정 진단(FieldValue + `InvalidStructValue`)으로 옮긴다. 콘솔은 이미 게시 실패 진단을 옮기고 있었고 같은 Value 대상 경로라 함께 고쳐진다. 시험: Value·staged 대상(`TextValidateHook_ValueAndStagedTargets`), 설정 층(`Config_StructValidationHookRejectsLayerValue` — hook 멤버를 담은 섹션 `SpanSettings`는 그 시험만 등록한다).
  - (2, 재현) `PathPrepareAttach`가 자리 바로 위 InstancedStruct에 담긴 hook struct를 놓쳤다(`any`에 ValidBox, `any.range` 상자의 range.high > limit). hook 단계 탐색이 검사 범위와 경로 전체 단계 수를 따로 받는다.
  - (3, 재현) `PreparedStructElements`를 직접 쓰면 추가 원소가 검증 없이 게시됐다. `PreparePublish`·staged `Publish()`가 아직 검증하지 않았으면 `ValidateAdded`를 부른다(게시 전 1회 — 대입 묶음이 먼저 부르면 다시 부르지 않고, `Append`·`AddedTarget`·`Reset`이 표시를 내린다). 묶지 않은 준비물의 `ValidateAdded`는 InvalidArgument다.
  - (4, 재현) staged 객체의 상자 경로가 Required 검사 중 Busy 규칙을 우회했다. `PreparedAttach::Publish()`의 staged 갈래가 staged 쓰기 판정(`CheckStagedWrite` — 검사 중 Busy)을 따르고, 상자 경로 진입 전에 쓰기 접근(`GetForWrite`)을 먼저 본다.
- **V10 단계 리뷰 반영 — 상자 경로 의미** —
  - (5, 재현) 값을 만드는 연산(원소 넣기·키 넣기·Optional 만들기·형 바꾸기)이 hook 트리가 있는 struct 기본값을 만들면 끝 단계를 상자 경로로 보내 기본값도 검증한다(기본값이 어기는 `ValidNonEmpty`를 만드는 네 연산이 거부되고 그대로다). 예외: Registry 컨테이너(StructRefArray — 참조 struct 원소)는 상자로 분리할 수 없어 경로 원소 넣기가 기본 원소를 제자리로 만든다(그 원소의 hook 멤버는 대입 묶음의 추가 원소 검증과 그 뒤의 경로 쓰기가 본다).
  - (6, 재현) 상자 경로가 실패해도 출력 인자(previous·removed·length)를 바꿨다(PropertyPath.h 계약 위반). 출력은 지역 값에 받아 두고 게시가 끝난 뒤 옮긴다.
  - (7, 재현) 바뀐 것이 없는 연산(이미 있는 키의 InsertKey)도 게시해 변경 표지·관찰자 알림을 냈다. 그런 연산은 게시하지 않는다. 경로 쓰기의 검증 범위는 9.1절보다 넓다 — 분리한 자리(원소면 그 컨테이너 필드 통째) 안의 모든 hook struct(형제 원소 포함)를 본다(V6 보강).
  - (8, 부분 재현) 우회 판정이 끝 지점 분류·검증보다 먼저 돌아 값싼 거부보다 깊은 복사가 먼저였고 오류 순서가 경로마다 달랐다. 판정을 분류·검증(FixedLength·NotOptional·ReadOnly 등) 뒤, Borrow 직전으로 옮겼다. staged 객체는 쓰기 접근 판정(`GetForWrite` — Required 검사 중 Busy 등)을 상자 분리보다 먼저 본다. 게시된 객체는 쓰기 Borrow가 변경 표지를 올리므로 먼저 얻지 않는다 — 같은 코드(encode 고정 Busy는 묶음 게시 구간, 파괴·무효 handle은 분리의 읽기 Borrow)가 분리 뒤에 나온다(깊은 복사가 먼저일 수 있다). `PathSetInstancedType`도 이제 같은 진입점(우회 판정)을 지난다(수정 전에는 hook struct 안의 형 바꾸기도 검증하지 않았다). 재현 시험(encode 고정 Busy)은 게시된 객체에서는 수정 전에도 Busy였고 staged Required 검사 중 표시에서 갈렸다(4와 같은 시험).
  - (11, 재현) staged 객체의 상자 게시에 재확인이 없어 상자 연산 중 할당 콜백이 같은 트랜잭션에 쓴 변경을 덮었다. `ConstructionContext::WriteCount()`(staged 쓰기 접근 누계)를 분리 뒤와 게시 직전에 대조해 다르면 Busy로 게시하지 않는다(그 트랜잭션의 다른 staged 쓰기도 막는 보수적 판정이다).
- **V11 단계 리뷰 반영 — 진단·MetaCompiler** —
  - (9, 재현) 추가 원소 거부 진단의 경로가 배열 필드였다. 거부한 원소의 지금 index를 붙인다(`elements[0]`). decode 거부는 진단 경로 없이 오류 코드만 남는다(V4 — 9.1절 "진단 경로는 그 struct의 위치"는 대입 묶음·JSON import에만 해당한다).
  - (10, Clang AST로 재현) 숨은 friend `Validate`(FriendDecl이라 조용히 무시), `::rocks::Result<void>`·`const Self&` 철자(정규식 불일치로 거부), consteval·`using Base::Validate`(생성 코드 컴파일 오류)를 고쳤다: friend·using-declaration·consteval은 RMC2309, 철자는 쓴 철자·desugar 철자·struct 안 별칭을 푼 철자 중 하나가 맞으면 받는다(consteval은 Clang JSON이 constexpr와 구별하지 않아 선언 앞 원문으로 본다).
- **V12 시험(단계 리뷰)** — `ValidateHookPath_AttachSeesHookInsideInstancedStruct`·`CreatedDefaultsAreValidated`·`EveryOperationInsideHook`(넣기·지우기·비우기·키 넣기·지우기·Optional 만들기·비우기·형 바꾸기·비우기, 실패 단계 재매핑, InsertKey 결과와 무게시, 거부 시 출력 보존)·`BusyRulesMatchInPlaceWrites`(encode 고정, staged Required 검사 중)·`ReentrantStagedWriteIsBusy`·`PreparedElementsValidateAddedDirectly`, ToolTests `Meta.ValidateHookRejectsFriendAndUsing`·`Meta.ValidateHookAcceptsQualifiedAndAliasSpellings`와 consteval 거부. 게시된 객체의 변경 표지 불일치(재진입 변경)는 기존 묶음 게시(`BeginBatchPublish` Busy)가 같은 규칙으로 막으므로 따로 시험하지 않았다.

### 17.3 구현(CV4)에서 정한 점

D18(`Array<uint8>` Bytes 블록 wire)을 넣었다. 기반 쪽 요약은 [리플렉션 보강](ReflectionExtensions.ko.md) 24절이고, 시험은 `ObjectSerialization/Tests/BytesGraphTests.cpp`·`ReflectionText/Tests/TextBytesTests.cpp`, fixture는 `ReflectionTestFixtures/BytesFixture.h`다.

- **B1 자리** — 가변 배열 `Array<uint8>`(클래스 필드·struct 멤버 — struct 원소 배열·Optional·InstancedStruct 안 struct의 멤버 포함)만 Bytes 블록이다. 정적 배열 `uint8 x[N]`은 늘 N개라 원소마다(ValueArray) 그대로 둔다. `Array<uint8>`은 다른 컨테이너의 원소·값이 될 수 없으므로(직접 중첩 금지 — ReflectionExtensions 1.3절) D18의 "원소 자리"는 struct 원소 안 멤버를 뜻한다.
- **B2 새 값 종류를 두지 않는다** — 스키마는 지금처럼 `{Array, 원소 UInt8}`이고 wire만 필드 단위로 고른다(`FieldWireType(field)`·`detail::IsByteArrayField`). 그래서 MetaSchema·C# `ValueKindTable` 성질 표와 descriptor·MetaCompiler는 바뀌지 않는다(대조 시험 그대로 통과). 경로 연산은 C++ 메모리에 닿으므로 원소 경로(`bytes[3]` 읽기·쓰기·넣기·지우기·원소 수)도 그대로다.
- **B3 wire와 한도** — u64 길이 + byte(`ArchiveWriter::WriteBytes`·`ArchiveReader::ReadBytes`)이고 필드 payload 전부가 블록 하나여야 한다(남은 byte는 InvalidArgument). 원문 String과 같은 규칙이다: 블록 크기는 archive 값 한도(maxValueBytes — 넘으면 CapacityExceeded)로 막고, 원소 누계(AddElements)에는 세지 않으며, 2단계 몫은 1 + 2 × byte 수를 1단계가 미리 청구한다. 배열 규칙도 함께 지킨다: 길이 ≤ MaxArrayElements(저장·로드 모두 CapacityExceeded), 필드 MaxElements는 저장에서 건다(ArrayTooLong 진단 뒤 CapacityExceeded — 로드는 걸지 않는다). 기본 MaxElements(65,536)는 바꾸지 않았다 — 큰 블록(압축 커브 등)은 `MaxElements`를 명시한다(CV8 `PackedTangentCurve::packed`).
- **B4 하위호환 없음** — 원소별 ValueArray wire를 읽는 경로는 남기지 않았다. 옛 데이터는 wire 대조(TypeMismatch)로 거부된다.
- **B5 텍스트 표기** — 통째 값은 명령행·JSON 모두 RFC 4648 Base64 문자열 하나다(Core `Encoding/Base64.h` 그대로 — 엄격한 디코딩: 길이 4의 배수, '=' 패딩, 공백·URL-safe 알파벳 거부, 비정규 끝 비트 거부). 빈 문자열은 빈 배열이다. 잘못된 Base64는 InvalidValue, 필드 MaxElements를 넘는 길이는 OutOfRange 진단이다. 원소 대입은 지금처럼 `bytes[i]=v`(0–255)다. JSON 숫자 배열은 받지 않는다(InvalidValue). 정적 배열 `uint8 x[N]`은 쉼표 목록·JSON 숫자 배열 그대로다. 권장안의 근거: 숫자 목록은 byte당 2–4글자에 구분자가 붙어 블록 텍스트가 3–5배 커지고, Base64는 4/3배이며 공용 구현이 이미 Core에 있다(기능 모듈에 따로 두지 않는다 — 16진은 2배라 쓰지 않았다). 설정 층의 배열 연산 객체(`$clear`·`$remove`·`$add`·`$append`)는 byte 배열에서 InvalidArrayOperation 진단이다(B8 2).
- **B6 텍스트 경로의 비용** — 텍스트 값은 원소 목록(`TextValue` — byte마다 FieldValue 하나)을 거쳐 원소마다 경로 연산으로 적용하므로 큰 블록의 명령행·JSON 입출력은 byte 수에 비례해 느리다. 큰 블록은 wire로 옮기는 것이 정상 경로이고 텍스트는 편집·확인용이다(필요해지면 TextValue에 byte 버퍼 모양을 더한다).
- **B7 시험** — 왕복(빈 배열·100,000 byte·MaxArrayElements 경계·struct 멤버·struct 원소 안 멤버·정적 배열), 다시 저장 byte 같음, 저장 크기 = 빈 배열 크기 + byte 수, 필드 MaxElements·MaxArrayElements 초과 저장 거부, 변조한 블록 길이(길게·짧게·255) 로드 거부와 무게시, 경로 원소 연산, 로드 할당 실패 전 지점의 rollback, 명령행·JSON Base64 왕복과 거부.

- **B8 단계 리뷰 반영(2026-11-18)** — 항목마다 시험으로 먼저 재현한 뒤(수정을 되돌린 빌드에서 새 시험이 실패) 고쳤다.
  - (1, 재현) 상자 경로가 이미 있는 키의 `PathInsertKey`를 늘 "바뀐 것 없음"으로 보아 Map<K, struct>의 기존 값 초기화(기본 값으로 바꿈)를 게시하지 않았고 기본 값 hook 검증도 건너뛰었다. "바뀐 것 없음"은 끝 단계가 Set 원소일 때만이다(경로 cursor가 끝 단계의 Set 여부를 남긴다). 시험 `ValidateHookPath_InsertKeyResetsExistingMapValue`(초기화 게시·변경 표지, 기본 값이 hook을 어기면 StructValidation). Set의 기존 원소는 지금처럼 게시하지 않는다(`EveryOperationInsideHook`의 `outer.list.map`은 Set이라 그대로 맞다).
  - (2, 재현) 설정 층의 배열 연산 객체(`$clear`·`$remove`·`$add`·`$append`)가 `Array<uint8>` 필드에서 적용 전체를 하드 오류(InvalidArgument — 배열 연산이 export한 Base64 문자열을 배열로 찾지 못함)로 끝냈다. byte 배열에서 배열 연산 객체는 InvalidArrayOperation 진단("byte arrays take a Base64 string")으로 거부한다(권장안 — byte 단위 연산은 두지 않는다). 시험 `Config_ByteArrayRejectsArrayOperations`(fixture `BlobSettings` — 기본 fixture에 등록하지 않는다).
  - (3, 재현) staged 재진입 감지(`ConstructionContext::WriteCount`)를 상자 분리(PathDetach) 뒤에 재서, 분리의 할당 콜백이 같은 트랜잭션에 쓴 변경을 놓쳤다(staged는 Pin이 무동작). 누계를 분리 전에 잰다. 시험 `ValidateHookPath_ReentryDuringDetachIsBusy`(분리 중 콜백 쓰기 + 할당 없는 상자 쓰기 → Busy, 값 그대로).
  - (4, 재현) `PreparedStructElements`의 추가 원소 검증 표시가 `ValidateAdded` 뒤 `AddedTarget` 시작 위치로 쓴 변경에도 남아 다시 검증하지 않았다. 그 시작 위치의 변경 경로 연산이 표시를 내린다(`PathCursor::MarkAddedElementWritten`). 시험 `ValidateHookPath_PreparedElementsWriteAfterValidateIsRevalidated`.
  - (5, 재현) 텍스트 Base64 풀기가 원소 수 한도 검사보다 먼저 입력 길이만큼 버퍼를 할당했다. 길이·패딩으로 byte 수를 먼저 구해 한도(OutOfRange)를 본 뒤 할당한다(길이가 4의 배수가 아니면 할당 없이 InvalidValue). 시험 `TextBytes_LengthIsCheckedBeforeDecoding`(64 KiB 넘는 할당을 거부하는 대상 allocator로 300,000자 입력 → OutOfMemory가 아니라 OutOfRange).
  - (6) MetaCompiler: consteval 판정(`DeclarationPrefixHas`)이 앞 주석의 낱말·`;`까지 보아(재현) 정상 hook을 거부할 수 있었다. 주석을 걷어낸 원문(다른 원문 검사와 나눈 캐시)으로 판정한다. 중첩 struct가 바깥 struct의 별칭 목록을 지우는지는 재현되지 않았다 — 중첩 선언은 바깥 멤버 순회가 끝난 뒤 방문한다. 둘 다 ToolTests `Meta.ValidateHookIgnoresCommentsAndNestedAliases`가 고정한다.
  - (7) 시험 보강: Optional·Map 값·InstancedStruct 내용 안 struct 멤버의 `Array<uint8>` 왕복·다시 저장 byte 같음·블록 길이 변조 거부(`BytesGraph_NestedPositionsRoundTripAndRejectTampering`), 로드 쪽 maxValueBytes 초과와 MaxArrayElements + 1 byte 블록(손으로 만든 Record)의 CapacityExceeded(`BytesGraph_LoadLimits`). 텍스트의 URL-safe 문자·비정규 끝 bit는 Core `Base64_StrictDecodingRejectsWithoutWriting`이 이미 덮어 더하지 않았다.

### 17.4 구현(CV5)에서 정한 점

새 Core 런타임 모듈 `Curves`(`Core/Source/Runtime/Curves` — 의존 ObjectRuntime, Engine 층 없음)에 4절의 값 타입을 넣었다. 시험은 `Curves/Tests/CurveGoldenTests.cpp`(UE 참조 대조 — 참조는 `CurveUeReference.h`, 머리 주석에 참조 함수 ↔ UE 파일:줄 대조표)·`CurveEvaluateTests.cpp`(독립 고정 벡터)·`CurveEditTests.cpp`·`CurveValueRangeTests.cpp`·`CurveReflectionTests.cpp`, fixture는 새 Developer 모듈 `CurvesTestFixtures`(`CurveFixture.h` — 헤더만)다.

- **C1 배치** — 공개 헤더는 `Curves/CurveTypes.h`(enum 다섯·키 둘·`ScalarCurveBase`)·`TangentCurve.h`·`PlainCurve.h`·`CurvesModule.h`(`RegisterCurvesTypes`)이고 이름공간은 `rocks`다(`InterpCurve`와 같다). 구현은 기능별로 나눴다: `TangentCurve.cpp`(평가·자동 접선·편집)·`TangentCurveRedundant.cpp`·`TangentCurveValueRange.cpp`·`PlainCurve.cpp`, 두 커브가 함께 쓰는 키 배열 연산과 평가 틀은 `Private/CurveKeyEdit.h`(템플릿)다. 시험 fixture를 `ReflectionTestFixtures`에 넣지 않은 것은 그 모듈이 Curves에 의존하지 않게 하려는 것이다(`ConfigTestFixtures`와 같은 선택).
- **C2 수학 층 보강** — 가중 구간 제어점 계산을 `math::WeightedCurveControls(k1, k2) -> CurveWeightedControls`로 꺼냈다(`WeightedCurveSegment`가 그대로 부른다 — 동작 불변, CV1 golden 그대로 통과). 값 범위(C10)가 Evaluate와 같은 곡선을 보려고 쓴다. CV1 시험의 UE 참조(`ue58curve`)는 `Core/Tests/KeyCurveUeReference.h`로 옮겨(동작 불변 이동) Curves golden이 같은 참조를 쓴다.
- **C3 API 이름(7.1절 표와 다른 점)** — `ShiftTime(delta)`는 결과 시간이 비유한이면 실패해야 하므로 `TryShiftTime`이다. `SetKeyTangents`는 `TrySetKeyTangents(index, 도착, 출발)`과 `TrySetKeyTangentWeights(index, 도착 가중치, 출발 가중치)`로 나눴다. `KeyCount()`와 키 범위 전체를 굽는 `TryBake(interval)`(UE `BakeCurve(SampleRate)`)을 더했다. `PlainCurve`에는 접선·보간 키 칸 연산이 없고 중복 제거에 표본 인자가 없다(C9).
- **C4 평가** — 두 커브가 같은 틀(`EvaluateCurve` — UE `Eval`·`RemapTimeValue` 줄 순서)을 쓰고 구간 경계만 다르다(5.7절). 키가 있으면 NaN 시간은 재배치 전에 quiet NaN을 낸다(15절 4). UE처럼 결과에 순환 오프셋을 늘 더하므로(0이어도) 기본값 −0은 +0이 된다(UE와 비트가 같다). int32 순환 경로가 키 범위를 벗어나게 옮긴 시간(5.1절 — 큰 주기 몫에서는 한 주기 길이까지)은 이 틀에서 떨어진 쪽의 외삽 갈래로 간다(UE와 같은 코드 경로). 그쪽 외삽이 Linear면 끝 키 값이 아니라 기울기로 이은 값이라 크게 달라질 수 있지만 UE와 비트가 같게 자르지 않는다(CV5 단계 리뷰 3).
- **C5 허용오차 인자** — `TryUpdateOrAddKey`·`TryRemoveRedundantKeys`의 허용오차가 NaN·음수면 InvalidArgument다(UE는 음수를 그대로 써서 늘 불일치·늘 보존이 된다). `FindKey`는 NaN 시간·NaN·음수 허용오차에서 `IndexNone`이고, `IsConstant`의 NaN 허용오차는 키가 둘 이상이면 거짓이다.
- **C6 회전 풀기** — UE 반복 뺄셈(±360)을 그대로 해서 UE가 끝나는 모든 입력에서 비트가 같다(예: 앞 키 0, 4e8 → 약 112만 번 뒤 104). 어느 값이 비유한이면 옮기지 않고, 360을 더하고 빼도 값이 그대로인 큰 값(UE는 끝나지 않는다 — 예: 1e12)이면 double 나머지(`Fmod(차, 360)`을 [−180, 180]으로)로 한 번에 옮긴다. 반복 상한 2^26은 안전장치다(진전이 있는 크기 — 약 2^33 아래 — 는 그 안에 끝난다). 처음 구현의 2^20 상한은 UE가 끝나는 입력(4e8 → UE 104, Rocks 40)에서 갈랐다(CV5 단계 리뷰 2).
- **C7 탐색** — `TryUpdateOrAddKey`·`FindKey`의 "허용오차 안 가장 앞 키"는 "키 시간 > time 또는 허용오차 안"이 정렬 배열에서 거짓 다음 참인 단조 조건이라 이진 탐색으로 경계를 찾는다(키 시간 ≤ time 쪽에서 float 차는 키 시간에 대해 늘지 않는다). UE `UpdateOrAddKey`의 앞에서부터 훑기와 결과가 같다(golden). 삽입 위치는 첫 `time ≥ 새 시간`(lower bound)이다.
- **C8 시간 옮기기** — `TrySetKeyTime`은 할당 없이 제자리에서 원소를 밀어 옮긴다. `TryShiftKeyTimes`는 고른 키를 원래 index 오름차순으로 다시 넣으므로 둘 이상이 같은 시간으로 가면 원래 index가 큰 키가 앞이다(시험이 이 순서를 고정한다). D10에 따라 둘 다 접선을 다시 계산하지 않는다. golden은 UE `SetKeyTime`·`DeleteKey`를 "Rocks 지우기 + `AutoSetTangents` + 넣기 + 옛 칸 복원"과 비트 대조하고, `TrySetKeyTime` 자체는 UE 식에서 삭제 재계산만 뺀 결과와 대조한다.
- **C9 Bake·중복 제거** — Bake: 키가 없으면 성공(무동작), `first ≥ last`면 표본 없음, 표본 수가 2^24를 넘으면 CapacityExceeded(i·interval의 float i가 정확한 범위), 표본은 원래 커브로 먼저 모두 평가한다. 중복 제거: `TangentCurve`는 RC 알고리즘(비교 기준은 시작 보존 키 — 15절 8, 제거 뒤 재계산 없음 — D10)이고, 표본 프레임 번호가 int32 끝에 닿으면 표본을 멈춘다(UE `FrameNumber += 1`은 감싸 돌아 끝나지 않는다). 표본 수는 UE와 같이 (키 간격 × 프레임 속도)에 비례하고, 호출 하나에 2^24개(Bake와 같은 상한)를 넘으면 CapacityExceeded이고 커브는 그대로다(CV5 단계 리뷰 9 — UE는 상한이 없다). 차가 NaN인 키는 남긴다(`!(차 ≤ tolerance)` — UE `>` 비교는 NaN 값 키를 지운다, 리뷰 4). 범위판의 first·last가 NaN이면 InvalidArgument, `first ≥ last`·보존 키 없음은 무동작이다. `PlainCurve`는 UE `FSimpleCurve::RemoveRedundantKeysInternal`(남긴 키를 되짚어 다시 지우는 알고리즘 — 두 번 불러도 더 지우지 않는다)을 옮겼다. 이 알고리즘의 비교 기준은 처음부터 시작 보존 키라 15절 8의 수정이 필요 없다. UE는 표본 프레임 속도 인자를 받지만 쓰지 않으므로 Rocks `PlainCurve`는 그 인자를 두지 않는다.
- **C10 값 범위(7.3절 구현)** — 가중 구간의 매개변수 집합을 일반 규칙으로 구현했다: S = {s ∈ [0, 1] : x(s) ≥ 0이고 모든 u ∈ (s, 1]에서 x(u) > x(s)}(s = 1, 곧 α = 1은 마지막 구간에서만 쓰인다 — 그 밖의 끝 극한은 x가 끝에서 증가할 때만 S에 든다). x′의 (0, 1) 안 근으로 나눈 조각을 오른쪽부터 보며, x가 증가하는 조각에서 오른쪽 조각들의 x 최솟값보다 작고 0 이상인 부분만 S이고, 그 조각의 두 끝(뛰는 점의 두 쪽 극한)과 y′ 근에서 y를 평가한다. 7.3절의 "x′의 두 근 s₁ < s₂" 서술은 이 규칙의 한 경우이고 x′ 근이 하나인 경우도 같은 규칙이다. 근은 double(이차식 안정 공식), 조각 경계는 이분법, y는 Evaluate와 같은 `BezierCurveValue`(float)다.
  - 키 값: 7.3절의 처음 문구 "키 값을 모두 넣고"는 고쳤다. 범위는 Evaluate가 키 범위 안에서 내는 값 집합의 닫힌 범위라 구간마다 실제로 쓰는 값만 넣는다(쓰이지 않는 키 값 — 길이 0 구간의 앞 키, 접힌 가중 구간이 건너뛰는 시작 값, 끝이 되돌아가는(x2 > 1) 가중 구간의 끝 값 — 은 들지 않는다).
  - 풀이 띠(CV5 단계 리뷰 1 — 정공법): Evaluate의 3차 방정식 풀이는 판별식 |D| ≤ 1e-8에서 근을 겹근 {2U − A/3, −U − A/3}(|Q| ≤ 1e-8이면 −A/3 하나)으로 근사한다. 얕은 접힘은 실근 셋인 α 구간 전체가 이 띠에 들어 수학적 S 밖의 매개변수가 골라지고, 무작위 탐색에서 수학적 범위를 폭의 2%까지 넘었다(처음 적은 "약 1.3e-4"는 틀렸다 — 반례: 키 (0, −0.33362928, 출발 접선 −39.46·가중치 24.106)·(1, 0.56734283, 도착 접선 7.873·가중치 10.2616), 실제 최솟값 Evaluate(0.243263736) ≈ −13.3529, 고치기 전 범위 최솟값 ≈ −13.178). 시간 다항식의 계수 중 α는 상수항에만 들어가 풀이의 P는 α와 무관하고 Q는 α에 대해 일차이므로, 띠를 Q 구간으로 구하고 후보가 0·1을 지나는 Q와 Q = 0·±1e-8로 나눈 조각마다 Evaluate와 같은 근 고르기로 고른 매개변수가 지나는 구간의 y 극값을 넣는다. 그래서 계약은 "Evaluate가 내는 값을 모두 담는다"(float 반올림 여유만)다. 띠 밖의 풀이(세 실근의 삼각 공식·실근 하나)는 정확해 S가 덮는다.
  - 제어점·값이 비유한이면 InvalidArgument다. 시험: 손 계산 넘침(제어점 0·1·1·0 → 0.75), 접힌 구간(시간 제어점 0·1.5·−0.5·1), 리뷰 반례 고정, 무작위 300 커브와 판별식 띠를 겨냥한 무작위 300 커브(시간 제어점 x1 ∈ [0.4, 0.8]·x2 ∈ [−0.45, −0.1], 끝이 되돌아가는 x2 ∈ [1.05, 1.6])에서 촘촘한 Evaluate 표본과 접힘 점 근처 표본(10^-12 … 10^-1 떨어진 α)이 범위 안(여유 1e-4·(1 + 폭) — 일반 무작위는 5e-4)이고 끝에 닿는지(여유 2e-2·(1 + 폭))를 본다.
- **C11 PlainCurve 값 범위** — 키 값의 최소·최대다(Linear·Constant 구간은 끝 값 사이). 비유한 키 값은 InvalidArgument다.
- **C12 리플렉션** — 검증 hook은 두 커브 모두 "키 시간 유한·비내림차순"(`ValidateKeys`)이다. 키 배열의 필드 `MaxElements`는 기본값(65536)이라 그보다 키가 많은 커브는 저장이 CapacityExceeded다(필요하면 에셋 층이 필드에 더 큰 한도를 둔다). 기본값·외삽 필드 ID는 `ScalarCurveBase` 소유이고 두 파생에서 같다. 시험은 다섯 자리(필드·Array 원소·Optional·Map 값·InstancedStruct 내용)마다 graph decode(저장 byte의 키 시간을 앞 키보다 작게·NaN으로 바꿔 로드 거부, 대상 Registry 빈 채)·JSON import(InvalidStructValue)·경로 쓰기(StructValidation)를 보고, 대입 묶음은 거부 시 묶음 전체를 게시하지 않는다. `PlainCurveInterp`의 Cubic은 JSON·명령행에서 enum 이름 거부로 막힌다(미선언 enum 값 2의 wire decode 거부는 리플렉션 기반 시험이 이미 덮는다).
- **C13 golden 범위** — 무작위 TangentCurve·PlainCurve 각 3,000개 × 시간 40개(키 시간 정확히·범위 안·몇 주기 밖·멀리·NaN·±Inf)가 수정을 켠 참조와 비트가 같고, 커브 층 NaN 수정(15절 4)만 끈 참조와는 키 있는 NaN 시간에서만 다르다(이 대조의 "수정을 끈 참조"는 수학 층 수정 15절 1–3을 켠 채라 UE 그대로가 아니다 — 수학 층의 UE 그대로 대조는 CV1 golden이 한다). 자동 접선 2,000개, 편집 순서 400 × 30단계, 중복 제거(전체·범위판 × 표본 30·23.976·60fps·없음) 1,500개 + 범위판 비교 기준 수정(15절 8)만 끈 참조(UE의 index 0 기준)와 다른 사례 존재, PlainCurve 중복 제거 1,500개와 두 번 불러도 같음, Bake(정확한 격자)·IsConstant 500개, Bake 간격 0.1의 UE float 누적과 다른 시간 고정, PlainCurve 편집 순서 400 × 30단계(UE FSimpleCurve 참조). 순환 사례의 주기 몫은 2^29 미만이다(double 재배치는 CV1 손 계산 벡터와 C4 고정 벡터 2^33).
- **C14 키 wire 실측(9.2절, D8)** — `CurveReflection_KeyWireSize`(키 1,000개 커브 저장 크기 − 빈 커브 저장 크기, 키당): `TangentCurveKey` **352 byte**, `PlainCurveKey` **116 byte**다. 9.2절 추정(약 350)과 맞고 판단은 9.2절에 적었다.

### 17.5 구현(CV6)에서 정한 점

계단 커브 셋(`IntegerStepCurve`·`NameStepCurve`·`StringStepCurve`와 키 `IntegerStepKey`·`NameStepKey`·`StringStepKey`)을 `Curves` 모듈에 넣었다. 시험은 `Curves/Tests/StepCurveGoldenTests.cpp`(UE `FIntegralCurve`·`FStringCurve` 줄 단위 참조 — 머리 주석에 대조표)·`StepCurveTests.cpp`(독립 고정 벡터·편집)·`StepCurveReflectionTests.cpp`, fixture는 `CurvesTestFixtures/CurveFixture.h`의 `StepCurveHolder`다.

- **S1 배치** — 공개 헤더는 `Curves/StepCurves.h` 하나(키 셋·커브 셋), 구현은 `Private/StepCurves.cpp`다. 공용 키 배열 연산(`CurveKeyEdit.h`)을 키 옮기기(Move)와 `CloneCurveKey`(비트 복사 가능한 키는 그대로, `StringStepKey`는 값 문자열을 키 배열의 allocator로 복제)로 일반화했다 — CV5 키에는 동작이 같다. fixture는 Name 값이 있어 Registry에 NamePool이 필요하므로 `CurveHolder`와 나눈 `StepCurveHolder`(필드·Array 원소·Optional·Map 값)다.
- **S2 평가** — 5.8절 규칙 그대로다. UE의 앞에서부터 훑기(`Time < Keys[i].Time`인 첫 i의 앞 키)는 결과가 같은 이진 탐색(시간 초과 첫 키 − 1)이다(golden). 같은 시간 키가 여럿이면 그 시간의 마지막 키다. **NaN 시간은 기본값(없으면 fallback)이다** — UE는 비교가 모두 거짓이라 키가 있으면 마지막 키(키 하나면 그 키)를 낸다. 스칼라 커브의 NaN 규칙(5.6절 — 위치가 없는 시간으로 키를 고르지 않는다)과 같은 취지이고 정수·Name·String에는 NaN 값이 없어 기본값으로 둔다. ±Inf는 첫·마지막 키 규칙 그대로다. `StringStepCurve::Evaluate`는 할당 없이 `StringView`(키 값·기본값 — 다음 편집까지, 또는 fallback)를, `NameStepCurve::Evaluate`는 `Name`을 돌려준다.
- **S3 기본값** — 셋 다 `Optional`이다(D6). String은 빈 문자열도 기본값이다(UE는 빈 문자열이 "없음"이라 빈 기본값을 둘 수 없다). Name 기본값은 Rocks가 더한 칸이다(D9).
- **S4 편집 연산** — 7.1절 표의 시간·값 연산을 같은 규칙으로 둔다: `TryAddKey`(같은 시간 키 앞, 회전 풀기 없음), `TryUpdateOrAddKey`(허용오차 1e-4 — UE `UE_KINDA_SMALL_NUMBER`), `TryRemoveKey`, `TrySetKeyTime`(UE `SetKeyTime`은 접선이 없어 결과가 같다), `TrySetKeyValue`, `TrySetKeys`, `Reset`, `TryShiftTime`·`TryScaleTime`·`TryShiftKeyTimes`, `FindKey`(가장 앞 키 — 15절 6), `TryGetTimeRange`, 그리고 UE `FIntegralCurve::FindKeyBeforeOrAt` 대응 `FindKeyAtOrBefore`(셋 다 — UE는 Integer에만 있다)다. Bake·값 범위는 두지 않는다(UE에도 없다). String 값은 키 배열의 allocator로 복사하고, 할당이 실패하면 OutOfMemory이고 커브는 그대로다(새 문자열을 먼저 만든 뒤 바꾼다).
- **S5 중복 키 제거** — UE 판정(원래 배열에서 앞·뒤 키와 값이 같은 안쪽 키 — 평가가 바뀌지 않는다)을 옮기고, 남은 키 하나를 기본값으로 바꾸는 UE 동작은 하지 않는다(15절 9). 값 같음은 int32 `==`, Name은 Rocks Name 규칙(ASCII 대소문자 무시 — UE `FName`과 같다)가 아니라 **토큰의 모든 칸(domain·id·displayId·number·kind)이 같아야** 하고(CV6·CV7 리뷰 6, CV8 리뷰 8 — 철자만 다른 키를 지우면 평가 결과의 표시 철자가 바뀌고, Name 규칙은 None 표시 이름의 domain을 보지 않는다. 키 핸들 표 H2의 Name 비교와 같은 판단), String은 byte 비교다(UE `FString ==`는 대소문자를 무시해 "walk"·"Walk"·"walk"의 가운데를 지운다 — 15절 10의 같음 규칙을 여기에도 적용). UE `FNameCurve`에는 중복 제거가 없지만 String과 같은 규칙으로 더했다.
- **S6 리플렉션** — 검증 hook은 CV5와 같은 `ValidateKeys`(키 시간 유한·비내림차순)다. Name 값 키 커브를 담은 객체의 decode·JSON은 기존 Name 필드 규칙(Registry·텍스트 옵션의 NamePool)을 따른다. 키 struct는 Atomic이고 키 배열의 도구 이름은 `keys`다.
- **S7 golden 범위** — 무작위 Integer 3,000개 × 시간 30개(키 시간 정확히·범위 안팎·NaN, `useDefaultBeforeFirstKey`·기본값 조합)가 수정을 켠 참조와 같고 UE 그대로와는 NaN에서만 다르다. `FindKeyAtOrBefore`도 UE `FindKeyBeforeOrAt`과 같다. String·Name 3,000개가 `FStringCurve::Eval` 참조(Name은 D9의 String 규칙 — 기본값·fallback은 키 단어 표 밖의 이름)와 같다. 편집 순서 Integer 400 × 30단계·String 300 × 30단계(대소문자만 다른 값 포함), 중복 제거 2,000개(UE 그대로는 남은 키 기본값 변환으로 첫 키 이전 평가가, FString 같음으로 남는 키 수가 달라지는 사례 존재).

### 17.6 구현(CV7)에서 정한 점

키 핸들 표(8절, D3)를 넣었다. 시험은 `Curves/Tests/CurveKeyHandleTableTests.cpp`와 `CurveReflectionTests.cpp`의 `CurveReflection_PathWriteMakesHandleTableStale`이다.

- **H1 배치** — 헤더 전용 템플릿 `Curves/CurveKeyHandleTable.h`(`CurveKeyHandle`·`CurveKeyHandleTable<Curve>`)이고 다섯 커브 형식(`TangentCurve`·`PlainCurve`·계단 커브 셋)에 같이 쓴다. 리플렉션 타입이 아니다(저장하지 않는다 — UE와 같다). 발급 카운터를 uint32 끝 근처로 옮기는 시험 접근점은 모듈 Private의 `CurveKeyHandleTableTestAccess.h`다(공개 API에는 카운터를 정하는 길이 없다).
- **H2 수정 감지** — 사본은 키 struct 칸마다 비트 비교한다(padding은 보지 않는다): float는 비트(−0과 +0도 다르다), enum, Name은 토큰의 모든 칸(Name 같음 규칙이 아니다 — 표시 철자만 바꾼 쓰기도 수정이다), String은 byte다. 새로 만든 표는 빈 사본이라 빈 커브에만 맞고, 키가 있는 커브는 먼저 `TryRebuild`를 부른다.
- **H3 TryRebuild** — 모든 키에 새 핸들을 발급하고(발급 카운터는 이어진다 — 옛 핸들 값이 새 키를 가리키는 일이 없다) 무효 표시를 지운다. 발급할 수가 uint32 끝을 넘으면 Overflow이고 표는 그대로다.
- **H4 표 판 편집 함수(UE 대응)** — `TryGetHandle`(UE `GetKeyHandle`), `TryFindIndex`(UE `GetIndex`), `TryAddKey`(UE `AddKey` — 새 핸들), `TryUpdateOrAddKey`(값만 바꾸면 그 키의 핸들, 더하면 새 핸들 — 값만 바꿀 때는 카운터 끝에서도 Overflow가 아니다), `TryRemoveKey`(UE `DeleteKey`), `TrySetKeyTime`(UE `SetKeyTime` — 핸들이 키를 따라간다), `TrySetKeyValue`(UE `SetKeyValue`), `TryShiftKeyTimes`(UE `ShiftCurve(Delta, KeyHandles)` — 커브 `TryShiftKeyTimes`와 같은 재삽입 규칙으로 핸들 순서를 맞춘다. UE는 모르는 핸들을 건너뛰고 중복이 없는 `TSet`을 받지만 Rocks는 모르는 핸들에 NotFound, 중복 핸들에 InvalidArgument다), `TryShiftTime`·`TryScaleTime`(UE `ShiftCurve(Delta)`·`ScaleCurve` — IndexedCurve.cpp 89–146, 순서가 그대로라 핸들도 그대로), `TryRemoveRedundantKeys`(UE 중복 제거의 핸들 유지 — RC 984–1100·IntegralCurve.cpp 228–257·StringCurve.cpp 209–238. 커브의 `TryRemoveRedundantKeys(Array<usize>& keptIndices, …)` 판이 남긴 키의 원래 index 목록 — 순서를 보존하는 부분열 — 을 내고 표는 핸들·사본을 앞으로 당긴다. 지운 키의 핸들은 NotFound), `TryEditKeys`(TangentCurve 전용 — 편집 함수에 **키별 칸 편집자**(`KeyFieldEditor`)만 준다: 핸들로 키를 골라 보간·접선 방식·가중 방식·접선·가중치만 바꾸고 시간·값·키 수·순서를 바꾸는 길이 없다. 안전장치로 편집 뒤 키 수·시간·값을 사본과 비트까지 비교해 다르면 — 편집 함수가 커브를 직접 잡아 바꾼 경우, 같은 시간 키의 순서 바뀜 포함 — 표를 무효로 한다), `TryAutoSetTangents`(TangentCurve 전용 표 판 `AutoSetTangents`). 인자는 커브 자신의 같은 이름 연산 인자를 그대로 넘긴다(`TryAddKey(curve, time, value, unwind)` 등). UE `GetNextKey`·`GetPreviousKey`·핸들 판 `ScaleCurve`·Bake는 두지 않는다(index 연산과 `TryGetHandle`로 한다).
- **H5 원자성** — 모든 함수는 실패하면 커브와 표가 그대로다. 예외는 `TryEditKeys` 하나다: 편집 함수가 실패를 돌려주면 그 전에 편집자로 바꾼 칸은 남고 표는 그 상태에 맞춰 다시 맞춘다(핸들은 계속 쓸 수 있다 — 편집자의 칸 쓰기는 할당하지 않아 실패는 모르는 핸들 NotFound뿐이다). 커브를 바꾸기 전에 핸들 목록·사본의 용량을 잡고 String 사본 키를 복제하며, 선택 이동은 새 순서·새 배열을 먼저 마련한 뒤 커브 연산을 부른다(커브 연산 뒤에는 할당하지 않는다). 무효 판정(StaleHandle)도 커브를 바꾸지 않는다.
- **H6 비용** — 핸들 → index 조회는 핸들 목록의 선형 탐색이다. 함수마다 사본 비교가 이미 O(N)이라 맵을 두지 않았다(편집기 빈도).
- **H7 복사·이동** — `TryCopyFrom`은 핸들·사본·발급 카운터·무효 표시를 그대로 복사한다(실패하면 그대로). 이동은 noexcept다. 복사 생성·대입은 지웠다(String 사본 복제가 실패할 수 있어 Rocks 규약대로 `TryCopyFrom`이다).
- **H8 시험** — 다섯 커브 형식 × 60커브 × 40단계 무작위 편집(더하기·같은 시간 더하기·UpdateOrAdd·지우기·시간 옮기기·값 바꾸기·선택 이동·표 판 Shift·Scale·중복 제거, TangentCurve는 `TryEditKeys` 편집자로 보간·가중 방식·접선 바꾸기 + `TryAutoSetTangents`)마다 모든 핸들이 같은 키(값에 둔 고유 번호)를 가리키고 핸들이 단조 증가, 표 밖 수정 다섯 가지(값·시간·키 수·접선 부호·보간)와 리플렉션 경로 쓰기의 StaleHandle 감지와 `TryRebuild`, Overflow 경계, 할당 실패 원자성(주입 단언 — 더하기·UpdateOrAdd·값 바꾸기·선택 이동·TryRebuild·TryCopyFrom), 복사·이동(이동 대입과 이동된 쪽 포함)과 무효 표의 복사. 리뷰 반영으로 같은 시간 키 순서 바뀜의 감지, double·int 허용오차의 Overflow, 계단 커브·범위판 중복 제거의 핸들 유지와 할당 실패 원자성, 대입 묶음 뒤 StaleHandle(`CurveReflection_AssignmentsMakeHandleTableStale`)을 더했다.
- **H9 UpdateOrAdd의 Overflow(CV6·CV7 리뷰 3)** — 더할지를 미리 짐작하지 않고 커브 연산 뒤 키 수로 판정한다. 발급 카운터가 끝인데 키가 더해졌으면 그 키를 `TryRemoveKey`로 지워 되돌리고 Overflow다. 처음 구현은 허용오차 인자가 정확히 float일 때만 그 값으로 미리 판정해 double·int 허용오차에서 카운터가 0으로 감쌌다.
- **H10 이동** — 이동된 표는 빈 사본의 표(빈 커브에만 맞는다)이고 발급 카운터는 남는다(그 표로 `TryRebuild`해도 옛 핸들 값을 다시 발급하지 않는다).

### 17.7 구현(CV8)에서 정한 점

압축 커브(11절, D13·D14·D18·D21)를 넣었다. 시험은 `Curves/Tests/PackedTangentCurveTests.cpp`(UE 참조 대조·D14·왕복·변조·원자성)와 `PackedTangentCurveReflectionTests.cpp`(fixture `CurvesTestFixtures/CurveFixture.h`의 `PackedCurveHolder`)이고, UE `CompressCurve` 참조는 `Curves/Tests/CurvePackUeReference.h`(머리 주석에 참조 함수 ↔ RC 줄 대조표)다.

- **P1 배치** — `Curves/PackedTangentCurve.h`(`PackedCurveFormat`·`PackedKeyTimeFormat` enum, `PackedTangentCurve`)와 `Private/PackedTangentCurve.cpp`. 공통 평가 틀(`curves_detail::EvaluateCurve`)을 키 목록 템플릿(Size·operator[] — Span 또는 packed 키 보기)으로 넓혀 `TangentCurve`·`PlainCurve`·압축이 같은 틀을 쓴다. 압축 평가는 키를 읽을 때 byte에서 `TangentCurveKey`로 푸는 보기(`PackedKeyView`)를 넘긴다 — 할당이 없다.
- **P2 Constant 형식 조건** — 키 1개, 또는 Cubic 키가 없고 값이 비트까지 같은 **유한 값**이며 앞·뒤 외삽이 모두 Linear가 아닐 때다. 11.1절의 "비트 같은 값"만으로는 원본과 비트가 같지 않다: Linear 외삽은 ±Inf 시간에서 기울기 0 × Inf = NaN, ±Inf 값은 구간 Lerp(Inf − Inf)·CycleWithOffset 차가 NaN이다. 이 조건이면 원본은 NaN 시간 말고 모든 시간에서 `값 + 0`(−0은 +0)이고 압축 Constant 평가와 같다(NaN 시간은 NaN — 원본과 같다).
- **P3 Weighted 판정** — 11.1절대로 구간 단위다. 가중 방식은 있지만 가중 구간이 없는 커브(첫 키 Arrive, 마지막 키 Leave — 마지막 키 Both는 앞 구간의 도착이 가중이다)는 Mixed·Cubic이 되고 가중 칸은 버린다 — 그 칸은 평가에 쓰이지 않으므로 평가는 비트가 같다(시험 반례: UE Weighted 5 float/키 → Rocks Cubic 3 float/키).
- **P4 키 시간 형식 판정** — Linear 이상 형식에서만 본다. errorThreshold·sampleRateHz가 NaN·Inf·0 이하, Δ = 0·비유한, `SizeUInt16 ≥ SizeFloat32`(UE와 같은 식 — 데이터 부분이 같아 전체 크기 비교와 같다), 표본(`min + k·(1/rate)`, k = 0부터 시간이 max 이하인 동안)이 2^24개를 넘으면 Float32다. 그 밖은 표본마다 원본 쪽(압축이 담는 칸만 남긴 키)과 손실 쪽(같은 키 + 양자화 후 복원한 시간)을 **`Evaluate`와 같은 평가**(구간 탐색·외삽 틀까지 — 11.2절의 "구간 이진 탐색"을 이렇게 구체화했다)로 구해 차가 모두 errorThreshold 이하면 Quantized16이다. 그래서 고른 Quantized16은 표본 시간마다 `|압축 Evaluate − 원본 Evaluate| ≤ errorThreshold`가 보장된다(복원된 마지막 키 시간이 max와 1 ulp 다를 때 외삽 갈래로 가는 경우까지 같은 식으로 본다). 판정이 키 사본 둘을 만들어 OutOfMemory가 날 수 있다. **CV8 단계 리뷰 반영**: 길이의 역수(1/Δ — Δ가 denormal이면 Inf)·표본 간격(1/rate — denormal rate면 Inf)이 비유한이거나 손실 쪽 복원 시간 하나라도 비유한이면(min·Δ가 float 끝 근처 — 예: min = 3·2^103, max = FLT_MAX) Float32다. 처음 구현은 이 경우 Validate가 거부하는 Quantized16을 만들었다. 양자화 Clamp는 UE 식(NaN이면 1 — 15절 33)이고, 평가의 이진 탐색은 키 시간만 읽는다(`PackedKeyView::TimeAt` — 키 전체를 풀지 않는다).
- **P5 칸 정규형** — `emptyDefault`는 Empty에서만 있고, `constantValue`는 Constant가 아니면 비트 0, Empty·Constant의 시간 형식은 Float32다. `TryPack`은 늘 이 정규형을 만들고(같은 커브 → 같은 값 → 다시 저장 byte 같음) `Validate`는 어긋나면 거부한다. 외삽은 모든 형식에서 원본 값을 옮긴다(Empty·Constant 평가에는 쓰이지 않지만 풀기가 되돌린다).
- **P6 한도** — 키 수가 uint32를 넘거나 packed가 1 MiB(`packed` 필드 `MaxElements` = MaxArrayElements — decode 한도와 같다)를 넘으면 `TryPack`은 CapacityExceeded이고 그대로다. Linear Float32는 131072키(정확히 1 MiB)까지다.
- **P7 평가** — 진입에서 O(1) 대조(enum 범위, Empty·Constant는 keyCount 0·packed 비어 있음, 그 밖은 keyCount ≥ 2·packed 크기 = 기대 크기)를 하고 어긋나면 `Evaluate` quiet NaN·`TryEvaluate` InvalidArgument다. Empty는 (기본값, 없으면 fallback) + 0, Constant는 NaN 시간이면 NaN, 아니면 값 + 0이다(원본 키 하나 커브와 같다). 키 형식은 공통 틀이고, 탐색 중 읽은 키의 보간 byte가 {1, 2, 3} 밖이거나 가중 byte가 3을 넘으면 NaN·InvalidArgument다(읽지 않은 키의 byte·정렬·유한 시간은 Validate 몫 — 이진 탐색은 범위 안 index만 읽으므로 메모리 밖 읽기는 없다).
- **P8 풀기** — `Validate`를 먼저 하고(위반 InvalidArgument), 키는 User 접선 방식, Linear 형식은 접선 0·Linear, Cubic 형식은 Cubic, 가중 칸은 Weighted 형식에서만 옮긴다. Constant는 (0, 값, Linear) 키 하나, Empty는 키 없음 + 기본값이다. 외삽을 옮기고 Empty가 아니면 기본값을 지운다. 키 교체는 `TrySetKeys`라 실패하면 out이 그대로다. Float32 형식은 풀어서 다시 압축하면 byte가 같다(시험).
- **P9 복사·이동** — `TryCopyFrom`(실패하면 그대로). 이동(생성·대입, noexcept)은 옮겨진 쪽을 새로 만든 값과 같은 Empty 정규형으로 되돌린다(CV8 단계 리뷰 — 처음에는 packed만 비고 형식·키 수가 남아 Validate를 통과하지 못했다). 칸을 직접 바꾸는 시험 접근점은 모듈 Private의 `PackedTangentCurveTestAccess.h`다.
- **P10 UE golden 범위** — 참조(`CurvePackUeReference.h`)는 UE `FindRichCurveCompressionFormat`·`FindRichCurveKeyFormat`·`CompressCurve`·`RoundHalfFromZero`를 그대로 옮겼다. 무작위 1500커브에서 (1) Rocks가 고른 형식·시간 형식으로 만든 UE 배치와 packed byte가 모두 같고, (2) 형식이 다른 사례는 UE Constant(1e-8·Linear 외삽)와 UE Weighted(가중 구간 없음)뿐이며, (3) 시간 형식 판정은 방식이 달라(P4) 다를 수 있어 일치 비율만 본다. UE 압축 평가(`FCompressedRichCurve::Eval`)는 옮기지 않았다 — Rocks 평가는 원본 `TangentCurve`와 비트 대조한다(D14). 참조는 UE의 손실 키 범위 밖 읽기를 고쳐 옮겼고(15절 32 — 반례 세 개를 고정 시험 `PackedTangentCurve_UeLossyKeyOverrunCounterexamples`로 둔다), Clamp는 UE 식이다.

### 17.8 구현(CV9)에서 정한 점

InterpCurve 미러와 값 범위(12절, D19)를 넣었다. 시험은 `Curves/Tests/InterpCurveReflectionTests.cpp`(fixture `CurvesTestFixtures/CurveFixture.h`의 `InterpCurveHolder`), `Core/Tests/MathInterpCurveBoundsTests.cpp`(UE `CalcBounds`·`FindBounds` 참조 — 머리 주석에 줄 대조표), MetaCompiler 시험 `Meta.BindsEnumMirrorPrivateMembersAndValidate`·`Meta.BindsEnum*`(ToolTests `MetaTests.Binds.cs`)다.

- **I1 대상 형식** — UE `NoExportTypes.h`의 USTRUCT 다섯(FInterpCurveFloat·Vector2D·Vector·Quat·LinearColor)은 UE 5에서 `float`·`FVector2D`(double)·`FVector`(double)·`FQuat`(double)·`FLinearColor`(float)다(UE `MathFwd.h` 47–50, `InterpCurvePoint.h` 446–451). 미러는 `InterpCurve<float>`·`<Vec2d>`·`<Vec3d>`·`<Quatd>`·`<LinearColor>`와 각 `InterpCurvePoint`로 12.1절과 같다. 미러 이름은 `InterpCurve{Float,Vector2d,Vector,Quat,LinearColor}Binding`·`InterpCurvePoint…Binding`이고 헤더는 `CoreTypeBindings/InterpCurveTypes.h`다.
- **I2 enum Binds(19절 확인 결과)** — MetaCompiler에 Core enum Binds가 없어 더했다: `ROCKS_ENUM(Binds = "rocks::InterpCurveMode") enum class InterpCurveModeBinding : uint8 { … }`. 생성 코드는 실제 enum의 `GeneratedType`을 내고(스키마 이름은 미러 이름), 미러 열거자마다 `static_assert(static_cast<int64>(실제::이름) == 값)`, underlying 같음을 대조한다. 미러가 나열한 열거자만 선언 값이다. 실제 enum 필드(클래스 필드·미러 멤버)는 미러로 풀린다. descriptor enum 항목에 `binds` 칸을 더했다. 대상이 enum이 아니면 RMC3000, CoreTypeBindings 밖이면 RMC2004다.
- **I3 private 멤버** — Core `InterpCurve<T>`가 `template <class> friend struct ::rocks::GeneratedType;`을 선언하고(Core는 `GeneratedType`을 앞 선언만 한다) 미러가 `points_`·`looped_`·`loopKeyOffset_`을 `Name = "points"`·`"looped"`·`"loopKeyOffset"`로 나열한다. 생성 코드(대조 static_assert·구조적 바인딩·멤버 포인터 thunk·깊은 복사·검증 트리)는 모두 `GeneratedType` 특수화 안이라 friend로 닿는다 — MetaCompiler 생성 코드는 바꾸지 않았다(구조적 바인딩은 C++20 접근 가능한 멤버 규칙). friend가 없는 private 멤버는 지금처럼 생성 코드 컴파일이 거부한다.
- **I4 미러 Validate** — Binds 미러의 `static Result<void> Validate(const 실제 타입&) noexcept`를 검증 hook으로 받는다(CV3이 막아 둔 것을 풀었다). 생성 코드는 미러 struct의 함수를 부르고 시그니처를 static_assert로 대조한다. InterpCurve 미러의 규칙(`ValidateInterpCurve`): 키 입력값 유한·비내림차순, `loopKeyOffset` 유한, 루프면 키가 있고 offset > 0(Core 불변식 "루프 offset은 유한한 양수"와 같다 — 12.1절의 "루프면 키가 있다"에 양수를 더했다). 모드 enum 범위는 enum decode가 이미 본다.
- **I5 TryCalcBounds** — `Result<CurveBounds<T>> TryCalcBounds() const`(`CurveBounds{minimum, maximum}`), 구간 식은 `math::CurveSegmentBounds`(UE `FindBounds`를 같은 식·연산 순서로 — 구간 길이 float 뺄셈, 0 허용오차 1e-8f·1e-8, UE `Min`·`Max` 선택 규칙). 지원 형식은 새 성질 `CurveValueTraits::HasBounds`(scalar·Vector2/3·LinearColor 참, Vector4·quaternion 거짓 — requires로 컴파일을 막는다). 구간 수는 Eval과 같다(루프면 키 수 — 키 하나의 루프 커브도 자기 자신으로 가는 닫는 구간 하나를 본다. UE는 키 하나면 그 값만이라 Eval이 내는 곡선 극값을 놓친다 — 15절 22에 묶는다). 닫는 구간의 끝 시간은 `마지막 키 + loopKeyOffset`(float 덧셈)이다 — Eval은 구간 길이로 offset 자체를 쓰므로 두 값은 float 반올림만큼 다를 수 있다(UE 비교 기준을 유지했다).
- **I6 golden 범위** — 무작위 3,000 × 일곱 형식(float·double·Vec2·Vec2d·Vec3·Vec3d·LinearColor)이 닫는 구간 수정을 켠 UE 참조와 비트가 같고 UE 그대로와는 루프 커브에서만 다르며, 촘촘한 Eval 표본이 범위 안이다. 손 계산 벡터(곡선 두 키 극값 4/9, 루프 닫는 구간 극값 −4/9 — UE는 찾지 못한다)와 NaN 값의 UE 선택 규칙도 본다.

### 17.9 남은 일과 후속 과제(CV10)

Curve 단계 CV1–CV10을 마쳤다(2026-11-18). 다른 문서에 반영한 곳: [에셋 시스템](AssetSystem.ko.md) 1절 Curve 행, [Core 상세 명세](CoreDetailedSpec.ko.md) G09, [키 커브 구현](../Implementation/InterpCurve.ko.md) 2절 표·4절 7·5절, [리플렉션 보강](ReflectionExtensions.ko.md) 23·24절 보강과 25절(Binds 확장), [리플렉션·빌드 보강](ReflectionFoundations.ko.md) 4.5절·지정자 표.

남은 일:
- 묶음 끝 스펙·문서 관점 리뷰(17절 — CV9 코드와 함께 리뷰어 1명).

UE 기능 중 두지 않은 것(결정·이유는 해당 절):
- `RCTM_SmartAuto`(D5 — 시퀀서 채널을 만들 때), `RCIM_None`·`RCTM_None`·`RCCE_None`(D4), `ReadjustTimeRange`(D12 — 시퀀서·도구 층).
- 키 핸들의 저장·undo 기록(D3 — 리플렉션 확장 CU4가 정한다), UE 핸들 판 `GetNextKey`·`GetPreviousKey`·핸들 집합 `ScaleCurve`·Bake(H4 — index 연산과 `TryGetHandle`로 한다).
- UE 압축 평가(`FCompressedRichCurve::Eval`)의 참조 이식(P10 — Rocks 평가는 원본과 비트 대조한다), `FInterpCurveTwoVectors`(Rocks 값 형식 없음), double·float 벡터 `InterpCurve` 미러(I1 — 소비자가 생길 때).
- UE 이진 키 직렬화(`FRichCurveKey` 27 byte — RC 163–181)와 열 단위 키 배열·전용 codec(D8 — 저작 데이터는 RecordList wire, 런타임은 `PackedTangentCurve`. 크기가 측정상 문제가 되면 에셋 층에서 다시 본다).
- 에셋 층(`UCurveFloat`·`UCurveVector`·`UCurveLinearColor`·`UCurveTable`·`FRuntimeFloatCurve`·`FCurveTableRowHandle` — 14절, A 단계)과 UE 에셋 반입.

UE 결함 기록: 15절의 ★ 항목이 UE 5.8.4에서 확인한 결함과 Rocks 수정이다(순환 0 길이·SSE 몫, Lerp 끝점, 근 2개 가중, Bake 누적, 범위판 중복 제거 기준, 계단 커브 남은 키 기본값, 압축 표본 탐색·Weighted 판정 위치·순환 오프셋·풀기 접선 손실·Empty 무한 반복, Constant 허용오차, Weighted 형식 판정, InterpCurve 닫는 구간, 회전 풀기, 표본 상한, 손실 키 범위 밖 읽기). UE에 보고할 때는 15절 문구와 해당 시험의 반례(`*Counterexample*`·`*_FormatDecisionFixes`·`InterpCurveBounds_HandVectors`)를 쓴다.

알려 둘 한계:
- 가중 경로(`atanf`·double libm)의 비트 결정성은 Windows x64 릴리스 C 런타임 조건에서만 UE와 맞춘다(D17·K12). 다른 플랫폼은 상대 오차 대조다.
- 압축 Quantized16은 표본 시간에서만 오차 상한을 보장한다(P4 — 표본 사이는 UE처럼 보장하지 않는다).
- 19절의 확인하지 못한 항목(SmartAuto 세부 식, `FRichCurveKey` 실제 크기, UE 쿡 데이터의 Weighted 빈도)은 설계에 영향이 없어 열어 둔다.

## 18. 결정 목록

권장안으로 확정했다(2026-11-18, 교차검증 1차 반영).

- **D1 시간·값 정밀도** — float(UE와 같음). double 커브가 필요해지면 템플릿이 아니라 구체 타입(`TangentCurveDouble` 같은 별도 `ROCKS_STRUCT`)을 따로 둔다 — 리플렉션 struct는 템플릿일 수 없다(RMC2000).
- **D2 다형성** — virtual 기반 없이 상속 + 자유 함수. 공통 칸은 리플렉션 base `ScalarCurveBase`다. 다형 저장(`InstancedStruct<ScalarCurveBase>`)을 쓸지는 에셋 층이 정한다. 쓰면 평가 분기는 담긴 타입으로 고르는 자유 함수이고, 모르는 파생 타입·base 자체는 Unsupported, 비어 있으면 NotFound다(14절).
- **D3 키 핸들** — 값 밖의 편집 표(`CurveKeyHandleTable`), 표마다 1부터 단조 uint32·0 무효·Overflow 거부, 저장 안 함, 복사·대입은 핸들 그대로. 표는 커브를 쥐지 않고 함수마다 인자로 받는다. 표는 마지막으로 맞춘 키 배열의 사본을 쥐고 호출마다 비트 비교해, 표를 거치지 않은 수정이면 표 전체를 무효(StaleHandle)로 하고 `TryRebuild`로 다시 발급한다.
- **D4 UE 없음 열거자** — `RCIM_None`·`RCTM_None`(숨김)과 `RCCE_None`(보이는 열거자, 동작은 Cycle)을 두지 않는다(번호 3·3·5는 비워 둔다).
- **D5 SmartAuto** — 두지 않는다(UE `FRichCurve`가 계산하지 않는 MovieScene 전용 모드). 시퀀서 채널을 만들 때 그쪽에서 연다.
- **D6 "없음" 기본값** — `Optional<T>`(UE sentinel 대신). 압축 Empty 형식의 기본값도 Optional이다.
- **D7 불변식과 struct 검증 hook** — 채택. hook 범위는 정렬·유한 키 시간·압축 정합이다. `PlainCurve` 보간은 별도 enum `PlainCurveInterp {Linear, Constant}`로 Cubic을 표현할 수 없게 하고, 키 배열은 private PROPERTY + 접근자로 C++ 쪽 불변식을 API가 지킨다. hook 계약(noexcept·무할당·순수·아무 스레드·게시 전 1회)과 중첩 위치 규칙은 9.1절. CV3, 규모 L.
- **D8 wire 크기** — 키당 약 350 byte(UE 27 byte의 약 13배)로 정정했고 CV5에서 실측했다(`TangentCurveKey` 352 byte·`PlainCurveKey` 116 byte — 9.2절). 저작 데이터는 리플렉션 RecordList wire를 유지하고 런타임 데이터는 `PackedTangentCurve`로 확정했다. 열 단위 배열·전용 codec은 에셋 층에서 실제 크기가 문제로 측정될 때 다시 본다.
- **D9 Name 계단 커브** — UE `FNameCurve`에는 평가 함수도 기본값 칸도 없다. Rocks는 `Optional<Name>` 기본값을 더하고 String과 같은 규칙(첫 키 이전은 기본값)으로 평가한다.
- **D10 접선 재계산 시점** — 편집 연산은 몰래 다시 계산하지 않고 `AutoSetTangents`를 명시적으로 부른다. UE가 재계산하는 연산(`DeleteKey`·`SetKeys`·`SetKeyTime` 안의 삭제·`RemoveRedundantKeys`)과는 결과가 다르고, 호출자가 같은 지점에서 `AutoSetTangents`를 부르면 같다.
- **D11 값 범위** — Cubic 구간 극값을 해석적으로 구한다. 가중 구간은 Evaluate가 실제로 쓰는 매개변수 집합(x가 단조면 [0, 1], 아니면 7.3절의 S — 증가 조각 중 오른쪽 최솟값보다 작은 부분) 위에서 구하고, 뛰는 점은 두 쪽 극한을 모두 넣는다. 3차 방정식 풀이의 판별식 띠(|D| ≤ 1e-8)에서 풀이가 실제로 고르는 매개변수도 넣어 Evaluate가 내는 값을 모두 담는다(17.4절 C10).
- **D12 ReadjustTimeRange** — 값 타입에 두지 않는다(시퀀서·도구 층의 시간 범위 맞춤 기능).
- **D13 압축 바이트 순서** — 리틀 엔디언 명시, union 없이 칸을 나눈다(`constantValue`·`emptyDefault`).
- **D14 압축 평가 식과 형식 결정** — 원본과 같은 핵심 식(`EvalCurveSegment`)을 쓴다. 형식 결정은 구간 단위 가중 판정으로 고치고, Constant는 비트 같은 값만이다. 그래서 Float32 압축이 원본과 비트가 같다(UE 압축 평가의 곱셈 순서·허용오차를 따르지 않는다).
- **D15 모듈** — 수학 핵심은 Core(`KeyCurveMath.h`, 접선 시간 차는 `InterpCurveMath.h` 재사용), 프레임 시간은 Core `Time`, 값 타입은 새 Core 런타임 모듈 `Curves`, InterpCurve 미러는 `CoreTypeBindings`, Engine 층에는 두지 않는다.
- **D16 순환 cycle 수 범위 밖** — |float 몫| < 2^30이면 double로 넓혀 `TryFloorToInt32`로 계산하고(UE와 같은 식), 그 밖이면 double floor·나머지(fmod 계열)로 재배치해 순환 의미(정확한 배수에서 max로 가는 규칙 포함)를 지킨다. Fatal로 가는 경로가 없다(5.1절).
- **D17 비트 기준** — Windows x64에서 UE와 비트가 같은 것을 목표로 한다. UE `SinCos(float)` 다항식은 그대로 옮긴다(플랫폼과 무관하게 결정적). `atanf`와 double libm(`acos`·`cos`·`sqrt`·`pow`)은 UE와 같은 함수를 쓰고, 그 부분의 플랫폼 간 비트 결정성은 보장하지 않는다(UE와 같은 수준). DeterministicMath 함수로 바꾸지 않는다. CV1에서 확인한 조건(17.1절 K12): 비교 대상은 UE `/fp:precise` 빌드(Editor·Program 타깃 — 게임 타깃 기본은 `/fp:fast`라 식 자체의 비트가 정해지지 않는다)이고, 두 쪽 모두 릴리스 C 런타임(`ucrtbase.dll`, `/MD`)일 때다.
- **D18 바이트 덩어리 리플렉션 값** — 기반에 보강해 채택한다. `Array<uint8>` 필드·멤버·원소의 wire를 ValueArray(원소당 U32 4 byte)에서 Bytes 블록 하나(u64 길이 + byte)로 바꾼다(하위호환 없음 — 지금 쓰는 곳은 시험 fixture뿐). 경로 원소 접근과 JSON 표기 등 세부는 ReflectionExtensions 개정에서 정한다. `PackedTangentCurve::packed`가 쓴다. CV4.
- **D19 InterpCurve 미러·값 범위** — 이 Curve 단계에 넣는다. 미러 대상은 UE USTRUCT가 있는 다섯 형식이다. private `points_`·`looped_`·`loopKeyOffset_`는 Core 타입의 `GeneratedType` friend 선언과 MetaCompiler의 Binds private 멤버 접근 보강으로 노출한다(접근자 미러가 아니다). 도구 이름은 `Name=`으로 둔다. 미러에 `Validate`를 둔다. `TryCalcBounds`는 UE `FindBounds` 식을 옮기고 루프 닫는 구간만 고친다. CV9.
- **D20 FrameRate·FrameTime** — Core `Time`에 최소 타입 `FrameRate`(분자·분모 int32)·`FrameNumber`(int32)·`FrameTime`(프레임 + subframe float)을 두고 UE `AsFrameTime`·`AsSeconds` 식을 옮긴다. `RemoveRedundantKeys` 표본판이 `Optional<FrameRate>`로 쓴다. 제거 뒤 `AutoSetTangents`를 부르지 않는 D10의 결과 차이는 7.2절에 적었다. CV2.
- **D21 압축 정합** — 평가 진입에서 O(1) 크기 대조(enum 범위, keyCount ≥ 2, `packed.Size() == ExpectedPackedSize(…)`)를 하고, 불일치면 `Evaluate`는 quiet NaN, `TryEvaluate`는 InvalidArgument다. 검증 hook은 같은 정합과 byte 값·시간 정렬까지 보고 decode·대입에서 거부한다.

## 19. 확인하지 못한 것

- UE `RCTM_SmartAuto`의 세부 식(시퀀서 CVar별 분기) — D5로 범위 밖이라 확인하지 않았다.
- UE `FRichCurveKey`의 실제 메모리 크기(28바이트로 보이지만 static_assert로 확인하지 않았다) — Rocks 설계에 영향 없음.
- ~~가중 경로의 `atanf`·double libm이 Rocks Windows x64 빌드(Clang + MSVC 런타임)와 UE 빌드(MSVC)에서 같은 C 런타임 구현으로 묶이는지~~ — CV1에서 확인했다(17.1절 K12): 같다. D17의 Windows x64 비트 목표를 유지한다(릴리스 C 런타임 조건).
- ~~리플렉션 키 wire의 정확한 byte 수(기본값 필드 생략 여부)~~ — CV5에서 실측했다(9.2절): 생략 없이 키당 352 byte.
- ~~Core enum(`InterpCurveMode`)의 Binds 미러 지원 여부, Binds 생성 코드가 실제 타입의 private 멤버에 `GeneratedType` friend로 닿는 경로~~ — CV9에서 확인했다(17.8절 I2·I3): enum Binds는 없어 더했고, private 멤버는 friend로 생성 코드 수정 없이 닿는다.
- UE 압축 Weighted 결함(11절)이 UE 쿡 데이터에서 실제로 드러나는지(애니메이션 코덱이 Weighted를 고르는 빈도) — Rocks 설계는 결함을 고치는 쪽이라 확인하지 않아도 된다.
- ~~가중 구간 값 범위(7.3절)의 비단조 판정이 x′ 근의 수치 오차로 갈리는 경계~~ — CV5 단계 리뷰에서 실제 문제는 x′ 근이 아니라 Evaluate 풀이의 판별식 띠(|D| ≤ 1e-8)였다. 범위가 그 띠에서 풀이가 고르는 매개변수를 넣도록 고쳤고, 띠를 겨냥한 무작위 표본(접힘 점 근처 포함)으로 확인했다(17.4절 C10).

## 20. 변경 이력

- **개정 1 초안(2026-11-18, 36027f9)**: 값 타입·평가·편집·핸들·압축 설계, 결정 D1–D15, 단계 CV1–CV7.
- **개정 12(2026-11-18, CV10)**: 17절 진행 표에 상태 칸(CV1–CV10 완료·커밋), 17.9절(다른 문서 반영 목록·남은 일·두지 않은 UE 기능·UE 결함 기록·한계). 다른 문서: 에셋 시스템 Curve 행, Core 상세 G09, InterpCurve 구현 문서, 리플렉션 보강 23–25절, 리플렉션·빌드 보강 4.5절.
- **개정 11(2026-11-18, CV9 구현·CV8 단계 리뷰 반영)**: 17.8절 I1–I6(InterpCurve 미러 다섯 형식·enum Binds·private 멤버 friend·미러 Validate·`TryCalcBounds`), 19절 확인 결과. CV8 리뷰 — 15절 32·33(UE 손실 키 범위 밖 읽기, UE Clamp), 17.7절 P3 예시·P4(비유한 역수·간격·복원 시간은 Float32, 시간 전용 탐색)·P9(이동된 쪽 Empty)·P10, 15절 30 표본 식, 17.5절 S5(Name 토큰 모든 칸).
- **개정 10(2026-11-18, CV8 구현·CV6·CV7 묶음 리뷰 반영)**: 리뷰 — 17.6절 H4(`TryEditKeys` 키별 칸 편집자·`TryAutoSetTangents`·표 판 중복 제거·Shift·Scale, UE `ShiftCurve` 핸들 차이)·H5 예외·H8·H9·H10, 17.5절 S5(Name 중복 제거는 표시 철자까지)·S7, 7.3절 반례 값 정정. CV8 — 17.7절 P1–P10(압축 커브 — Constant 외삽·유한 조건, 키 시간 판정을 Evaluate와 같은 평가로, 칸 정규형, 한도, 평가·풀기), 11.1·11.2·11.4절 정정(Bytes wire는 CV4에서 적용됨), 15절 30–31.
- **개정 9(2026-11-18, CV7 구현 반영)**: 17.6절 H1–H8(키 핸들 표 — 칸별 비트 비교 감지, 표 판 편집 함수, 원자성, `TryEditKeys`).
- **개정 8(2026-11-18, CV6 구현·CV5 단계 리뷰 반영)**: 17.5절 S1–S7(계단 커브 셋 — NaN 시간은 기본값, String 빈 기본값, 중복 제거의 같음 규칙, `FindKeyAtOrBefore`). CV5 리뷰: 값 범위가 풀이 판별식 띠의 매개변수까지 담음(7.3절·C10·D11·19절), 회전 풀기 UE 비트 일치(C6), 표본판 상한·NaN 키 보존(C9), 순환 int32 경로 벗어남 표현(5.1절·C4), golden 참조 표현(C13), 15절 23–29, D8 확정, 7.1표.
- **개정 7(2026-11-18, CV5 구현·CV4 단계 리뷰 반영)**: 17.3절 B8·B5, 17.4절 C1–C14(`Curves` 모듈, 값 범위의 일반 매개변수 집합, API 이름, 허용오차·회전 풀기·Bake·중복 제거 경계), 9.2절 키 wire 실측과 저작 wire 유지 판단, D8·19절 갱신.
- **개정 6(2026-11-18, CV3 단계 리뷰 반영)**: 17.2절 V9–V12.
- **개정 5(2026-11-18, CV4 구현 반영)**: 17.3절 B1–B7(`Array<uint8>` Bytes 블록 wire, Base64 텍스트).
- **개정 4(2026-11-18, CV1·CV2 단계 리뷰·CV3 구현 반영)**: 순환 int32 경로를 |몫| < 2^30으로 좁히고 double 경로의 끝 바로잡기를 뺌(5.1절, 15절 1, D16, 17.1절 K3·K8), UE 비트 범위 기준을 Bezier에 넣는 비율로(15절 2, 16절, K13), 17.2절 V1–V8(struct 검증 hook).
- **개정 3(2026-11-18, CV1·CV2 구현 반영)**: 17.1절 K1–K12. 5.4절 double 시간 혼용 설명 정정(float와 늘 같음)과 근 고르기 문구, 16절 해당 시험, D17 조건(UE `/fp:precise`·릴리스 C 런타임), 19절 1번 확인 결과.
- **개정 2(2026-11-18, 교차검증 1차 반영, 결정 D1–D21 확정)**:
  - 사실 정정: UE `SinCos(float)`는 libm이 아니라 다항식(5.4절, D17). 가중 경로의 키 시간 double 혼용을 의사코드에 반영(5.4절). `SolveCubic` 연산 순서를 UE 식 그대로 적음. Windows x64 UE `FloorToInt`는 SSE 식(5.1절). `RCCE_None`은 숨김이 아님(2절, D4). UE `SetKeyTime`은 안의 `DeleteKey`로 접선을 재계산함(6절). `PlainCurve`·`TangentCurve`의 첫 키 시간 중복 차이(5.7절), 마지막 키 시간 중복 결과(5.2절). `Lerp` 끝의 차 넘침 예외(5.3절, 15절 2). 계단 커브 중복 제거의 Integer 조건(15절 9). UE `FNameCurve`에 기본값이 없음(4절, D9). wire 크기 약 13배(9.2절, D8). 압축 보간 byte는 형식 값(11.3절). 압축 "앞 경계 비교" 차이 주장은 결과 차이가 없어 지움(11.5절).
  - 치명 반영: cycle 몫 넘침이 Rocks `FloorToInt32` Fatal로 가는 경로를 D16으로 막음(5.1절). 압축 칸 정합 미검증의 메모리 밖 읽기를 D21(평가 진입 O(1) 대조)과 hook으로 막음(11.4절).
  - 설계 보강: struct 검증 hook 계약·중첩 위치(9.1절, D7), private 키 배열과 `PlainCurveInterp`(4절), 키 핸들 표의 연결·감지 방식(8절, D3), 압축 형식 결정의 구간 단위 가중·비트 같음 Constant(11.1절, D14), Empty 기본값 Optional(11.4절), 값 범위의 가중 비단조 처리(7.3절, D11), `Array<uint8>` Bytes 블록 wire(D18), InterpCurve 미러와 `TryCalcBounds`(12절, D19), `FrameRate`·`FrameTime`과 중복 제거의 D10 관계(7.2절, D20), 선택 키 이동(7.1절), 자동 접선 시간 차 하한 재사용(3·6절), D1의 구체 타입, D2의 에셋 층 위임과 모르는 파생 타입 규칙(14절).
  - 시험·단계: 독립 고정 벡터, 참조 코드 원본 대조 리뷰, SSE `FloorToInt` 참조, 넘침·변조 packed·hook·Bytes·FrameRate·InterpCurve 미러 시험 추가(16절). 단계를 CV1–CV10으로 늘리고(FrameRate·hook·Bytes·packed 정합·InterpCurve 미러) 교차검증을 단계마다 둠(17절).
