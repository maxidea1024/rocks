# 키 기반 보간 커브 — Core 수학 층(G09)

기준일: 2026-11-16. 이 문서는 [Core 상세 명세](../Design/CoreDetailedSpec.ko.md) G09와 [에셋 시스템](../Design/AssetSystem.ko.md) 1절 Curve 행 중 **Core 수학 층**의 설계다. 대상은 UE 5.8 `FInterpCurve<T>`·`FInterpCurvePoint<T>`(5.8.4, `Engine/Source/Runtime/Core/Public/Math/InterpCurve.h`·`InterpCurvePoint.h`, 보조 식은 `Math/UnrealMathUtility.h`의 `CubicInterp*`, `Private/Math/UnrealMath.cpp`의 `ClampFloatTangent`·`TQuat::CalcTangents`·`Squad`·`Log`·`Exp`)다. `FRichCurve` 계열 값 타입, 리플렉션 미러, 에셋 타입은 이 단계 밖이다. 기본은 UE 5.8과 같은 의미이고, 다른 곳은 4절에 근거를 적는다.

## 1. API

헤더는 `Core/Public/Core/Math/` 아래 넷이다.

| 헤더 | 내용 |
|---|---|
| `InterpCurvePoint.h` | `InterpCurveMode`(Linear·CurveAuto·Constant·CurveUser·CurveBreak·CurveAutoClamped, UE 번호와 같은 순서), `CurveValueTraits<T>`(값 형식별 scalar·성분·지원 기능), `InterpCurvePoint<T>` |
| `InterpCurveMath.h` | 성분별 보간 식 `math::CurveHermite`·`CurveHermiteDerivative`·`CurveHermiteSecondDerivative`(UE 항 순서), `math::AutoCurveTangent`·`math::ClampCurveTangent`(UE `AutoCalcTangent`·`ClampFloatTangent`), quaternion `QuaternionLog`·`QuaternionExp`·`SlerpFullPath`·`SquadControlPoint`·`Squad` |
| `PolynomialRoots.h` | `math::FindPolynomialRootsInRange<Degree>`·`FindPolynomialCriticalPointsInRange` — 최근접 탐색의 5차 방정식 실근(UE `TPolynomialRootSolver` 역할) |
| `InterpCurve.h` | `InterpCurve<T>`와 형식 별칭 |

```cpp
template <typename T>
class InterpCurve {                       // 이동 전용, Allocator를 빌려 키 배열을 소유
public:
    using Scalar = typename CurveValueTraits<T>::Scalar;  // 입력 키·접선 시간 단위의 정밀도
    using Point = InterpCurvePoint<T>;
    static constexpr usize NoSegment = IndexNone;

    explicit InterpCurve(Allocator& allocator) noexcept;

    Result<usize> TryAddPoint(Scalar inValue, const T& outValue) noexcept;  // Linear, 접선 0
    Result<usize> TryAddPoint(const Point& point) noexcept;
    Result<usize> TryMovePoint(usize index, Scalar newInValue) noexcept;
    Result<void> TryRemovePoint(usize index) noexcept;
    Result<void> TryReserve(usize count) noexcept;
    Span<const Point> Points() const noexcept;
    const Point& GetPoint(usize index) const noexcept;
    void SetOutValue(usize index, const T& value) noexcept;
    void SetTangents(usize index, const T& arrive, const T& leave) noexcept;
    void SetMode(usize index, InterpCurveMode mode) noexcept;
    void Reset() noexcept;
    Result<void> TryCopyFrom(const InterpCurve& other) noexcept;
    friend bool operator==(const InterpCurve&, const InterpCurve&) noexcept;

    Result<void> TrySetLoopKey(Scalar loopKey) noexcept;
    void ClearLoopKey() noexcept;
    usize SegmentCount() const noexcept;                    // 루프면 키 수, 아니면 키 수 − 1

    usize FindSegmentIndex(Scalar input) const noexcept;    // UE GetPointIndexForInputValue
    T Eval(Scalar input, const T& emptyValue = 기본값) const noexcept;
    T EvalDerivative(Scalar input, const T& emptyValue = 0) const noexcept;        // quaternion 제외
    T EvalSecondDerivative(Scalar input, const T& emptyValue = 0) const noexcept;  // quaternion 제외
    void AutoSetTangents(Scalar tension = 0, bool stationaryEndpoints = true) noexcept;
    Result<Interval<Scalar>> TryGetKeyInterval() const noexcept;
    Result<CurveNearestPoint<Scalar>> TryFindNearest(const T& position) const noexcept;  // 거리 있는 형식
    CurveNearestPoint<Scalar> FindNearestOnSegment(const T& position, usize segment) const noexcept;
};
```

