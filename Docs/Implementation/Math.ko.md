# Core 수학 — 플랫폼 최적화 계층·값 타입·색·난수

기준일: 2026-11-11(좌표·단위 규약 UE 전환 1·2단계 2026-11-17 — 2절 "좌표·단위 규약"·"행렬·Transform·view 규약"). 이 문서는 **현재 구현된 범위와 계약**을 기록한다. 실행하지 않은 플랫폼과 미구현 기능은 각 절의 "미검증·후속"에 남긴다. 설계 정책 변경 이유는 [전체 구현 계획 §3.3](../Design/EngineImplementationPlan.ko.md)의 2026-11-11 개정 메모를 따른다.

## 1. 구조 — 플랫폼별 최적화 함수의 선택

UE 5.8 Core는 `FGenericPlatformMath` 위에 `TUnrealPlatformMathSSEBase<Base>`(x64 SSE 재정의)와 그 위의 `TUnrealPlatformMathSSE4Base<Base>`를 얹고 `FWindowsPlatformMath`(= `FMicrosoftPlatformMathBase`)·`FMacPlatformMath`를 `FPlatformMath`로 선택하며, 벡터 연산은 `VectorRegister`가 SSE·NEON·FPU 구현 중 하나를 고른다(UE 5.8(5.8.4, commit `f45729c0b6f0`) 대조 — `Engine/Source/Runtime/Core/Public/`의 `GenericPlatform/GenericPlatformMath.h`, `Math/UnrealPlatformMathSSE.h`, `Math/UnrealPlatformMathSSE4.h`, `Microsoft/MicrosoftPlatformMath.h`, `Windows/WindowsPlatformMath.h`, `Mac/MacPlatformMath.h`, `HAL/PlatformMath.h`, `Math/VectorRegister.h`에서 확인. 처음 확인은 UE 5.6 commit `cdda65ce`이며 이 계층은 5.8에서도 같다). Rocks도 같은 계층을 쓰되 CPU 명령 계층을 기준으로 선택한다.

```text
PlatformBase/Public/Platform/Math
  GenericPlatformMath        모든 CPU의 기준 scalar 구현(C 수학 라이브러리, bit 분류)
  └ ClangPlatformMath        Windows clang-cl·Apple Clang 공통 compiler builtin(bit 연산)
     ├ SsePlatformMath<Base>  x64 SSE2 재정의(sqrt, 정수 변환, rsqrt 근사)
     └ NeonPlatformMath<Base> ARM64 재정의(sqrt, fcvt* 변환, frsqrte 근사)
  VectorRegisterScalar       4-lane 기준 구현(항상 컴파일, 차등 oracle)
  VectorRegisterSse/Neon     같은 함수 목록의 SSE2/NEON 구현
Platform/PlatformMath.h      PlatformMath, simd:: 선택과 ScalarSimdBitExact
Core/Public/Core/Math        Vec·Quat·Mat4·Transform·Rotator·Rect·Color·RandomStream
```

- Windows x64·Intel macOS는 SSE2, Apple Silicon은 NEON 경로를 쓴다. 지원 toolchain이 모두 Clang이므로 현재 OS별 재정의는 없다. OS별 차이가 생기면 같은 상속 단계를 추가한다. 알 수 없는 CPU는 같은 계약의 Generic/scalar를 쓴다.
- x64 baseline은 SSE2만 사용한다. SSE4.1·AVX·FMA는 CPU·OS 기능 확인 없이 실행하지 않는다.
- 정확 함수(sqrt, 1/sqrt, 정수 변환, 사칙·min/max)는 Generic과 bit 단위로 같은 결과를 계약한다. 근사는 `InvSqrtEstimate`처럼 이름으로 구분하고 상대 오차 4e-7 이하(양의 정규 유한 float)만 약속한다. `rsqrtss`/`frsqrte` 근사 bit는 CPU 세대마다 다를 수 있다.
- 4-lane 레지스터는 합산 순서(`Dot3=(p0+p1)+p2`, `Dot4=(p0+p1)+(p2+p3)`, 행렬은 열 순서 누적)와 FMA 미사용을 계약으로 고정한다. x64에서는 scalar와 SIMD가 bit 단위로 같다(`ScalarSimdBitExact=true`). 단 NaN은 NaN 여부만 같고 부호·payload bit는 backend마다 다를 수 있다.
- 빌드는 모든 native 대상에 `-ffp-contract=off`(clang-cl `/clang:-ffp-contract=off`)를 준다(2026-11-11 결정). clang 기본값(`on`)은 FMA 명령이 항상 있는 ARM64에서만 `a*b+c`를 fused 연산으로 바꿔, 같은 소스의 결과와 판정이 플랫폼마다 달라진다. 리뷰에서 확인한 예: 정확히 특이한 행렬 `(0.1,0.3)/(0.2,0.6)`은 x64에서 행렬식 0으로 `TryInverse`가 실패하지만, FMA 축약 시 행렬식 3.3e-18로 성공해 1.8e17 크기 원소를 냈다. `aarch64-pc-windows-msvc` 대상 codegen에서 fused 명령이 `on` 28개 → `off` 0개로 바뀌는 것을 확인했다. ARM64 실행은 아직 하지 않았으므로 `ScalarSimdBitExact`는 실기기 확인 전까지 `false`로 둔다. fast-math 계열 인자는 Rocks.Build가 모듈 인자에서 거부하고, `Base/NumericLimits.h`(Types.h를 거쳐 거의 모든 번역 단위에 들어간다)가 `__FAST_MATH__`·`__FINITE_MATH_ONLY__`(0이 아님)를 빌드 오류로 막는다(이중 방어 — `IsFinite` 검사와 범위 밖 float 변환의 ±Inf가 이 전제에 기댄다).

### UE와 의도적으로 다른 플랫폼 수학 규약

| 항목 | UE 5.8 (원본 확인, `Math/UnrealPlatformMathSSE.h` 86–127 — 5.6과 같은 파일) | Rocks |
|---|---|---|
| SSE `FloorToInt32` | `_mm_cvt_ss2si(F + F - 0.5f) >> 1` — 현재 반올림 모드 의존, 큰 값에서 범위 축소 | 절사 명령 + 정확한 보정. 범위 밖·NaN은 Fatal(`Try*`는 false) |
| `RoundToInt` | +무한대 방향(`F + F + 0.5` 기법) | 0에서 멀어지는 방향(C `round`, AArch64 `fcvtas`) |
| 범위 밖 정수 변환 | CPU의 integer indefinite 값이 그대로 반환될 수 있음 | 노출하지 않고 Fatal |
| `Min/Max` NaN | 구현별 | `(a<b)?a:b` 고정. SSE minps와 같고 NEON은 비교·선택으로 맞춘다 |

