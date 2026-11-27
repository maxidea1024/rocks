# 커브 에셋 — 2. 커브 에셋 클래스(스칼라·벡터·색)

[목차](README.ko.md)

## 2. 커브 에셋 클래스(스칼라·벡터·색)

이 장의 클래스와 값 struct는 모두 `CurveAssets` 모듈에 있다.

- 필드는 영속 `ROCKS_PROPERTY`다.
- EditorOnly 필드는 없다. UE의 `AssetImportData`가 하던 일은 Rocks에서 AssetDatabase ImportRecord가 맡으므로 에셋 객체에 두지 않는다.
- 스레드는 객체 Registry의 소유 스레드 전용이다. 값을 읽는 평가 함수는 const이고 할당하지 않는다.

### 2.1 `CurveAsset`(추상 기반 — UE `UCurveBase`)

- `class CurveAsset : public Asset`는 추상 클래스라 직접 만들지 않는다.
- UE `FCurveOwnerInterface`의 채널 목록을 virtual로 둔다. 리플렉션 클래스는 virtual을 가질 수 있다(선례 `SoundSource.h`).
  - `virtual usize ChannelCount() const noexcept = 0;`
  - `virtual const TangentCurve& Channel(usize index) const noexcept = 0;`: 범위 밖은 assert다. 호출자가 `ChannelCount`로 확인한다.
  - `virtual TangentCurve& MutableChannel(usize index) noexcept = 0;`
  - `virtual StringView ChannelName(usize index) const noexcept = 0;`: UE `GetCurves`의 이름이다.
    - 스칼라는 "Value"다. UE 비const판은 객체 이름을 쓰지만 Rocks 에셋 객체에는 이름이 없다(◇ 7.2절 11).
    - 벡터는 "X"·"Y"·"Z", 색은 "R"·"G"·"B"·"A"다.
- `Result<Interval<float>> TryGetTimeRange() const`: 키가 있는 채널들의 시간 범위 합집합이다.
  - 키가 있는 채널이 없으면 NotFound다.
  - UE(`CurveBase.cpp:21-77`)는 키 없는 채널의 (0, 0)도 합쳐 범위에 0이 섞이고, 채널이 0개면 out을 건드리지 않는다(★ 7.2절 1).
- `Result<Interval<float>> TryGetValueRange() const`: 같은 규칙의 값 범위 합집합이다(각 채널의 `TryGetValueRange` — CurveData 7.3절).
  - 채널 하나라도 InvalidArgument(비유한 범위)면 그 오류다.
- `void ResetCurve() noexcept`: 모든 채널의 키를 비운다.
  - 기본값·외삽은 그대로 둔다(UE `ResetCurve` → `FRichCurve::Reset` — `CurveBase.cpp:117-128`, `RichCurve.cpp:673-677`).
- 변경 통지(UE `OnCurveChanged`·`OnUpdateCurve`)는 편집기 전용이라 지금 두지 않는다. atlas는 변경을 구독하는 대신 쿠킹 때 다시 굽는다(4.3절).

### 2.2 `FloatCurveAsset`(UE `UCurveFloat` — `CurveFloat.h:29-55`)

- 필드:
  - `ROCKS_PROPERTY() TangentCurve curve;`
  - `ROCKS_PROPERTY() bool eventCurve = false;`
    - UE `bIsEventCurve`이고 타임라인 이벤트 트랙용 표지다.
    - 이벤트 트랙은 키 시간만 쓰고 값은 버린다(`RE/Private/BlueprintGeneratedClass.cpp:1780-1783`). 표지의 뜻은 타임라인(CM5)이 쓴다.
- `float Evaluate(float time) const noexcept` = `curve.Evaluate(time)`(fallback 0)다.
  - UE `GetFloatValue`(`CurveFloat.cpp:45-48`)와 같다. 키가 없으면 defaultValue, 그것도 없으면 0이다.
- 채널은 하나("Value")다.
- 같음 비교는 리플렉션 값 비교다. UE `operator==`(표지와 커브 — `:74-77`)와 같다.

### 2.3 `VectorCurveAsset`(UE `UCurveVector` — `CurveVector.h:29-30`)

- 필드: `ROCKS_PROPERTY() TangentCurve x; … y; … z;`
  - UE `FloatCurves[3]`이다. 리플렉션 고정 배열 대신 이름 있는 필드 셋으로 둔다.
  - 단위·좌표는 UE와 같다(CurveData 13절).
- `Vec3d Evaluate(float time) const noexcept`: 성분마다 `Evaluate(time)`(fallback 0)이다.
  - UE `GetVectorValue`(`CurveVector.cpp:64-71`)와 같다.
  - 반환은 Rocks 벡터 기본형 `Vec3d`이고, 값은 float 평가 결과를 넓힌 것이다.
- 채널은 셋("X"·"Y"·"Z")이다.