값 형식은 `float`·`double`·`Vec2/2d`·`Vec3/3d`·`Vec4/4d`·`Quat/Quatd`·`LinearColor`다(별칭 `InterpCurveFloat`·`InterpCurveVector` 등). 8bit `Color`는 보간 대상이 아니다. 접선은 "입력 1단위당 값 변화"이며 quaternion만 접선 칸에 SQUAD 제어점(단위 quaternion)을 담는다(UE와 같은 저장 규약). 인스턴스에 동기화는 없다. 변경 없는 동안의 const 호출은 여러 스레드에서 함께 해도 된다. `Points()` view와 `GetPoint` 참조는 키 추가·이동·삭제·`Reset`·`TryCopyFrom`에서 무효다.

## 2. UE 5.8 대응표

| UE 5.8 | Rocks | 비고 |
|---|---|---|
| `EInterpCurveMode`(`CIM_*`) | `InterpCurveMode` | `CIM_Unknown` 없음 |
| `FInterpCurvePoint<T>` | `InterpCurvePoint<T>` | 필드 `inValue`·`outValue`·`arriveTangent`·`leaveTangent`·`mode` |
| `IsCurveKey` | `InterpCurvePoint::IsCurveKey` | 같음 |
| `FInterpCurve::Points`(공개 배열) | `Points()`·`GetPoint`·`Set*` | 정렬 불변식을 지키려고 입력 키 직접 수정을 막음 |
| `AddPoint`·`MovePoint`·`Reset` | `TryAddPoint`·`TryMovePoint`·`Reset` | 비유한 키·할당 실패를 결과로 |
| `SetLoopKey`·`ClearLoopKey`·`bIsLooped`·`LoopKeyOffset` | `TrySetLoopKey`·`ClearLoopKey`·`IsLooped`·`LoopKeyOffset` | 비유한 루프 키 거부 |
| `GetPointIndexForInputValue` | `FindSegmentIndex` | -1 대신 `NoSegment` |
| `Eval`·`EvalDerivative`·`EvalSecondDerivative` | 같은 이름 | 4절 |
| `AutoSetTangents` | 같은 이름 | 4절 |
| `FindNearest`·`FindNearestOnSegment`(`Inaccurate*` 폐기 별칭) | `TryFindNearest`·`FindNearestOnSegment` | 구간 번호 usize |
| `GetKeyInterval` | `TryGetKeyInterval` | 빈 커브는 NotFound |
| `operator==`·`operator!=` | `operator==`(`!=`는 C++20 합성) | 루프가 꺼져 있으면 offset을 보지 않는 것까지 같음 |
| `CalcBounds`·`Curve*FindIntervalBounds` | 후속 | 이번 범위 밖 |
| `operator<<(FArchive&)` | 후속 | 리플렉션 미러·직렬화 단계 |
| `ClampFloatTangent`·`ComputeCurveTangent`·`AutoCalcTangent` | `math::ClampCurveTangent`·내부 접선 계산 | |
| `FMath::CubicInterp`·`CubicInterpDerivative`·`CubicInterpSecondDerivative` | `math::CurveHermite*` | 기존 `math::CubicInterp`는 합산 순서가 달라 그대로 둔다 |
| `TQuat::CalcTangents`·`Squad`·`SlerpFullPath`·`Log`·`Exp` | `SquadControlPoint`·`Squad`·`SlerpFullPath`·`QuaternionLog`·`QuaternionExp` | 4절 |
| `TPolynomialRootSolver<double, 5>` | `math::FindPolynomialRootsInRange<5>` | 구간 안 근 + 접하는 근 후보 |
| `FInterpCurveTwoVectors` | 없음 | `TwoVectors` 미구현 |

