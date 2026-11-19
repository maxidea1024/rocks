# 키 기반 보간 커브 — Core 수학 층(G09)

기준일: 2026-11-16(리뷰 반영 개정 포함), 2026-11-18 Curve 단계 CV9 반영(값 범위 `TryCalcBounds`·리플렉션 미러 — 2절 표·4절 7·5절). 이 문서는 [Core 상세 명세](../Design/CoreDetailedSpec.ko.md) G09와 [에셋 시스템](../Design/AssetSystem.ko.md) 1절 Curve 행 중 **Core 수학 층**의 설계다. 대상은 UE 5.8 `FInterpCurve<T>`·`FInterpCurvePoint<T>`(5.8.4, `Engine/Source/Runtime/Core/Public/Math/InterpCurve.h`·`InterpCurvePoint.h`, 보조 식은 `Math/UnrealMathUtility.h`의 `CubicInterp*`, `Private/Math/UnrealMath.cpp`의 `ClampFloatTangent`·`TQuat::CalcTangents`·`Slerp_NotNormalized`·`Squad`·`Log`·`Exp`)다. `FRichCurve` 계열 값 타입과 리플렉션 미러는 [커브 데이터](../Design/CurveData.ko.md)(Curve 단계)가 맡고 에셋 타입은 A 단계다. 기본은 UE 5.8과 같은 의미이고, 다른 곳은 4절에 UE 코드 근거와 함께 적는다.

## 1. API

헤더는 `Core/Public/Core/Math/` 아래 넷이다.

| 헤더 | 내용 |
|---|---|
| `InterpCurvePoint.h` | `InterpCurveMode`(Linear·CurveAuto·Constant·CurveUser·CurveBreak·CurveAutoClamped, UE 번호와 같은 순서), `CurveValueTraits<T>`(값 형식별 scalar·성분·나눗셈 방식·clamp 여부·지원 기능), `InterpCurvePoint<T>` |
| `InterpCurveMath.h` | 성분별 보간 식 `math::CurveHermite`·`CurveHermiteDerivative`·`CurveHermiteSecondDerivative`(UE 항 순서, 기저 계수는 비율 형식), `math::ClampCurveTangent`(UE `ClampFloatTangent`), quaternion `QuaternionLog`·`QuaternionExp`·`CurveSlerp`(UE `TQuat::Slerp`)·`SlerpFullPath`·`SquadControlPoint`·`Squad` |
| `PolynomialRoots.h` | `math::FindPolynomialRootsInRange<Degree>`·`FindPolynomialCriticalPointsInRange` — 최근접 탐색의 5차 방정식 실근(UE `TPolynomialRootSolver` 역할) |
| `InterpCurve.h` | `InterpCurve<T>`와 형식 별칭 |

```cpp
template <typename T>
class InterpCurve {                       // 이동 전용, Allocator를 빌려 키 배열을 소유
public:
    using Scalar = typename CurveValueTraits<T>::Scalar;  // 성분(값·접선) 정밀도. 키는 항상 float
    using Point = InterpCurvePoint<T>;
    static constexpr usize NoSegment = IndexNone;

    explicit InterpCurve(Allocator& allocator) noexcept;

    Result<usize> TryAddPoint(float inValue, const T& outValue) noexcept;  // Linear, 기본 접선
    Result<usize> TryAddPoint(const Point& point) noexcept;
    Result<usize> TryMovePoint(usize index, float newInValue) noexcept;
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

    Result<void> TrySetLoopKey(float loopKey) noexcept;
    void ClearLoopKey() noexcept;
    usize SegmentCount() const noexcept;                    // 루프면 키 수, 아니면 키 수 − 1

    usize FindSegmentIndex(float input) const noexcept;     // UE GetPointIndexForInputValue
    T Eval(float input, const T& emptyValue = 기본값) const noexcept;
    T EvalDerivative(float input, const T& emptyValue = 0) const noexcept;        // quaternion 제외
    T EvalSecondDerivative(float input, const T& emptyValue = 0) const noexcept;  // quaternion 제외
    void AutoSetTangents(float tension = 0, bool stationaryEndpoints = true) noexcept;
    Result<Interval<float>> TryGetKeyInterval() const noexcept;
    Result<CurveNearestPoint<Scalar>> TryFindNearest(const T& position) const noexcept;  // 거리 있는 형식
    CurveNearestPoint<Scalar> FindNearestOnSegment(const T& position, usize segment) const noexcept;
};
```