## 2. 값 타입과 계약

### 허용 오차 정책

전역 epsilon을 어떤 함수에도 묵시적으로 적용하지 않는다. UE의 `UE_SMALL_NUMBER`(1e-8)·`UE_KINDA_SMALL_NUMBER`(1e-4)와 같은 값의 `math::SmallNumber<T>`·`math::KindaSmallNumber<T>`, `math::MachineEpsilon<T>`을 제공하며 호출자가 `IsNearlyEqual(a, b, math::KindaSmallNumber<float>)`처럼 명시한다. 절대 오차는 값의 크기에 따라 의미가 달라지므로(float 1e6 cm(10 km) 근처 간격은 0.0625 cm) 크기 비례 비교에는 `IsNearlyEqualRelative(a, b, relative, absolute)`나 UE `IsNearlyEqualByULP`와 같은 규칙의 `IsNearlyEqualByUlps`를 쓴다. 함수별로 문서화한 기본 허용치(`Transform::IsValid` 1e-4, `TryFromMatrix` 직교성 1e-5, `ShapeUnitTolerance` 1e-5, OBB 평행 축 보정 등)는 각 헤더에 명시되어 있다.

### 좌표·단위 규약(UE 5.8과 같다)

사용자 요구(2026-11-17)에 따라 공간 좌표계·회전 의미·단위를 UE 5.8(5.8.4, `f45729c0b6f0`)과 **같게** 바꾸고 있다. 이전 규약(오른손, +Y 왼쪽, 미터, Rotator 라디안·yaw·pitch·roll 순서·roll 부호 반대)을 위한 호환 계층은 두지 않는다. 1단계(축·방향 상수·외적 설명·Quaternion·Rotator·MathUtility 규약 문장과 golden)와 2단계(행렬·Transform 행 벡터 전환, view·투영 생성 함수, 아래 "행렬·Transform·view 규약")를 반영했고, 도형 앞면·Ray 기본 방향, 단위 변환 경계 함수, World 계약·문서 전수 정정, 렌더 경계(셰이더 `mul` 순서·packing)는 후속 단계다. 아래 근거 행은 UE 5.8.4 원본(`Engine/Source/Runtime/Core/`)이며 부호·식은 좌우수로 추측하지 않고 UE 식을 옮긴 probe로 계산해 확인했다(5절).

| 항목 | 규약(UE와 같음) | UE 근거 |
|---|---|---|
| 축·좌우수 | 왼손, +X 전방, +Y 오른쪽, +Z 위 | `Public/Math/Vector.h:84-100`, `Private/Math/UnrealMath.cpp:35-51` |
| 방향 상수 | `Forward` (1,0,0), `Backward` (−1,0,0), `Right` (0,1,0), `Left` (0,−1,0), `Up` (0,0,1), `Down` (0,0,−1) | 같은 곳 |
| 외적 | 표준식 `(a.y·b.z − a.z·b.y, …)`. `Cross(Forward, Right) = Up`, `Cross(Right, Forward) = Down`. 2D `Cross`는 z 성분(양수 = +X→+Y 쪽, 위에서 볼 때 시계 방향) | `Vector.h:1521-1530`, `Vector2D.h` `operator^` |
| 길이·시간 | cm, 초. 허용 오차 상수 값은 UE와 같고 의미도 cm다 | `Engine/Classes/GameFramework/WorldSettings.h:593-595`·`Engine/Private/WorldSettings.cpp:127`(WorldToMeters 100), `Core/Public/Math/UnrealMathUtility.h:130-131` |
| 각도 단위 | `Rotator`는 도(degree). 삼각함수·`Quaternion` 축-각·`AngularDistance`·`TryAngleBetween`은 라디안 | `Rotator.h:24`, `GenericPlatformMath.h:521-522`, `Quat.h:912-921` |
| Quaternion | 성분 (x,y,z,w), Hamilton 곱 `a*b`는 b 먼저(UE FQuat와 같음, UE FTransform·FMatrix 곱과 반대), `RotateVector` 식 같음. 양의 각은 왼손 회전: Q(+Z,+90°)·Forward = Right, Q(+X,+90°)·Right = Up, Q(+Y,+90°)·Up = Forward. 축 벡터 `GetAxisX/Y/Z` = `GetForwardVector/GetRightVector/GetUpVector`(이전 `GetForward/GetLeft/GetUp`은 없앴다) | `Quat.h:29-31`·`:49-58`·`:1236-1258`·`:1293-1330`, `Rotator.h:31`, `UnrealMathFPU.h:1456-1459` |
| Rotator 필드·단위 | `pitch, yaw, roll`(도), 생성자 `Rotator(pitch, yaw, roll)`. 리플렉션 미러 `RotatorBinding`·`RotatordBinding`도 같은 순서(이름 기반 필드 ID라 ID는 그대로, 저장된 값이 없어 Version 1 유지) | `Rotator.h:45-52`, `:101` |
| Rotator 의미 | 내재 yaw → pitch → roll = Q(+Z, yaw)·Q(+Y, −pitch)·Q(+X, −roll). pitch +는 기수 위(Forward→Up), yaw +는 위에서 볼 때 시계(Forward→Right), roll +는 +X로 볼 때 시계(Right→Down, Up→Right). 이전 Rocks roll은 대수 부호가 반대였다 | `Rotator.h:26-32`·`:45-52` |
| Rotator → Quaternion | UE 스칼라 단일식, 각 성분 `Fmod 360` 뒤 반각 sin·cos. float도 CRT sin·cos(UE float은 다항 근사 SinCos·VectorSinCos라 float 결과는 반올림보다 조금 더 다를 수 있다) | `UnrealMath.cpp:518-535`(double), `:448-466`(float) |
| Quaternion → Rotator | 입력을 정규화(Rocks — 실패는 `InvalidArgument`)한 뒤 UE 식: s = z·x − w·y, 임계 0.4999995 밖은 pitch ±90·roll 0·yaw = `NormalizeAxis(±2·atan2(x, w))`, 그 밖은 asin·atan2 식. float pitch는 `math::FastAsin`(UE와 같은 bit), double은 `Asin`. 결과 구간도 UE 그대로(yaw·roll은 −180이 나올 수 있다 — 이전 Rocks의 (−π, π] 보정은 없앴다) | `UnrealMath.cpp:640-677`(float, `:672` FastAsin), `:691-719`(double) |
| Rotator 정규화 | `ClampAxis` [0, 360), `NormalizeAxis` (−180, 180], `GetNormalized`·`GetDenormalized`, `Equals`(차를 NormalizeAxis로 감아 비교), `Vector()` = (cos p·cos y, cos p·sin y, sin p), `RotateVector`·`UnrotateVector`(quaternion 경유 — UE는 회전 행렬 경유, 반올림 수준 차이) | `Rotator.h:649-651`·`:688-716`, `UnrealMath.cpp:371-402` |
| 도 단위 도우미 | `math::UnwindDegrees`·`FindDeltaAngleDegrees`(UE와 같은 [−180, 180] 경계 — 범위 안 값과 −180은 그대로, 180 초과는 (−180, 180], −180 미만은 [−180, 180)), `math::FastAsin` | `UnrealMathUtility.h:610-634`(Wrap)·`:819-854`·`:898-913`·`:949-964` |