## 3. 의미 결정

- **키 정렬·중복 입력값**: 키는 입력값 오름차순을 항상 유지한다. 새 키는 같은 입력값을 가진 기존 키들 **앞**에 들어간다(UE `AddPoint`의 `Points[i].InVal < InVal`). 같은 입력값이 여럿이면 그 입력에서의 값은 마지막(가장 먼저 넣은) 키이고 왼쪽 극한은 첫 키라 계단이 생긴다. 길이 0 구간은 평가에 쓰이지 않는다. `TryMovePoint`는 값·접선·모드를 유지하고 같은 규칙으로 새 위치를 정한다.
- **빈 커브**: `Eval`은 호출자가 준 `emptyValue`(기본은 0, quaternion은 항등)를 반환한다(UE `Default`). 최근접 탐색·키 구간은 NotFound다.
- **키 하나**: 모든 입력에서 그 키 값이다. 1차 미분은 앞쪽이 `leaveTangent`, 키 이후가 `arriveTangent`, 2차 미분은 0이다(UE와 같다). 루프를 켜면 그 키에서 자기 자신으로 가는 구간이 생긴다.
- **범위 밖(외삽)**: 값은 끝 키 값으로 고정한다(외삽하지 않음). 1차 미분은 첫 키 앞에서 첫 키의 `leaveTangent`, 마지막 키 이후(루프 아님)는 마지막 키의 `arriveTangent`를 반환한다 — 상수 외삽의 미분 0이 아니라 끝 방향을 주는 UE 규약이며 스플라인 방향 계산이 여기에 기댄다. 2차 미분은 0이다. ±Inf 입력도 같은 규칙이다.
- **루프 경계**: 루프는 입력을 감지 않는다. `[마지막 키, 마지막 키 + offset)`에 마지막 키 → 첫 키 값의 닫는 구간을 더할 뿐이다(첫 키의 `arriveTangent` 사용). `마지막 키 + offset` 이상은 첫 키 값, 첫 키 앞은 첫 키 값이다. 주기 평가는 호출자가 `math::Wrap`으로 입력을 감는다. offset은 마지막 키 기준이라 키를 더하면 루프 끝도 따라 움직인다. 루프 키가 마지막 키 이하이거나 키가 없으면 루프를 끈다(UE). 자동 접선은 첫 키의 이전 키를 `첫 키 − offset`의 마지막 키로 본다. `stationaryEndpoints`가 참이면 루프에서도 첫 키 접선은 0이다(UE) — 이음매에서 멈추지 않으려면 false를 준다.
- **접선 연속성**: CurveAuto·CurveAutoClamped는 도착·출발 접선을 같게 계산해 키에서 1차 미분이 연속이다. CurveUser는 같은 값을 쓰는 것이 약속이고(검사하지 않는다) CurveBreak만 꺾임을 허용한다. Linear 키의 자동 접선은 이웃 구간의 **기울기**(값 차 ÷ 입력 차)이고, Linear·Constant 뒤의 자동 곡선 키는 앞 키 접선을 복사하므로 Linear → 곡선 전환에서도 1차 미분이 연속이다(4절 차이 1). 복사가 우선이라 Linear 뒤의 CurveAutoClamped 키는 clamp되지 않는다(UE와 같다).
- **quaternion 보간 방식**: Linear는 최단 경로 `Slerp`, Constant는 유지, 곡선 모드는 Shoemake SQUAD `Slerp(Slerp(q0, q1, t), Slerp(s0, s1, t), 2t(1 − t))`다. 구간마다 q1이 q0과 다른 반구면 q1·s1을 함께 뒤집어 최단 경로를 택하고, 안쪽 세 slerp는 다시 뒤집지 않는다. 자동 제어점은 `s = q·exp(−(1 − tension)·(log(q⁻¹q_prev) + log(q⁻¹q_next)) / 4)`(이웃은 q 반구로 맞춤), 고정 끝점·Linear·Constant 키는 `s = q`다. 결과는 정규화한다. 키·제어점은 단위 quaternion이 전제이고 검사하지 않는다. 키 간격이 고르지 않아도 제어점은 시간 비례 보정을 하지 않는다. 미분 평가·최근접 탐색은 제공하지 않는다(4절 차이 4).
- **NaN 입력**: 값·미분이 모든 성분 NaN이다(4절 차이 2). 비유한 키는 `TryAddPoint`·`TryMovePoint`·`TrySetLoopKey`가 InvalidArgument로 거부하고 상태를 바꾸지 않는다. 비유한 출력값은 받아서 결과로 전파한다. 이웃 키 차이가 Scalar 범위를 넘는 키 배치는 지원하지 않는다.
- **정밀도**: 입력 키·접선 시간·보간 비율의 형식은 값 형식의 scalar다(float 계열 float, double 계열 double). UE는 모든 형식에서 float 입력 키를 쓴다(4절 차이 5). 구간 안 비율은 반올림으로 1을 넘으면 1로 자른다(UE는 `checkf`).
- **최근접 탐색**: 거리 있는 형식(`float`·`double`·`Vec2/3/4` 계열)만 지원한다. 구간마다 Constant는 두 끝 키 중 가까운 쪽(같으면 다음 키), Linear는 선분 사영, 곡선은 `(C(t) − p)·C'(t) = 0`의 5차식 근과 두 끝점을 비교한다(UE와 같은 순서·같은 엄격한 `<` 비교라 동률은 앞 후보). 계수와 근은 double로 계산한다. 길이 0 Linear 구간은 끝 키를 고른다(UE `Clamp(NaN)` 결과와 같음).