값 형식은 `float`·`double`·`Vec2/2d`·`Vec3/3d`·`Vec4/4d`·`Quat/Quatd`·`LinearColor`다(별칭 `InterpCurveFloat`·`InterpCurveVector` 등). 8bit `Color`는 보간 대상이 아니다. 입력 키·루프 offset·구간 길이·보간 비율·tension은 모든 형식에서 float다(UE `FInterpCurvePoint::InVal`·`FInterpCurve::LoopKeyOffset`이 float). 최근접 결과의 `input`도 float, `distanceSquared`는 Scalar다. 접선은 "입력 1단위당 값 변화"이며 quaternion만 접선 칸에 SQUAD 제어점(단위 quaternion)을 담는다(UE와 같은 저장 규약). 인스턴스에 동기화는 없다. 변경 없는 동안의 const 호출은 여러 스레드에서 함께 해도 된다. `Points()` view와 `GetPoint` 참조는 키 추가·이동·삭제·`Reset`·`TryCopyFrom`에서 무효다. 키 추가·예약·복사는 할당이 실패하면 오류를 반환하고 커브를 바꾸지 않는다.

## 2. UE 5.8 대응표

| UE 5.8 | Rocks | 비고 |
|---|---|---|
| `EInterpCurveMode`(`CIM_*`) | `InterpCurveMode` | `CIM_Unknown` 없음 |
| `FInterpCurvePoint<T>` | `InterpCurvePoint<T>` | 필드 `inValue`(float)·`outValue`·`arriveTangent`·`leaveTangent`·`mode` |
| `FInterpCurvePoint(In, Out)` 생성자 | `InterpCurvePoint(input, output)` | 접선을 memset 0으로 두는 것은 같다. **quaternion만 다르다**: UE는 영 quaternion, Rocks는 키 자신(제어점 = 키). 그 키를 CurveUser로 바꾸고 접선을 주지 않으면 UE는 영 제어점이 `SlerpFullPath`에 들어가 결과가 다르다 |
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
| `CalcBounds`·`Curve*FindIntervalBounds`·`FindBounds` | `TryCalcBounds`(`CurveBounds<T>`)·`math::CurveSegmentBounds` | 빈 커브 NotFound(UE는 기본값), float·double·Vec2/3·LinearColor만(`CurveValueTraits::HasBounds` — quaternion·Vector4는 컴파일 거부), 4절 7 |
| `operator<<(FArchive&)`·USTRUCT(`FInterpCurveFloat` 등, NoExportTypes.h) | CoreTypeBindings 미러(`CoreTypeBindings/InterpCurveTypes.h`) | 리플렉션 그래프·JSON·경로·대입 묶음. 다섯 형식(float·Vec2d·Vec3d·Quatd·LinearColor — UE 5 정밀도)과 각 키, `InterpCurveMode` enum Binds. private 멤버는 `GeneratedType` friend로, 도구 이름 `points`·`looped`·`loopKeyOffset`. 미러 `Validate`가 정렬·유한 입력값·루프 정합을 지킨다(커브 데이터 12.1·17.8절) |
| `ClampFloatTangent`·`ComputeCurveTangent`·`AutoCalcTangent` | `math::ClampCurveTangent`·내부 접선 계산 | 4절 1·3 |
| `FMath::CubicInterp`·`CubicInterpDerivative`·`CubicInterpSecondDerivative` | `math::CurveHermite*` | 기존 `math::CubicInterp`는 합산 순서가 달라 그대로 둔다 |
| `TQuat::Slerp`(커브 Linear 구간) | `CurveSlerp` | 같은 식, 전환 기준 0.9999 |
| `TQuat::CalcTangents`·`Squad`·`SlerpFullPath`·`Log`·`Exp` | `SquadControlPoint`·`Squad`·`SlerpFullPath`·`QuaternionLog`·`QuaternionExp` | 4절 4 |
| `TPolynomialRootSolver<double, 5>` | `math::FindPolynomialRootsInRange<5>` | 구간 안 근 + 접하는 근 후보 |
| `FInterpCurveTwoVectors` | 없음 | `TwoVectors` 미구현 |

