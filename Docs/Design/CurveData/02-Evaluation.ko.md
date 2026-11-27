# 커브 데이터 — 평가 의미·자동 접선

[목차](README.ko.md)

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