### 2.4 `ColorCurveAsset`(UE `UCurveLinearColor` — `CurveLinearColor.h:28`)

- 필드는 `TangentCurve r, g, b, a`(UE `FloatCurves[4]`)와 색 조정 일곱이다.
- 조정 필드의 편집 범위는 UE `ClampMin`·`ClampMax`를 옮긴 편집 메타일 뿐이다. 평가는 값을 자르지 않고 그대로 쓴다(UE와 같다).

| 필드 | 기본 | 편집 범위 | UE |
|---|---|---|---|
| `adjustHue` | 0 | 0–359 | `AdjustHue`(`CurveLinearColor.h:84-85`) |
| `adjustSaturation` | 1 | 0–1 | `AdjustSaturation`(`:87-88`) |
| `adjustBrightness` | 1 | 0–1 | `AdjustBrightness`(`:90-91`) |
| `adjustBrightnessCurve` | 1 | 0–1 | `AdjustBrightnessCurve`(`:93-94`) |
| `adjustVibrance` | 0 | 0–1 | `AdjustVibrance`(`:96-97`) |
| `adjustMinAlpha` | 0 | 0–1 | `AdjustMinAlpha`(`:99-100`) |
| `adjustMaxAlpha` | 1 | 0–1 | `AdjustMaxAlpha`(`:102-103`) |

#### 2.4.1 원값 평가

`LinearColor EvaluateUnadjusted(float time) const`(UE `GetUnadjustedLinearColorValue` — `CurveLinearColor.cpp:145-166`):

- R·G·B는 각 채널의 `Evaluate(time)`(fallback 0)이다.
- A는 a에 키가 없으면 1이고, 이때 a의 defaultValue는 보지 않는다(UE와 같다). 키가 있으면 `Evaluate(time)`이다.
- 값을 자르지 않는다(범위 밖 허용).

#### 2.4.2 조정 평가

`LinearColor Evaluate(float time) const`(UE `GetLinearColorValue`)와 `LinearColor EvaluateClamped(float time) const`(UE `GetClampedLinearColorValue`)는 둘 다 아래 조정을 한다. `clampAlways`가 앞의 것은 거짓, 뒤의 것은 참이다. UE `GetAdjustedColorValue`(`:81-142`)이고, 원본은 TextureCompressor `AdjustColorsOld`다.

1. O = `EvaluateUnadjusted(time)`.
2. clampValue = clampAlways이거나 max(O.r, O.g, O.b) < 1 + `ColorAdjustTolerance`.
3. HSV = UE 식 RGB → HSV(2.4.3절). r = 색상(도), g = 채도, b = 명도다. sRGB 변환은 없다.
4. V ×= adjustBrightness.
5. |adjustBrightnessCurve − 1| > `ColorAdjustTolerance`이고 adjustBrightnessCurve ≠ 0이면 V = pow(V, adjustBrightnessCurve)(float).
6. |adjustVibrance| > `ColorAdjustTolerance`이면 S += clamp(adjustVibrance, 0, 1) × `VibranceScale` × pow(1 − S, `VibranceSaturationExponent`).
7. S ×= adjustSaturation.
8. H += adjustHue.
9. clampValue면 V = clamp(V, 0, 1).
10. RGB = UE 식 HSV → RGB(2.4.3절).
11. A = lerp(adjustMinAlpha, adjustMaxAlpha, O.a).

- 이름 있는 상수(`CurveAssets/CurveAssetConstants.h`):
  - `ColorAdjustTolerance` = 1e-4(UE `UE_KINDA_SMALL_NUMBER`)
  - `VibranceScale` = 0.5
  - `VibranceSaturationExponent` = 5
- 식·연산 순서·float 정밀도는 UE와 같게 두고 비트 대조로 시험한다(7.4절).
- UE의 `IsNearlyEqual`·`IsNearlyZero`는 `≤ tolerance`일 때 "가깝다"이므로 위 5·6단계의 조건은 `>`다.

#### 2.4.3 HSV 변환(UE float판)

Core `LinearColor::LinearRGBToHSV`·`HSVToLinearRGB`는 UE와 같지 않다. 그래서 조정 평가는 UE 식을 `CurveAssets` 안에 float로 따로 둔다(공개 `CurveAssets/ColorCurveEvaluate.h` — `EvaluateUnadjustedColorCurve`·`EvaluateAdjustedColorCurve`, 조정 값은 `ColorCurveAdjustments.h`). 비교 도우미(Max·Min·Max3·Min3·Clamp)는 UE 5.8.4 형태다(`GenericPlatformMath.h:944-972` `(B < A) ? A : B`·`(A < B) ? A : B`, `UnrealMathUtility.h:551-561`·`592-595` `Max(Min(X, Hi), Lo)` — 같은 값이면 오른쪽이라 명도 −0이 +0으로 잘리는 것까지 같다). Core는 바꾸지 않는다. 그 함수의 다른 사용처에 영향이 없게 하려는 것이다. Core와 UE의 차이는 Core 담당에 알린다.