UE와 의도적으로 다른 점(규약이 아니라 안전성·정확도 정책 — 요구 3절 "규약과 충돌하지 않으면 유지"):
- `ClampAxis`: UE 본문은 −1e-20 같은 아주 작은 음수에서 반올림으로 360을 낸다. Rocks는 0을 돌려 UE가 선언한 [0, 360)(`Rotator.h:418-424`)을 지킨다. `NormalizeAxis` 결과는 둘 다 0으로 같다.
- `UnwindDegrees`: 경계 값은 UE ±360 반복 루프와 같다(2단계에서 −180을 180으로 바꾸던 차이를 없앴다). UE 루프는 Inf에서 끝나지 않고 큰 값에서 뺄셈 반올림이 쌓이며, Rocks는 Fmod(정확)로 한 번에 감고 비유한 입력을 NaN으로 만든다.
- `TryFromQuaternion`: UE `FQuat::Rotator`는 단위 입력을 가정하고 검사하지 않는다. Rocks는 정규화하고 영·비유한 입력을 실패로 돌려준다(단위 입력의 결과는 반올림 수준에서 같다).

### 행렬·Transform·view 규약(UE 5.8과 같다)

2단계(2026-11-17)에서 이전 column vector 규약(`p' = M * p`, `world = parent * local`, 열 우선 `m[column*4+row]`, Rocks view 공간 +X 전방·+Y 왼쪽·+Z 위, 원근 인자 수직 전체 FOV·aspect)을 UE 규약으로 바꿨다. 호환 계층은 없다. 메모리 배치는 원래 UE와 비트 단위로 같았으므로(이동 `m[12..14]`, probe) SIMD 커널 산술은 그대로이고 `A * B`가 커널을 `MatrixMultiply(B, A)`로 부른다 — GPU에 올리는 행렬 바이트도 그대로다.

| 항목 | 규약(UE와 같음) | UE 근거 |
|---|---|---|
| 벡터 적용 | 행 벡터 `p' = p · M`(`Vector4 * Matrix4`, `TransformVector4`). `TransformPoint`(w=1)·`TransformVector`(w=0) = UE TransformPosition·TransformVector | `Public/Math/Matrix.h:28-32`, `UnrealMathFPU.h:1619-1631` |
| 원소·배치 | `At(row, column)` = `M[row][column]`, 행 우선 `m[row*4+column]`, 행 0·1·2 = 축(`GetScaledAxis`), 마지막 행 = 이동(`GetTranslation` = UE GetOrigin). `FromRows`는 UE 평면 생성자와 같다 | `Matrix.h:49`, `Matrix.inl:27-34` |
| 곱·합성 | `A * B`는 A를 먼저 적용한다. 부모·자식은 `Local * ParentToWorld`. Quaternion 곱(b 먼저)과 반대. double 곱은 UE FPU와 같은 누적 순서 `((a0·b0 + a1·b1) + a2·b2) + a3·b3` | `Matrix.h:28-32`, `UnrealMathFPU.h:1538-1564` |
| Quaternion ↔ 행렬 | `ToMatrix`: `M[0][1] = 2(xy + wz)`, `M[1][0] = 2(xy − wz)` 등 UE와 원소가 비트 단위로 같다(2배 위치만 다름). `TryFromRotationMatrix`는 UE와 같은 색인(`x ∝ M[1][2] − M[2][1]`), 결과 w ≥ 0 | `QuatRotationTranslationMatrix.h:76-91`, `Quat.h:775-847` |
| Transform | 멤버·생성자 순서 rotation·translation·scale, `Transform`은 모든 성분 double(UE FTransform = TTransform<double>). 점 변환 scale → rotation → translation. `TryMultiply(a, b)` = UE `a * b`(a 먼저). 컴포넌트 월드 = `TryMultiply(relative, parentToWorld)`·행렬 `relative.ToMatrix() * parentMatrix`. `ToMatrix` = UE ToMatrixWithScale | `TransformVectorized.h:50-55`·`:75-79`·`:207`·`:303-345`·`:1675-1690`, `MathFwd.h:47`, `Engine/Private/Components/SceneComponent.cpp:732` |
| view 공간 | +X 오른쪽, +Y 위, +Z 전방(깊이). `MakeViewRotation(q)` = 역회전 × swizzle(행 (0,0,1,0)·(1,0,0,0)·(0,1,0,0)·(0,0,0,1)), `MakeWorldToView(origin, q)` = `T(−origin) * MakeViewRotation(q)`. `TryMakeLookFrom`: Z = 시선, X = Up × Z, Y = Z × X, 이동 행 −eye·축 | `Engine/Private/LocalPlayer.cpp:1241-1245`, `SceneView.cpp:684`·`:819-823`, `InverseRotationMatrix.h:35-51`, `Matrix.inl:957-976` |
| 원근 투영 | `TryMakePerspective(halfFovRadians, width, height, minZ[, maxZ], depth = Reversed)` = UE `FReversedZPerspectiveMatrix(HalfFOV, Width, Height, MinZ[, MaxZ])`(Standard는 `FPerspectiveMatrix`). HalfFOV는 **수평** 반각 라디안(UE 카메라는 수평 FOV°·π/360), `M[0][0] = 1/tan`, `M[1][1] = W/tan/H`, `M[2][3] = 1`, Reversed `M[2][2] = n/(n−f)`·`M[3][2] = f·n/(f−n)`, 무한(maxZ 생략·+Inf) `M[2][2] = 0`·`M[3][2] = n` | `PerspectiveMatrix.h:101-153`(주석 `:37`·`:49`의 "Y direction"은 식과 다르다), `Engine/Private/Camera/CameraStackTypes.cpp:212-216` |
| 직교 투영 | `TryMakeOrthographic(halfWidth, halfHeight, near, far, depth)` = UE `FReversedZOrthoMatrix(Width, Height, 1/(far−near), −near)`(UE Width·Height는 반폭·반높이), Standard는 `FOrthoMatrix` | `OrthoMatrix.h:53-72`, `CameraStackTypes.cpp:190-205` |
| oblique·mirror | oblique clip은 view 공간 평면(UE FPlane `Ax+By+Cz=D` 부호)으로 clip z 열을 바꾼다. Mirror는 3x3 `I − 2nnᵀ`, 이동 행 `2dn` | `ClipProjectionMatrix.h:44-68`, `MirrorMatrix.h:36-46` |
| 축 회전 생성 | UE처럼 축별 회전 행렬 함수가 없다(`MakeRotation(Quaternion::FromUnitAxisAngle(axis, radians))`) | `Core/Public/Math/` 행렬 타입 목록 |