## 4. UE 5.8과 다른 점과 근거

1. **Linear 키 자동 접선 단위**: UE는 `NextOut − ThisOut`(값 차)를 그대로 저장해 곡선 키 접선(값 차 ÷ 입력 차)과 단위가 다르다. Linear → 곡선 전환 키가 이 값을 복사하므로 구간 길이가 1이 아니면 1차 미분이 끊긴다. Rocks는 같은 식을 `max(KindaSmallNumber, 입력 차)`(UE 곡선 접선과 같은 보호)로 나눈다. 구간 길이 1이면 UE와 비트가 같다.
2. **NaN 입력**: UE는 `Default`(대개 0)를 반환한다. Rocks 수학 규약(G01 — 잘못된 입력을 조용히 0으로 바꾸지 않음)에 따라 NaN을 반환한다.
3. **CurveAutoClamped 성분 범위**: UE는 float·`FVector`·`FVector2D`·`FTwoVectors`만 clamp하고(`FLinearColor`는 무시) 5.8의 double `FVector`도 메모리를 float 배열로 읽는다. Rocks는 quaternion 밖의 모든 형식에서 성분별로 그 형식의 정밀도로 clamp한다.
4. **quaternion**: UE는 제어점을 키 시간차로 나눠(`ComputeCurveTangent`의 `OutTangent /= ...`) 평가 때 다시 구간 길이를 곱하므로 단위 길이가 아닌 제어점이 `SlerpFullPath`의 각도 계산에 들어가 곡선이 일그러진다. 고정 끝점 제어점은 항등 quaternion(`T(ForceInit)`)이라 끝 키가 항등이 아니면 항등 쪽으로 휜다. 계수도 −1/2로 표준 SQUAD(−1/4)와 다르고 tension을 무시한다. Linear·Constant 키에는 quaternion 뺄셈 값을 접선으로 넣는다. 미분은 제어점을 성분별 다항식으로 미분해 의미가 없다. Rocks는 3절의 표준 SQUAD를 쓰고 미분·최근접 탐색을 컴파일 시점에 막는다. Linear slerp는 기존 Rocks `Slerp`(정규화 선형 보간 전환 기준 0.9995, UE 0.9999)다.
5. **입력 키 정밀도**: UE는 float 키(`LWC_TODO: Precision loss` 표시)다. Rocks double 형식 커브는 double 키를 써 긴 시간축·큰 좌표에서도 키 간격이 유지된다. float 계열은 UE와 같은 float 연산이다.
6. **수치 세부**: Linear 보간은 `math::Lerp`라 비율이 반올림으로 정확히 1이 되어도 끝 키 값을 그대로 낸다(UE `A + Alpha·(B − A)`는 반올림이 남는다. 비율 0과 (0, 1) 안은 비트가 같다). 벡터·색의 나눗셈은 성분별 정확한 나눗셈이다(UE `TVector`·`TVector2`·`FLinearColor`의 `/=`는 역수 곱이라 1ulp 다를 수 있다. float 커브는 UE도 정확한 나눗셈). UE의 `FVector` SIMD `CubicInterp` 특수화는 항을 `(s0·P0 + s1·T0) + (s2·T1 + s3·P1)`로 묶고 FMA를 쓸 수 있다. Rocks는 일반 템플릿의 왼쪽부터 합산하는 순서로 고정한다(`-ffp-contract=off`). clamp 접선의 Min/Max는 UE `FMath::Min/Max`와 같은 선택 규칙을 써 ±0 부호까지 같다.
7. **API 형태**: 공개 `Points` 배열 대신 접근자(정렬 불변식), `Try*` 결과, 구간 번호 usize.