## 3. 의미 결정

- **키 정렬·중복 입력값**: 키는 입력값 오름차순을 항상 유지한다. 새 키는 같은 입력값을 가진 기존 키들 **앞**에 들어간다(UE `AddPoint`의 `Points[i].InVal < InVal`). 같은 입력값이 여럿이면 그 입력에서의 값은 마지막(가장 먼저 넣은) 키이고 왼쪽 극한은 첫 키라 계단이 생긴다. 길이 0 구간은 평가에 쓰이지 않는다. `TryMovePoint`는 값·접선·모드를 유지하고 같은 규칙으로 새 위치를 정한다.
- **빈 커브**: `Eval`은 호출자가 준 `emptyValue`(기본은 0, quaternion은 항등)를 반환한다(UE `Default`). 최근접 탐색·키 구간은 NotFound다.
- **키 하나**: 모든 입력에서 그 키 값이다. 1차 미분은 앞쪽이 `leaveTangent`, 키 이후가 `arriveTangent`, 2차 미분은 0이다(UE와 같다). 루프를 켜면 그 키에서 자기 자신으로 가는 구간이 생긴다.
- **범위 밖(외삽)**: 값은 끝 키 값으로 고정한다(외삽하지 않음). 1차 미분은 첫 키 앞에서 첫 키의 `leaveTangent`, 마지막 키 이후(루프 아님)는 마지막 키의 `arriveTangent`를 반환한다 — 상수 외삽의 미분 0이 아니라 끝 방향을 주는 UE 규약이며 스플라인 방향 계산이 여기에 기댄다. 2차 미분은 0이다. ±Inf 입력도 같은 규칙이다.
- **루프 경계**: 루프는 입력을 감지 않는다. `[마지막 키, 마지막 키 + offset)`에 마지막 키 → 첫 키 값의 닫는 구간을 더할 뿐이다(첫 키의 `arriveTangent` 사용). `마지막 키 + offset` 이상은 첫 키 값, 첫 키 앞은 첫 키 값이다. 주기 평가는 호출자가 `math::Wrap`으로 입력을 감는다. offset은 마지막 키 기준이라 키를 더하면 루프 끝도 따라 움직인다. 루프 키가 마지막 키 이하이거나 키가 없으면 루프를 끈다(UE). 자동 접선은 첫 키의 이전 키를 `첫 키 − offset`의 마지막 키로 본다. `stationaryEndpoints`가 참이면 루프에서도 첫 키 접선은 0이다(UE) — 이음매에서 멈추지 않으려면 false를 준다.
- **접선 연속성**: CurveAuto·CurveAutoClamped는 도착·출발 접선을 같게 계산해 키에서 1차 미분이 연속이다. CurveUser는 같은 값을 쓰는 것이 약속이고(검사하지 않는다) CurveBreak만 꺾임을 허용한다. Linear 키의 자동 접선은 이웃 구간의 **기울기**(값 차 ÷ 입력 차)이고, Linear·Constant 뒤의 자동 곡선 키는 앞 키 접선을 복사하므로 Linear → 곡선 전환에서도 1차 미분이 연속이다(4절 1). 복사가 우선이라 Linear 뒤의 CurveAutoClamped 키는 clamp되지 않는다(UE와 같다).
- **quaternion 보간 방식**: Linear는 `CurveSlerp`(UE `TQuat::Slerp`와 같은 식 — 최단 경로, |내적| ≥ 0.9999이면 선형 가중치 후 정규화), Constant는 유지, 곡선 모드는 Shoemake SQUAD `Slerp(Slerp(q0, q1, t), Slerp(s0, s1, t), 2t(1 − t))`다. 구간마다 q1이 q0과 다른 반구면 q1·s1을 함께 뒤집어 최단 경로를 택하고, 안쪽 세 slerp는 다시 뒤집지 않는다. 그래서 뒤집힌 구간의 끝(t = 1, 루프 끝에서 비율이 1로 잘린 경우)은 −q1이다 — 같은 회전이고 성분 부호만 키에서 평가한 q1과 반대다(UE `Squad`도 같다). 자동 제어점은 `s = q·exp(−(1 − tension)·(log(q⁻¹q_prev) + log(q⁻¹q_next)) / 4)`(이웃은 q 반구로 맞춤), 고정 끝점·Linear·Constant 키는 `s = q`다. 결과는 정규화한다. 키·제어점은 단위 quaternion이 전제이고 검사하지 않는다. 키 간격이 고르지 않아도 제어점은 시간 비례 보정을 하지 않는다. 미분 평가·최근접 탐색은 제공하지 않는다(4절 4).
- **NaN 입력**: 값·미분이 모든 성분 NaN이다(4절 2). 비유한 키는 `TryAddPoint`·`TryMovePoint`·`TrySetLoopKey`가 InvalidArgument로 거부하고 상태를 바꾸지 않는다. 비유한 출력값은 받아서 결과로 전파한다. 이웃 키 차이가 float 범위를 넘는 키 배치는 지원하지 않는다.
- **정밀도**: 입력 키·구간 길이·보간 비율·tension은 float이고 Hermite 기저 계수도 float로 계산해 성분 정밀도로 넓힌다(UE `CubicInterp<T, float>`). 따라서 double 형식 커브도 값의 상대 정확도는 float 수준(약 1e-7)이다 — UE `FVector` 커브와 같다. 성분 산술·접선은 성분 정밀도다. 구간 안 비율은 반올림으로 1을 넘으면 1로 자른다(UE는 `checkf`).
- **나눗셈**: 구간 길이·시간 차(float)로 나눌 때 벡터·색은 역수를 성분 정밀도로 한 번 구해 곱하고(UE `TVector`·`TVector2`·`TVector4`의 `operator/`·`/=`가 `RScale = T(1) / Scale` 곱, `FLinearColor`는 `1.0f / Scalar` 곱), scalar(float·double)는 정확히 나눈다(UE float 커브와 같다).
- **CurveAutoClamped**: float·double·Vector2·Vector3는 성분별로 clamp한다. Vector4·LinearColor는 UE처럼 clamp를 무시하고 CurveAuto와 같게 계산한다(UE 일반 `ComputeCurveTangent`가 "Clamping not supported for non-float vector types"로 명시 — `InterpCurvePoint.h` 226). Vector2·Vector3를 clamp하는 것은 4절 3의 UE 결함 수정이다.
- **최근접 탐색**: 거리 있는 형식(`float`·`double`·`Vec2/3/4` 계열)만 지원한다. 구간마다 Constant는 두 끝 키 중 가까운 쪽(같으면 다음 키), Linear는 선분 사영, 곡선은 `(C(t) − p)·C'(t) = 0`의 5차식 근과 두 끝점을 비교한다(UE와 같은 순서·같은 엄격한 `<` 비교라 동률은 앞 후보). 계수와 근은 double로 계산한다. 값이 같은 두 키 사이의 길이 0 Linear 선분은 끝 키를 고른다(UE `Clamp(NaN)` 결과와 같음). 같은 입력값 키 사이(입력 길이 0) 구간도 UE처럼 그대로 계산하므로, 반환 입력값은 그 키지만 거리는 `Eval`이 실제로 내지 않는 두 키 값 사이 선분·곡선까지의 거리일 수 있다(UE와 같다).