UE와 의도적으로 다른 점(오류·정확도 정책, 요구 3절 "규약과 충돌하지 않으면 유지"): 생성 함수는 입력을 검사해 `Result`로 실패한다(UE는 0 나눗셈·NaN 행렬을 만든다). Transform 합성은 행렬 곱을 분해해 shear를 Unsupported로 알린다(UE `Multiply`는 shear를 버린다 — `TransformVectorized.cpp:295-345`). 0 scale 역변환은 InvalidArgument다(UE는 그 축 역수를 0으로 둔다 — `TransformVectorized.h:363-374`·`:1423`). LookFrom은 평행 up을 거부하고 X축을 한 번 더 직교화한다. Standard 무한 원근은 UE의 `1 − Z_PRECISION` 대신 정확한 극한(scale 1, offset −n)이다. Reversed 원근 깊이 offset은 overflow를 피하는 `n·(f/(f−n))`(UE `−f·n/(n−f)`와 반올림 수준 차이). 대량 경로(애니메이션 본·렌더 상대 좌표)는 `Transform3f`를 쓰는 계약이다(아래 `Transform.h` 행).

공통 계약: 실패 가능한 연산은 `Result`(`Try*`)로 반환하고 입력을 바꾸지 않는다. 허용 오차는 호출자가 명시하며 전역 epsilon은 없다.

