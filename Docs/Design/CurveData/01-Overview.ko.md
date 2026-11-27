# 커브 데이터 — 요약·타입·모듈·데이터

[목차](README.ko.md)

## 1. 요약

- 만드는 것은 스칼라 키 커브 둘(접선 있는 커브, 접선 없는 커브), 계단 커브 셋(정수·Name·String), 압축된 접선 커브 하나, 편집기·도구가 쓰는 키 핸들 표, 그리고 이미 있는 `InterpCurve<T>`의 리플렉션 미러와 `TryCalcBounds`다.
- 비트 기준은 **Windows x64에서 UE와 같은 비트**다(D17). 평가 식은 UE `FRichCurve::Eval`과 같은 식을 같은 연산 순서·같은 정밀도(float, 그리고 UE가 double을 섞는 곳은 double)로 옮긴다. UE `FMath::SinCos(float)` 다항식은 그대로 옮긴다. `atanf`와 double libm 함수는 UE와 같은 함수를 부르므로 플랫폼 간 비트 결정성은 UE와 같은 수준까지만 보장한다. 다르게 정한 곳은 UE 결함이나 Rocks 규약 때문인 곳뿐이고 15절에 모두 적는다.
- 값 타입은 처음부터 리플렉션 struct(`ROCKS_STRUCT`)로 만든다. B9에서 연 struct 상속(B9k)·Optional(B9e)·Name(B9c)·enum underlying(B9a2)·Atomic(B9b)을 쓴다. 새 값 타입에는 미러(`Binds`)를 쓰지 않는다. `InterpCurve<T>`는 Core 타입이라 미러로 리플렉션한다(12절).
- 리플렉션 기반을 셋 보강한다: struct 검증 hook(D7), `Array<uint8>`의 Bytes 블록 wire(D18), 미러의 private 멤버 접근(D19). 셋 다 [리플렉션 보강](../ReflectionExtensions.ko.md)에 넣을 변경이다.
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
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Optional<float> defaultValue;              // 키가 없을 때의 값(UE DefaultValue = MAX_flt는 "없음")
    ROCKS_PROPERTY() CurveExtrapolation preExtrapolation = CurveExtrapolation::Constant;
    ROCKS_PROPERTY() CurveExtrapolation postExtrapolation = CurveExtrapolation::Constant;
};

ROCKS_STRUCT(Atomic)
struct TangentCurveKey {
    ROCKS_GENERATED_BODY()

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
struct PlainCurveKey {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float time = 0;
    ROCKS_PROPERTY() float value = 0;
};

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