## 4. UE 5.8과 다른 점과 근거

2026-11-16 사용자 결정: 아래 1·3·4는 UE 결함 수정이라 유지하고, 2는 Rocks 수학 규약에 따라 유지하며, 5는 끝점 정확성을 위한 수정이라 유지한다. 키 정밀도·Vector4·LinearColor clamp·벡터 나눗셈은 UE와 같게 되돌렸다.

1. **Linear 키 자동 접선 단위** — UE 결함 수정. UE `AutoSetTangents`는 Linear 키에 `LeaveTangent = NextPoint.OutVal − ThisPoint.OutVal`(값 차, `InterpCurve.h` 729–734)을 저장한다. 곡선 키 접선은 `ComputeCurveTangent`가 시간 차로 나눈 값(`InterpCurvePoint.h` 228–232)이고 `Eval`은 접선 × 구간 길이를 쓴다(`InterpCurve.h` 374). Linear 뒤 곡선 키는 이 값을 그대로 복사하므로(`InterpCurve.h` 724–727, 주석 "so there are no discontinuities") 의도와 달리 구간 길이가 1일 때만 1차 미분이 이어진다(입력이 점 번호인 스플라인에서는 가려진다). Rocks는 값 차를 `max(KindaSmallNumber, 입력 차)`(곡선 접선과 같은 보호)로 나눈 기울기를 저장한다. 구간 길이 1이면 UE와 비트가 같다. **UE에서 저장한 접선을 그대로 쓰면 평가 결과는 UE와 같다**(Linear 키 접선은 Linear 구간 평가에 쓰이지 않고 곡선 키 접선은 저장값 그대로 쓰인다). `AutoSetTangents`로 다시 계산할 때만 모양이 달라진다.
2. **NaN 입력** — Rocks 규약. UE는 NaN 입력에서 `Default`(대개 0)를 반환하고 `ENABLE_NAN_DIAGNOSTIC` 진단을 낸다(`InterpCurve.h` 321–328, 구간 비율의 `checkf`를 피하려는 의도된 방어). UE 결함은 아니지만 Rocks는 [Core 상세 명세](../Design/CoreDetailedSpec.ko.md) G01("잘못된 입력을 조용히 0으로 바꾸지 않는다")에 따라 모든 성분 NaN을 반환한다. 빈 커브는 NaN 입력이어도 `emptyValue`다.
3. **CurveAutoClamped의 Vector2·Vector3** — UE 결함 수정. UE는 `FVector`·`FVector2D`에 `ComputeClampableFloatVectorCurveTangent`를 쓰는데(`InterpCurvePoint.h` 304–333) 이 함수는 값을 `(float*)&Point`로 `sizeof(T) / 4`개의 float로 읽는다(237–283). UE5의 `FVector`·`FVector2D`는 double이라 double 비트를 float 쌍으로 읽어 접선이 무의미해지고, float 벡터(`FVector3f`·`FVector2f`)는 그 특수화가 없어 clamp되지 않는다. Rocks는 Vector2·Vector3(float·double)를 성분별로 각 정밀도에서 clamp한다(clamp가 의도된 형식의 의도된 계산). double scalar 커브도 float와 같은 의미로 clamp한다(UE에는 double scalar 커브 형식이 없다).
4. **quaternion** — UE 결함 수정. (a) UE 일반 `ComputeCurveTangent`가 `TQuat::CalcTangents` 결과(제어점)를 `OutTangent /= PrevToNextTimeDiff`로 나누고(`InterpCurvePoint.h` 228–232) `Eval`이 구간 길이를 다시 곱해(`InterpCurve.h` 374) 길이가 약 0.5인 제어점이 `SlerpFullPath_NotNormalized`의 `Acos(Clamp(dot))`에 들어가 각도가 틀어진다(`UnrealMath.cpp` 1258–1276). (b) 고정 끝점·Constant 키의 접선은 `T(ForceInit)` = 항등 quaternion이라(`InterpCurve.h` 697–698, 738–739) 끝 키가 항등이 아니면 항등 쪽으로 휜다. (c) `CalcTangents`의 계수는 −1/2이고 tension을 쓰지 않는다(`UnrealMath.cpp` 1302–1311). `Log`의 규약이 반각 × 축(1454–1480)이므로 표준 SQUAD 계수는 −1/4다. (d) Linear 키 접선은 quaternion 뺄셈 값이다. 미분은 제어점을 성분별 다항식으로 미분해 의미가 없다. Rocks는 3절의 표준 SQUAD를 쓰고 미분·최근접 탐색을 컴파일 시점에 막는다. UE 스플라인 회전 곡선과 모양이 같지 않다. Linear 구간의 slerp는 UE와 같은 `CurveSlerp`(전환 기준 0.9999)다.
5. **Linear 보간의 끝점** — UE 결함 수정. UE `FMath::Lerp(A, B, Alpha) = A + Alpha·(B − A)`(`UnrealMathUtility.h` 1123–1126)는 Alpha = 1에서 B를 정확히 내지 않는다(반올림으로 1ulp 오차 — 예: 키 −1000·1 사이 입력 1 바로 아래에서 비율이 1로 반올림된다). Rocks는 `math::Lerp`로 비율 0·1에서 끝 값을 그대로 내고, 비율 (0, 1) 안은 UE 식과 비트가 같다.
6. **API 형태**: 공개 `Points` 배열 대신 접근자(정렬 불변식), `Try*` 결과, 구간 번호 usize, quaternion 새 키의 기본 접선(2절 생성자 행).
7. **값 범위(`TryCalcBounds`)** — UE 결함 수정(2026-11-18, CV9). 구간 식은 UE `FindBounds`(`UnrealMath.cpp` 1512–1573)와 같은 식·연산 순서이고 Rocks 의미 참조와 비트가 같다. 다른 곳 둘: (a) 루프 닫는 구간의 끝 시간을 마지막 키 + offset으로 둔다 — UE `CalcBounds`(`InterpCurve.h` 746–772)는 첫 키의 입력값을 넘겨 구간 길이가 음수가 되고 극값을 놓친다. (b) 키 하나의 루프 커브도 자기 자신으로 가는 닫는 구간을 본다 — UE는 키 하나면 그 값만 내지만 `Eval`은 그 구간을 Hermite로 평가한다. 구간 수는 `Eval`과 같다(`SegmentCount`).