| 헤더 | 내용 | 주요 계약 |
|---|---|---|
| `MathUtility.h` | `math::` 상수·각도 변환, Clamp·Lerp·Sign, 분류, 초월 함수(UE와 같은 근사 `FastAsin` 포함), 정수 변환, `UnwindRadians`·`FindDeltaAngleRadians`·`UnwindDegrees`·`FindDeltaAngleDegrees`, `SmoothStep`·`CubicInterp`·`InterpTo`·`InterpConstantTo`·`GridSnap`·`Wrap` | Clamp는 NaN을 전파하고 min>max는 Fatal. Lerp는 t=0/1에서 끝점 정확. InterpTo/ConstantTo는 UE와 달리 숨은 "충분히 가까움" 기준이 없고, 도달 조건에서는 target을 정확히 반환. 입력 NaN은 target으로 바꾸지 않고 NaN으로 전파. `GridSnap`의 0.5 경계는 grid 부호와 무관하게 +∞ 방향(UE는 음수 grid에서 반대). `Wrap`은 반열린 구간 |
| `Vector2/3/4.h` | `Vec2/3/4`(float), `Vec2d/3d/4d`(double) | `TryNormalize(minimumLength)`는 길이 단위(제곱 아님) 기준, 가장 큰 성분으로 먼저 나눠 overflow/subnormal 입력도 정규화. 영·비유한은 InvalidArgument. Vector2/3 같은 계약(UE는 2D/3D 정규화 실패 동작이 다름) |
| `IntVector.h` | `IntVec2`, `IntVec3` | overflow·0 나눗셈·INT32_MIN/-1은 Fatal(UE는 정의되지 않은 동작) |
| `Matrix4.h` | `Mat4`, `Mat4d` | UE FMatrix와 같은 행 우선 저장 `m[row*4+column]`·행 벡터 `p · M`·`A*B`는 A 먼저, 이동은 마지막 행 `m[12..14]`, 기본값 단위 행렬. float↔double 변환은 explicit 생성자(`Mat4d(Mat4)` 정확, `Mat4(Mat4d)` 원소별 최근접 반올림 — Vector·Quaternion과 같다). float 곱·변환은 `platform::simd`. `TryInverse`는 double Laplace 전개, 행렬식 0·비유한·결과가 T로 비유한이면 실패 |
| `MatrixFactory.h` | UE 인자의 원근(`TryMakePerspective(halfFov, width, height, minZ[, maxZ], depth)`·비대칭), 직교(반폭·반높이·비대칭), `DepthRange{Standard, Reversed}`, oblique clip(UE ClipProjection), view(`MakeViewRotation`·`MakeWorldToView`·`TryMakeLookFrom`·`TryMakeLookAt`), quaternion 회전·역회전, 이동·scale·회전+이동·`MakeScaleRotationTranslation`, RotationAboutPoint, Mirror | 위 "행렬·Transform·view 규약" 표. 기본 깊이는 Reversed(near→1). API별 y 반전은 RHI 경계에서 한 번 적용. oblique clip은 `DepthRange`를 기본값 없이 받아 두 깊이 규약을 모두 지원(Reversed는 near를 `w - z = a·c`로 바꾼다)하며 새 z 열(`M[*][2]`)만 바꾼다. 행렬 깊이 계수 부호와 맞지 않는 플래그·far 평면 너머 평면은 InvalidArgument, 직교 투영은 Unsupported로 거부한다(조용히 뒤집힌 행렬을 내지 않음). LookFrom은 up과 lookDirection 사이 sin이 `LookFromMinimumSine`(64·machine epsilon) 미만이면 실패(외적 정확 0 판정만으로는 평행 입력이 임의 방향으로 성공했다)하고, 기준 근처에서도 직교 행렬이 되도록 X축을 Gram-Schmidt로 한 번 더 직교화한다. 원근 Reversed 깊이 offset은 `n·(f/(f−n))`로 계산해 큰 far에서 overflow하지 않음(UE `−f·n/(n−f)`와 반올림 수준 차이). UE에 없는 축별 회전 함수(이전 `MakeRotationX/Y/Z`)는 없앴다 |
| `Quaternion.h` | `Quat`, `Quatd` | 성분 (x,y,z,w), UE와 같은 왼손 양의 각, `a*b`는 b 먼저, 축 벡터 `GetAxisX/Y/Z`·`GetForwardVector/GetRightVector/GetUpVector`. 회전 함수는 단위 전제. `ToMatrix`는 UE FQuatRotationMatrix와 색인·원소가 같다(행이 축). 행렬→quaternion은 UE와 같은 색인의 대각합 분기 방식, w>=0 정규화. `Slerp`는 최단 경로이며 비유한 입력은 비유한 결과로 드러낸다. `AngularDistance`는 `4·atan2(|a−b|, |a+b|)`(같은 반구로 맞춘 뒤)로 계산해 q와 −q는 정확히 0이고 작은 각도 구분한다(2·acos 방식은 float 같은 회전에서 9.8e-4 rad를 냈다). 결과는 [0, π]로 자른다(NaN은 전파). `TryInverse`는 가장 큰 성분으로 먼저 나눠 제곱 길이 overflow/underflow 입력도 처리 |
| `Rotator.h` | `Rotator<T>`(UE FRotator와 같은 규약·식) | 도(degree), 필드·생성자 `pitch, yaw, roll`. 의미·변환식·정규화는 위 "좌표·단위 규약" 표. 특이점(임계 0.4999995)에서는 UE와 같이 pitch ±90·roll 0으로 합친다. UE FRotator 값을 그대로 옮겨 쓸 수 있다 |
| `Transform.h` | `Transform`(UE FTransform — 회전·이동·scale 모두 double), `Transform3<T>`(`Transform3f`·`Transform3d` — UE FTransform3f·3d) | 멤버·생성자 순서 rotation·translation·scale(UE). 점 변환 scale→rotation→translation, 합성 `TryMultiply(a, b)` = UE `a * b`(a 먼저, 부모·자식은 `TryMultiply(local, parent)`). `Transform`의 연산은 같은 값의 `Transform3d`와 같은 구현이라 결과가 비트 단위로 같다(차이는 리플렉션 미러 형식뿐 — `Transform` 멤버 단위, `Transform3d` Atomic). `Transform3<T>`는 변환·`ToMatrix`·합성 행렬 곱을 T로 계산하고(회전 정규화는 제곱 길이 [2^-60, 2^60]에서 sqrt 한 번), `TryFromMatrix` 분해는 세 타입이 같은 double 구현(`detail::TryDecomposeAffine` — 행 0·1·2 축 길이, 최대 성분이 [2^-500, 2^500] 밖일 때만 나눠 구해 overflow·underflow 없음)을 쓴 뒤 각 정밀도로 반올림한다. `TryInverse`는 역 affine을 double로 직접 만들어 분해하므로(행렬식 없음) 극단 scale에서도 된다. 비균일 scale+회전 합성의 shear는 근사하지 않고 `Unsupported`(UE는 버린다). 0 scale 역변환은 InvalidArgument(UE는 그 축 역수 0, Transform3f는 역수가 float로 Inf인 |s| < 약 2.94e-39도). 역변환 점·벡터는 결과가 범위를 넘으면 비유한 성분으로 성공한다. 반사는 x scale 부호. 대량 경로(애니메이션 본, 렌더 카메라 상대 좌표·인스턴스)는 `Transform3f`를 쓴다(큰 월드 절대 좌표는 `Transform`으로 두고 원점을 뺀 상대 값만 좁힌다). 세 타입 사이 변환은 `To…`(정확)·`RoundTo…`(성분별 최근접 짝수 반올림, 결과 유한성은 `IsValid`로 확인)뿐이다(리플렉션 B9 3.2절) |
| `Rect.h` | `Box2<T>`(UE FBox2D), `IntRect`(UE FIntRect) | Box2는 닫힌 집합(`Contains`=UE IsInsideOrOn, `ContainsStrictly`=IsInside), 무효 플래그 대신 `TryMake`/`TryFromPoints`. IntRect는 반열린 픽셀 구간, 넓이 int64 |
| `Interval.h` | `Interval<T>`(UE TInterval) | 닫힌 구간, 무효 상태 대신 `TryMake`. 교집합이 없으면 NotFound, NaN을 포함하면 무효로 드러남 |
| `Float16.h` | `Float16`(UE FFloat16) | RTNE, 크기 65520 이상은 ±Inf, NaN 유지. `FromFloatClamped`는 ±65504로 포화하되 NaN은 NaN(UE SetClamped는 NaN을 최댓값으로 만들 수 있음). 변환은 `PlatformMath::FloatToHalf/HalfToFloat`(Generic 정확 구현, ARM64는 fcvtn/fcvtl, x64는 F16C가 baseline이 아니어서 Generic) |
| `MathBatch.h` | `math::TransformPoints/TransformVectors` | 행 벡터 `p · M`. 원소 수 불일치·부분 겹침은 Fatal, 정확한 in-place 허용, 결과는 원소별 Mat4 변환과 동일 |
| `Color.h` | `Color`(8-bit), `LinearColor`(float, 선형 sRGB 원색) | 아래 3절 |
| `RandomStream.h` | PCG32 결정적 난수 | 아래 4절 |
| `InterpCurve.h`·`InterpCurvePoint.h`·`InterpCurveMath.h`·`PolynomialRoots.h` | `InterpCurve<T>`(UE FInterpCurve) 키 커브, Hermite·SQUAD 식, 다항식 실근 | [키 커브](InterpCurve.ko.md) — 키 정렬·루프·외삽·접선·quaternion 보간의 의미 결정과 UE 차이 |