## 5. 시험 계획

`Core/Source/Runtime/Core/Tests/MathInterpCurve*Tests.cpp`. 무작위 사례는 고정 seed라 매 실행이 같고, 타이밍에 기대는 시험은 없다.

- **경계(G09)**: 키 정렬·중복 입력값(삽입 위치, 정확한 입력의 값과 왼쪽 극한), 빈 커브(fallback·NotFound), 키 하나(값·미분·루프), 범위 밖(값 고정·끝 접선·2차 미분 0·±Inf), 루프 경계(닫는 구간 시작·끝·offset 이상·첫 키 앞·키 추가 뒤 이동·끄기 조건), 접선 연속성(자동 모드 키 양쪽 극한, Linear → 곡선 전환, CurveBreak의 꺾임), NaN·비유한 키 거부와 상태 보존, `TryMovePoint` 속성 유지.
- **UE 의미 대조 golden**: 시험 안에 UE 5.8 식을 그대로 옮긴 참조 구현(`AddPoint`·`SetLoopKey`·`GetPointIndexForInputValue`·`Eval`·`EvalDerivative`·`EvalSecondDerivative`·`AutoSetTangents`·`ComputeCurveTangent`·`ClampFloatTangent`)을 두고, 4절 차이 1·3·6(Lerp 끝점)을 켜고 끄는 선택지를 둔다. 고정 seed 무작위 커브(모든 모드, 중복 키, 루프·비루프, float·double·벡터·색)에서 Rocks와 차이를 켠 참조가 키 삽입 위치·접선·값·1·2차 미분 모두 **비트 단위로 같고**, 차이를 끈 참조와는 간격 1 정수 키에서 같고 그 밖(Linear 구간 길이 ≠ 1, 색 clamp)에서만 다름을 확인한다.
- **double reference**: 독립 double 구현(멱급수 계수 Horner 평가)과 float 커브 결과를 상대 오차로 대조하고, 수치 미분과 1·2차 미분을 대조한다.
- **AutoClamped**: 단조 키에서 overshoot가 없음과 극점 키 접선 0.
- **quaternion**: 키 통과, 단위 길이, 반구 뒤집기, 독립 double SQUAD 참조와의 각거리, tension 1 = 제어점이 키, Linear = `Slerp`.
- **최근접 탐색**: 조밀 표본 + 황금분할 double oracle보다 거리가 크지 않음, Constant·길이 0 Linear·키 하나·빈 커브·루프 구간, 다항식 근 찾기(중근·접하는 근·구간 끝 근 제외).