빌드 결정성: UE의 `FVector`(double) SIMD `CubicInterp` 특수화(`Vector.h` 2795)는 항을 `(s0·P0 + s1·T0) + (s2·T1 + s3·P1)`로 묶고 FMA를 쓸 수 있어 비트를 맞출 수 없다. Rocks는 일반 템플릿의 왼쪽부터 합산하는 순서로 고정한다(`-ffp-contract=off`). 이 차이는 의미 결정이 아니라 UE 쪽 구현 변형이다. clamp 접선의 Min/Max는 UE `FMath::Min/Max`와 같은 선택 규칙을 써 ±0 부호까지 같다.

## 5. 시험 계획

`Core/Source/Runtime/Core/Tests/MathInterpCurve*Tests.cpp`. 무작위 사례는 고정 seed라 매 실행이 같고, 타이밍에 기대는 시험은 없다.

- **경계(G09)**: 키 정렬·중복 입력값(삽입 위치, 정확한 입력의 값과 왼쪽 극한), 빈 커브(fallback·NotFound), 키 하나(값·미분·루프), 범위 밖(값 고정·끝 접선·2차 미분 0·±Inf), 루프 경계(닫는 구간 시작·끝·offset 이상·첫 키 앞·키 추가 뒤 이동·끄기 조건), 접선 연속성(자동 모드 키 양쪽 극한, Linear → 곡선 전환, CurveBreak의 꺾임), NaN·비유한 키 거부와 상태 보존, `TryMovePoint` 속성 유지, 할당 실패 주입(`TryAddPoint`·`TryReserve`·`TryCopyFrom` 실패 뒤 커브 불변).
- **UE 의미 대조 golden**: 시험 안에 UE 5.8 식을 그대로 옮긴 참조 구현(`AddPoint`·`SetLoopKey`·`GetPointIndexForInputValue`·`Eval`·`EvalDerivative`·`EvalSecondDerivative`·`AutoSetTangents`·`ComputeCurveTangent`·`ClampFloatTangent`)을 둔다. 키·비율은 float, 기저 계수는 float, 벡터·색 나눗셈은 역수 곱이고 4절 1·5를 켜고 끄는 선택지가 있다.
  - Rocks 의미(차이를 켠 참조): 모든 비회전 형식(float·double·Vec2/3/4 두 정밀도·LinearColor)에서 키 삽입 위치·접선·값·1·2차 미분이 **비트 단위로 같다**.
  - UE 그대로(차이를 끈 참조): 간격 1 정수 키에서 float·double·Vec2/3(float)·Vec2d·Vec4/4d·LinearColor가 비트 단위로 같다. 이것이 UE 비트 대조의 실제 범위다. Vec3d(UE `FVector`)는 SIMD 특수화 때문에 UE와의 비트 대조 대상이 아니다(일반 템플릿 식과만 대조). Vector2·Vector3·double의 CurveAutoClamped 키는 4절 3이라 이 대조에서 뺀다. quaternion은 UE 대조가 없다.
  - 간격이 1이 아니면 UE 그대로와 다르다는 것, 간격 2의 Linear → 곡선 이음매에서 UE 식의 미분이 끊기는 반례, UE가 저장한 접선을 그대로 쓰면 결과가 같다는 것을 확인한다.