## 2.1 기본 도형과 충돌 판정

`Shapes.h`는 `Plane`, `Ray`, `Segment`, `Sphere`, `Box`(AABB), `Triangle`, `Capsule`, `OrientedBox`를 float/double 템플릿으로 제공하고, `Intersection.h`는 순수 함수 판정을 제공한다.

| 분류 | 함수 | 지원 조합 |
|---|---|---|
| 광선 | `Raycast(ray, shape, maxDistance)` → `RayHit`(결과·거리·점·법선·`startedInside`) | Plane, Sphere, Box, Triangle(양면 기본, `FrontOnly` 선택), Capsule, OrientedBox |
| 선분 | `SegmentCast(segment, shape)` → `SegmentHit`(t ∈ [0,1]) | Plane, Sphere, Box, Triangle |
| 겹침 | `Overlap(a, b)` → `QueryResult{Miss, Hit, InvalidInput}` | Sphere–Sphere, Sphere–Box, Box–Box, Box–Plane, Sphere–Plane, Capsule–Sphere, Capsule–Capsule, Sphere–Triangle, OrientedBox–OrientedBox(분리축 15개), 양방향 |
| 평면 분류 | `ClassifyPlaneSide` → `Front/Back/Intersecting/InvalidInput` | Box, Sphere |
| 포함·최근접 | `ContainsPoint`, `TryClosestPoint`, `TryDistanceSquared`, `TrySignedDistance`, `TryClosestPoints(segment, segment)` | Box·Sphere·Capsule·OrientedBox 포함, Plane·Segment·Box·Triangle·OrientedBox 최근접 |

계약: 모든 도형은 닫힌 집합이며 경계 접촉은 Hit이다. `OrientedBox`는 `IsValid`가 회전 길이 오차(`ShapeUnitTolerance`)를 허용하므로 축·좌표 변환과 모든 판정이 `UnitRotation()`(정규화한 회전)을 쓴다. 평면 광선 판정은 거의 평행해 거리가 overflow하면 Miss다. 광선·선분의 시작점이 삼각형 위(평면 위 정확 판정)면 방향과 무관하게 거리 0 Hit(공면 포함, FrontOnly는 앞면 진입일 때만)이고, 시작점이 삼각형 밖인 공면 광선·선분은 Miss다. 선분 끝점이 삼각형 위면 계산된 t가 1을 넘어도 t = 1 Hit(들어오는 면은 시작점 쪽), capsule 옆면 위에서 출발한 광선은 거리 0 Hit다. OBB 판정은 매 호출 회전을 정규화하므로 `ContainsPoint(OBB)`가 약 9 ns → 16 ns(x64 /O2)로 느려졌다(정확성 우선). 시작 접촉 거리는 +0이다. `maxDistance`는 T로 변환된 뒤 검사한다(float 도형에 double을 넘기면 −1e-50은 −0으로, float 범위 밖은 +Inf로 바뀜). 겹침 판정에 숨은 epsilon이 없고, 유일한 허용치는 OrientedBox 분리축의 평행 축 보정 `ObbParallelEpsilon`(float 1e-6, double 1e-12, 인자로 변경 가능)이다. 비유한 값·음수 반지름·뒤집힌 box·비단위 방향/회전·NaN 또는 음수 `maxDistance`는 Miss/Hit로 숨기지 않고 `InvalidInput`이다. 광선 방향은 단위 벡터가 전제(`Ray::TryMake`가 정규화)이며 거리는 실제 거리다. 고체 안에서 시작한 광선은 거리 0, `startedInside=true`, 법선 0 벡터다. 퇴화 삼각형은 mesh에 흔하므로 광선·선분에서 Miss로 처리하고 최근접·구 겹침에서는 선분·점으로 다룬다. 공유 변을 가진 삼각형 집합의 watertight 판정은 보장하지 않는다.

UE 5.8 원본과의 차이(원본 확인 — `Math/Sphere.h`·`CapsuleShape.h`·`Box.h`·`Plane.h`·`Ray.h`·`OrientedBox.h`·`Private/Math/UnrealMath.cpp`, 5.6과 견줘 인라인 지시어·로그 매크로만 바뀌었다): `TSphere::Intersects/IsInside`와 `FCapsuleShape::IsInside`는 기본 1e-4 허용치를, `LineBoxIntersection`·`LineExtentBoxIntersection`은 `BOX_SIDE_THRESHOLD=0.1`을, `SegmentPlaneIntersection`은 ±1e-4 t 허용치를, `SegmentDistToSegment*`는 절대 1e-4 임계값을 쓴다. UE `TPlane(A,B,C)`는 일직선 점에서 영 법선 평면을 만들지만 Rocks `TryFromPoints`는 실패한다. UE `TRay` 기본 방향은 +Z, Rocks는 +X다. UE `FOrientedBox`에는 교차 함수가 없다.

검증: 13개 도형 테스트와 33개 판정 테스트(리뷰 회귀 5개 포함)가 고정 seed 무작위 사례를 독립 double oracle(축별 최소화·황금분할 탐색·조밀 표본·해석해·SDF 근 찾기·독립 15축 분리축)과 비교하고 모든 겹침 쌍의 대칭을 확인한다. 미구현: frustum, swept shape, 접촉 깊이·manifold, Segment–Capsule/OBB, OBB–Sphere/Box, Capsule–Box/Triangle.

## 3. 색

`Color`는 채널당 UNorm8, `LinearColor`는 선형 sRGB 원색·D65·straight alpha이며 HDR 값을 허용한다. sRGB 변환은 IEC 61966-2-1 식을 double로 정확히 계산하고 alpha에는 적용하지 않는다. 256개 byte 모두 왕복한다. 저장 양자화는 NaN·음수→0, 1 이상·+Inf→255(D3D float→UNORM 규칙과 같은 방향)이며, `GetClamped`는 NaN을 숨기지 않는다. 16진 문자열은 `#` 선택, 3/4/6/8자리를 받고 잘못된 입력에서는 false와 함께 출력값을 바꾸지 않는다. 휘도는 Rec.709 가중치(0.2126, 0.7152, 0.0722)다.