| | Core | UE |
|---|---|---|
| 계산 정밀도 | double(`Core/Private/Math/Color.cpp:51-56`) | float(`Runtime/Core/Private/Math/Color.cpp:383-443`) |
| RGB 최댓값 ≤ 0 | V = max(음수) | (0, 0, 0, A)(`:389-392`) |
| HSV → RGB의 S | 자르지 않음 | [0, 1]로 자름(`:416`) |

- RGB → HSV(UE `:383-406`):
  - max ≤ 0이면 (0, 0, 0, A)다.
  - H는 [0, 360) 도, S = (max − min) / max, V = max다.
- HSV → RGB(UE `:411-443`):
  - H는 360으로 감고 음수를 보정한다.
  - S는 [0, 1]로 자르고 V는 자르지 않는다.

#### 2.4.4 그 밖

- `bool HasAlphaKeys() const`: a에 키가 있는지다(UE `HasAnyAlphaKeys` — `.h:57`).
- 채널은 넷("R"·"G"·"B"·"A")이다.
- **새 에셋의 기본 키**:
  - UE: `WITH_EDITOR` 생성자에서 RGB가 비었으면 t = 0 검정(A = 1)과 t = 1 흰색(A = 1) 키를 넣는다(`:50-68`).
    - 쿠킹 런타임의 기본 객체에는 이 키가 없다. 그래서 기본 키 그대로인 커브가 차이 저장에서 빠지면 런타임에 빈 커브가 될 수 있다.
    - UE에서 실제로 그런지는 확인하지 못했다(7.5절).
  - Rocks: 생성자에 키를 넣지 않는다. 빌드 종류와 무관하게 클래스 기본값이 같아야 A5 차이 저장이 한 기준을 쓴다(CAD4).
  - 새 에셋에 검정→흰색 키를 넣는 일은 편집기(Studio)의 새로 만들기가 한다. 그러면 결과 에셋은 그 키를 값으로 가진다.
  - 명령행 `asset-new`는 빈 커브를 만든다.

#### 2.4.5 쿠킹용 결정적 색 평가(CA7 — CurveData D22, CAD12)

- `LinearColor EvaluateDeterministic(float time) const`·`EvaluateUnadjustedDeterministic(float time) const`: `Evaluate`·`EvaluateUnadjusted`와 같은 식이고 수학만 결정적이다. 채널은 `TangentCurve::EvaluateDeterministic`, 조정의 거듭제곱 둘(명도 지수·활기 — UE `FMath::Pow`)은 C `powf` 의미를 지키는 결정적 거듭제곱이다: 밑 ≥ 0은 `(float)deterministic::Pow`, 유한 음수 밑은 지수가 정수면 |밑|의 거듭제곱에 홀수 지수의 부호, 아니면 NaN이고, −Inf 밑·±0 밑의 부호와 무한대도 C(C11 F.10.4.4)와 같다. HSV의 `fmod`는 정확한 연산이라 그대로 둔다. 결과 성분의 NaN은 고정 quiet NaN이다.
- 공용 식에 결정적 판 `EvaluateAdjustedColorCurveDeterministic`·`EvaluateUnadjustedColorCurveDeterministic`과 `DeterministicPowf`(C `powf` 의미)를 둔다(`ColorCurveEvaluate.h`). 두 판은 cpp 안의 수학 정책 템플릿 하나를 공유하므로 식이 갈라지지 않는다. 런타임 `Evaluate`의 비트(UE 대조 — CA1 시험)는 바뀌지 않는다.
- 쓰는 곳: atlas 굽기(4.3절), 머티리얼 ColorRamp 상수(MaterialSystem 6.13절 MS2f4 — 머티리얼 세션이 바꾼다).

### 2.5 값 struct(평탄 커브 — 3.1절 사본의 대상)

참조 없는 값 struct다. 복사·배열 원소·Map 값·Optional 내부에 모두 쓸 수 있다.

| struct | 필드 | 평가 |
|---|---|---|
| (스칼라) | `TangentCurve` 그대로 | `Evaluate` |
| `VectorCurveValue` | `TangentCurve x, y, z` | `Vec3d Evaluate(float)` — 2.3절과 같다 |
| `ColorCurveValue` | `TangentCurve r, g, b, a`, 조정 일곱, `bool adjusted` | `adjusted`면 2.4.2절, 아니면 2.4.1절 |

- 색 평가는 에셋·값·원천이 모두 `ColorCurveEvaluate.h`의 자유 함수를 쓴다(같은 식). 벡터 평가는 성분마다 `Evaluate`라 각자 둔다.
- 평탄 값의 복사(`TryCopyFrom`)와 원천의 평탄화는 대상 채널마다 그 채널의 allocator로 사본을 만들고, 모두 성공한 뒤에 옮긴다(대상의 수명 계약을 바꾸지 않는다).