- **double reference**: 독립 double 구현(멱급수 계수 Horner 평가, 비율은 계약대로 float)과 값·1·2차 미분을 상대 오차로 대조한다. float 기저 때문에 double 형식도 1e-6 수준이다. 별도로 중앙 차분과 1·2차 미분을 대조한다.
- **AutoClamped**: 단조 키에서 overshoot가 없음, 극점 키 접선 0, Vector3 clamp와 LinearColor·Vector4의 CurveAuto 동등.
- **quaternion**: 키 통과, 단위 길이, 반구 뒤집기, 독립 double SQUAD 참조와의 각거리, tension 1 = 제어점이 키, Linear = `CurveSlerp`.
- **값 범위**(`MathInterpCurveBoundsTests.cpp`): 일곱 형식 무작위 커브가 UE `FindBounds`·`CalcBounds` 참조(닫는 구간 수정을 켠 것)와 비트가 같고 UE 그대로와는 루프 커브에서만 다르다. 촘촘한 `Eval` 표본이 범위 안이다. 손 계산 벡터(곡선 두 키 극값 4/9, 루프 닫는 구간 −4/9, 키 하나 루프 7/3, NaN 값의 UE 선택 규칙).
- **리플렉션 미러**(`Curves/Tests/InterpCurveReflectionTests.cpp`): 다섯 형식 그래프·JSON 왕복과 다시 저장 byte 같음, 도구 이름·Atomic·enum 미러, decode·JSON import·대입 묶음·경로 쓰기의 hook 거부, 미선언 enum 값 거부, 생성 깊은 복사의 할당 실패 원자성.
- **최근접 탐색**: 조밀 표본 + 황금분할 double oracle보다 거리가 크지 않음, Constant·길이 0 Linear·키 하나·빈 커브·루프 구간, 다항식 근 찾기(중근·접하는 근·구간 끝 근 제외).