UE 5.8 `Color.h`/`Color.cpp`와의 차이(원본 확인 — 아래 개수는 5.6에서 잰 값이며 5.8에서 같은 코드다. 5.8은 sRGB 디코딩 표를 `Color.cpp`에서 `Color.h`의 `constexpr` 배열로 옮겼으나 값 256개가 같고, `ToFColorSRGB`·`QuantizeRound`·`ReinterpretAsLinear`·`FromHex`·`GetLuminance`·`FLinearColor(FColor)` 본문은 인라인 지시어만 바뀌었다): UE sRGB 디코딩은 표 기반으로 256개 중 25개가 정확 반올림보다 1 ulp 낮다. UE `ToFColorSRGB`는 표 보간 인코더로 [2^-13, 1) 구간에서 547,648개 float가 정확 반올림과 ±1 차이 난다. UE `QuantizeRound`는 float 연산으로 반올림 경계 바로 아래 128개 값이 1 크다. UE `ReinterpretAsLinear`는 `1/255` 곱으로 256개 중 126개가 1 ulp 크다. UE `FromHex`는 4자리를 지원하지 않고 잘못된 글자를 검증하지 않는다. UE `GetLuminance`는 설정에 따라 0.3/0.59/0.11 또는 작업 색 공간 계수를 쓴다. UE `FLinearColor(FColor)`는 암묵적으로 sRGB를 디코딩하지만 Rocks는 `FromSRGB`/`ReinterpretAsLinear` 중 하나를 명시해야 한다. 메모리 배치(UE little-endian BGRA, Rocks RGBA)는 포맷 계약이 아니며 외부 형식은 `ToPacked*`를 쓴다.

## 4. 결정적 난수

`RandomStream`은 PCG32(XSH RR, 64-bit LCG 상태)다. 같은 seed·stream이면 모든 플랫폼에서 같은 정수열을 내며 `NextFloat01`(24-bit)·`NextDouble01`(53-bit)도 bit 단위로 같다. 경계 있는 정수는 거부 표본 추출로 편향이 없고, `NextInt32InRange`는 int32 전체 구간을 overflow 없이 처리한다. `NextFloatInRange`는 [min, max)를 지키며 단순 식이 max로 반올림되는 경우도 보정한다. 인스턴스는 스레드 안전하지 않고 암호용이 아니다. 참조 출력은 `imneme/pcg-c`의 `check-pcg32.out`(seed 42, stream 54)과 대조했다. UE `FRandomStream`(32-bit LCG, 23-bit fraction, 범위 편향·전체 구간 overflow·잘못된 범위의 조용한 Min 반환)과 다르다.

## 5. 검증

- 단위 테스트: `Core/Source/Runtime/PlatformBase/Tests/PlatformMathTests.cpp`(Generic↔CPU 구현 bit 대조, 정수 변환 경계·무작위 30만 건, InvSqrtEstimate 오차, scalar↔SIMD 레지스터 bit 대조 5만 건×2), `Core/Source/Runtime/Core/Tests/Math*Tests.cpp`(벡터·행렬·투영·quaternion·Transform·Transform3f/3d(`MathTransform3Tests.cpp` — 변환 경계값의 최근접 짝수 반올림, 세 타입의 분해 판정 일치)·Rotator·보간·Rect·Float16, 독립 Gauss-Jordan 역행렬 oracle·Python 정수 행렬식·half 65,536개 전수 왕복과 이진 탐색 oracle 등), `ColorTests.cpp`, `RandomStreamTests.cpp`.
- 좌표·회전 golden(2026-11-17): UE 5.8.4 원본 식(`UnrealMath.cpp` Rotator↔Quat·Vector, `Rotator.h` ClampAxis·NormalizeAxis, `Quat.h` 축-각·RotateVector, `Vector.h` 외적, `UnrealMathUtility.h` FastAsin·UnwindDegrees)을 그대로 옮긴 독립 C++ probe(x64 Windows clang-cl, `-ffp-contract=off`, double·float)의 출력을 시험 기대값으로 썼다. 정확 일치(`==`)는 결과가 정확히 표현되는 항목(방향 상수, 축 외적, `atan2(±0, −1)`의 ±180, `Fmod 360` 감김의 bit 동일, 특이점의 ±90·0, ClampAxis·NormalizeAxis·UnwindDegrees, float `FastAsin` bit와 그것을 쓰는 float pitch 1건)에만 쓰고, 축 ±90° 회전 결과(UE에서도 2.3e-16 반올림이 남는다)는 1e-15, quaternion 성분은 2 ulp, 임의 각은 1e-15(성분)·1e-12(도), 특이점 근처는 1e-9(도)로 비교한다. 왕복 시험만으로 갈음하지 않고 축별 ±90°, 임의 각 2건, 특수 quaternion 7건, 특이점 임계 안팎 5건, roll 부호를 구체 값으로 고정했다(`MathVectorTests.cpp`·`MathRotatorInterpTests.cpp`·`MathQuaternionTransformTests.cpp`). 무작위 시험은 UE 단일식 = 축-각 분해 Q(+Z, yaw)·Q(+Y, −pitch)·Q(+X, −roll)(1e-15)와 왕복(1e-9도)을 확인한다.
- 행렬·Transform·view golden(2026-11-17, 2단계): UE 5.8.4 원본 식(`UnrealMathFPU.h` VectorMatrixMultiply·VectorTransformVector, `QuatRotationTranslationMatrix.h`, `Quat.h` TQuat(FMatrix), `TransformVectorized.cpp` Multiply·`TransformVectorized.h` TransformPosition·ToMatrixWithScale, `InverseRotationMatrix.h`·`LocalPlayer.cpp` swizzle·`SceneView.cpp` WorldToView, `PerspectiveMatrix.h`, `OrthoMatrix.h`, `Matrix.inl` LookFrom, `MirrorMatrix.h`)을 옮긴 독립 probe의 출력을 `MathMatrixTransformGoldenTests.cpp`에 고정했다. 정확 일치(`==`)는 같은 식·같은 연산 순서인 항목 — 곱 순서((T·S)·(1,0,0) = 22, (S·T) = 12), 정수 행렬 A·B와 행 벡터 곱(double·float SIMD), quaternion → 행렬 2건, 무한·Standard 원근 전체 원소와 Reversed의 1/tan·W/tan/H·n/(n−f), 직교 Reversed·Standard 전체 원소, 회전 없는 view 축(world (100,0,0) → view (0,0,100) 등) — 에만 쓴다. 허용 오차: 행렬 → quaternion 1e-15(대각합 분기·나눗셈 방식 차이, Rocks w ≥ 0), Transform 합성(Child · Parent 원점 (0, 200, 50) 등 — 행렬 곱 분해 경로) 회전 1e-14·이동 1e-13·점 1e-12, view(quaternion 경유) 회전 원소 1e-15·이동 행 1e-12, Reversed 깊이 offset 2e-15, LookFrom(재직교화) 축 1e-15·이동 1e-13, Mirror(법선 정규화) 3x3 1e-15·이동 행 1e-14. 왕복 시험으로 갈음하지 않았다. 기존 행렬·Transform·oblique·LookFrom 시험은 새 규약으로 고쳤고(허용 오차를 넓힌 곳 없음, double Transform 합성은 1e-4 → 1e-9로 좁힘) `Transform`과 `Transform3d`의 결과가 비트 단위로 같음을 확인한다.
- 마이크로벤치마크(측정용, 시험 아님): `Programs/MathBench`(Release, 단일 스레드, 변환 4096개 × 64회, 5회·9회 중 최고값). 2026-11-17 Windows x64(clang-cl Release) 두 번 잰 범위, 연산당 ns(double `Transform` / float `Transform3f`): `TryMultiply` 68–70 / 62–65, `TryInverse` 57–59 / 54–57, `TransformPoint` 3.8–3.9 / 3.9–4.0, `ToMatrix` 두 개의 곱 4.4–4.6 / 4.5–5.2. 원소 하나씩의 지연 위주 경로에서는 double이 float보다 최대 약 13% 느린 정도이고(분해·정규화의 나눗셈·sqrt가 지배), float의 이점은 메모리(40 대 80 byte)·SIMD 묶음 처리에서 나온다 — 대량 경로를 `Transform3f`로 두는 계약의 근거다. ARM64·macOS는 재지 않았다.
- 키 커브: `MathInterpCurveTests.cpp`(G09 경계)·`MathInterpCurveGoldenTests.cpp`(UE 5.8 식 비트 대조, 독립 double reference, 조밀 표본 최근접 oracle) — 상세는 [키 커브](InterpCurve.ko.md) 5절.
- 음성 대조(자식 프로세스): `math-invalid-0..7` — NaN·범위 밖 정수 변환, IntVector overflow, 일괄 변환 크기 불일치·부분 겹침, 행렬 인덱스, 비단위 축, Clamp 범위 위반이 각각 정상 종료하지 않고 지정 진단으로 끝나는지 확인한다.
- ARM64는 `--target=aarch64-pc-windows-msvc`로 NEON 경로의 **컴파일만** 확인했다. 실행 결과는 아래 미검증이다.
- 독립 리뷰(2026-11-11, 수학·도형 각 1개 subagent): 수학 쪽은 SSE↔Generic 정수 변환(double 1.6억 건, float 전체 2^32), InvSqrtEstimate(양의 정규 float 전수, ARM FRSQRTE 에뮬레이션 포함), half 변환(F16C 하드웨어와 전수 일치), scalar↔SSE 레지스터(300만 세트)에서 불일치 0을 확인했고, 위 표의 LookTo 평행 판정·Reversed oblique·AngularDistance·quaternion 역원·NaN 전파·FMA 축약·큰 far overflow를 찾아 수정했다. 도형 쪽은 비단위 OBB 회전, capsule 옆면 시작, 삼각형 끝점, 평면 Inf 거리, Box NaN 소실을 찾아 수정했고 수정 후 리뷰어 재현 프로그램 전부(OBB 무작위 70만 쌍 등)에서 실패 0이다. 재현 입력은 회귀 테스트로 옮겼다.
- 수정분 재검토(같은 날, 별도 subagent): 끝점 보정이 회전된 삼각형의 공면 선분을 t = 1 Hit로 만드는 회귀, Rotator 테스트가 수정 경로를 타지 않음, `AngularDistance`가 π를 1–4 ulp 넘음, 그리고 기존 문제(회전된 삼각형 위 시작점 누락, LookTo 기준 근처 비직교, oblique의 직교 투영·far 너머 평면 허용, `InterpConstantTo(Inf, Inf)`의 NaN)를 찾았다. 모두 수정하고 재현 입력을 테스트로 옮겼으며, 재검토 probe(삼각형 무작위 약 64만 건, oblique 무작위 frustum 20,000×8 구성, LookTo 120만 건 등)에서 실패 0이다.

## 6. 미검증·후속

- macOS(x64·ARM64) 빌드·실행, ARM64 NEON 실행 결과와 `ScalarSimdBitExact`의 실제 성립 여부, Apple libm의 초월 함수·`pow` 결과 차이.
- 성능 측정(scalar 대 SIMD, batch 크기·정렬)은 실행하지 않았다. SIMD 경로가 항상 빠르다고 주장하지 않는다. float·double Transform 비용만 `Programs/MathBench`로 쟀다(5절).
- double 4-lane 레지스터(`VectorRegister4Double` 대응), AVX2/FMA 선택 kernel과 CPU 기능 dispatch, SoA batch kernel.
- UE 5.8 `Core/Public/Math` 101개 파일(`.h`·`.inl`·`.isph`, 5.6은 99개 — 5.8에서 `SpringMath.h`가 새로 생기고 `StaticSpatialIndex.h`가 Engine WorldPartition에서 옮겨 왔다) 중 미구현: SpringMath(스프링 감쇠)·StaticSpatialIndex(정적 공간 색인, 5.8 추가분), Float16Color·PackedVector(반정밀 색·패킹 벡터), BoxSphereBounds, DualQuat, Range·RangeSet(정수·열린 경계 범위), CurveEd·InterpCurve의 `CalcBounds`(InterpCurve 본체는 2026-11-16 구현 — [키 커브](InterpCurve.ko.md)), Int128·BigInt·GuardedInt, NumericLimits, Halton·Sobol·LFSR, SHMath, ConvexHull2d·PolynomialRootSolver(구간 실근은 `PolynomialRoots.h`로 구현, 일반 solver 형태는 미구현), GenericOctree, UnitConversion, TwoVectors, TransformCalculus, BasicMathExpressionEvaluator, IntVector4·UInt 변형, ColorList(이름 색 목록), ISPC(`*.isph`) 경로. 우선순위는 [Core 상세 명세](../Design/CoreDetailedSpec.ko.md) §3.4를 따른다.
