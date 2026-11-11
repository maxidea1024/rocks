# Core 수학 — 플랫폼 최적화 계층·값 타입·색·난수

기준일: 2026-11-11. 이 문서는 **현재 구현된 범위와 계약**을 기록한다. 실행하지 않은 플랫폼과 미구현 기능은 각 절의 "미검증·후속"에 남긴다. 설계 정책 변경 이유는 [전체 구현 계획 §3.3](../Design/EngineImplementationPlan.ko.md)의 2026-11-11 개정 메모를 따른다.

## 1. 구조 — 플랫폼별 최적화 함수의 선택

UE 5.6 Core는 `FGenericPlatformMath` 위에 `TUnrealPlatformMathSSEBase<Base>`(x64 SSE 재정의)를 얹고 `FWindowsPlatformMath`·`FMacPlatformMath`를 `FPlatformMath`로 선택하며, 벡터 연산은 `VectorRegister`가 SSE·NEON·FPU 구현 중 하나를 고른다(고정 커밋 `cdda65cecacd9a1278020925c357bf4cc0b17e8c`의 `GenericPlatformMath.h`, `UnrealPlatformMathSSE.h`, `Windows/WindowsPlatformMath.h`, `HAL/PlatformMath.h`, `Math/VectorRegister.h`에서 확인). Rocks도 같은 계층을 쓰되 CPU 명령 계층을 기준으로 선택한다.

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
- 빌드는 모든 native 대상에 `-ffp-contract=off`(clang-cl `/clang:-ffp-contract=off`)를 준다(2026-11-11 결정). clang 기본값(`on`)은 FMA 명령이 항상 있는 ARM64에서만 `a*b+c`를 fused 연산으로 바꿔, 같은 소스의 결과와 판정이 플랫폼마다 달라진다. 리뷰에서 확인한 예: 정확히 특이한 행렬 `(0.1,0.3)/(0.2,0.6)`은 x64에서 행렬식 0으로 `TryInverse`가 실패하지만, FMA 축약 시 행렬식 3.3e-18로 성공해 1.8e17 크기 원소를 냈다. `aarch64-pc-windows-msvc` 대상 codegen에서 fused 명령이 `on` 28개 → `off` 0개로 바뀌는 것을 확인했다. ARM64 실행은 아직 하지 않았으므로 `ScalarSimdBitExact`는 실기기 확인 전까지 `false`로 둔다.

### UE와 의도적으로 다른 플랫폼 수학 규약

| 항목 | UE 5.6 (원본 확인) | Rocks |
|---|---|---|
| SSE `FloorToInt32` | `_mm_cvt_ss2si(F + F - 0.5f) >> 1` — 현재 반올림 모드 의존, 큰 값에서 범위 축소 | 절사 명령 + 정확한 보정. 범위 밖·NaN은 Fatal(`Try*`는 false) |
| `RoundToInt` | +무한대 방향(`F + F + 0.5` 기법) | 0에서 멀어지는 방향(C `round`, AArch64 `fcvtas`) |
| 범위 밖 정수 변환 | CPU의 integer indefinite 값이 그대로 반환될 수 있음 | 노출하지 않고 Fatal |
| `Min/Max` NaN | 구현별 | `(a<b)?a:b` 고정. SSE minps와 같고 NEON은 비교·선택으로 맞춘다 |

## 2. 값 타입과 계약

### 허용 오차 정책

전역 epsilon을 어떤 함수에도 묵시적으로 적용하지 않는다. UE의 `UE_SMALL_NUMBER`(1e-8)·`UE_KINDA_SMALL_NUMBER`(1e-4)와 같은 값의 `math::SmallNumber<T>`·`math::KindaSmallNumber<T>`, `math::MachineEpsilon<T>`을 제공하며 호출자가 `IsNearlyEqual(a, b, math::KindaSmallNumber<float>)`처럼 명시한다. 절대 오차는 값의 크기에 따라 의미가 달라지므로(float 1e6 m 근처 간격은 약 0.06 m) 크기 비례 비교에는 `IsNearlyEqualRelative(a, b, relative, absolute)`나 UE `IsNearlyEqualByULP`와 같은 규칙의 `IsNearlyEqualByUlps`를 쓴다. 함수별로 문서화한 기본 허용치(`Transform::IsValid` 1e-4, `TryFromMatrix` 직교성 1e-5, `ShapeUnitTolerance` 1e-5, OBB 평행 축 보정 등)는 각 헤더에 명시되어 있다.

공통: 오른손 좌표계, +X 전방·+Y 왼쪽·+Z 위, 미터·라디안·초. column vector `p' = M * p`, 합성 `world = parent * local`(UE의 row vector·`A*B=A 먼저`와 반대). 실패 가능한 연산은 `Result`(`Try*`)로 반환하고 입력을 바꾸지 않는다. 허용 오차는 호출자가 명시하며 전역 epsilon은 없다.

| 헤더 | 내용 | 주요 계약 |
|---|---|---|
| `MathUtility.h` | `math::` 상수·각도 변환, Clamp·Lerp·Sign, 분류, 초월 함수, 정수 변환, `UnwindRadians`·`FindDeltaAngleRadians`, `SmoothStep`·`CubicInterp`·`InterpTo`·`InterpConstantTo`·`GridSnap`·`Wrap` | Clamp는 NaN을 전파하고 min>max는 Fatal. Lerp는 t=0/1에서 끝점 정확. InterpTo/ConstantTo는 UE와 달리 숨은 "충분히 가까움" 기준이 없고, 도달 조건에서는 target을 정확히 반환. 입력 NaN은 target으로 바꾸지 않고 NaN으로 전파. `GridSnap`의 0.5 경계는 grid 부호와 무관하게 +∞ 방향(UE는 음수 grid에서 반대). `Wrap`은 반열린 구간 |
| `Vector2/3/4.h` | `Vec2/3/4`(float), `Vec2d/3d/4d`(double) | `TryNormalize(minimumLength)`는 길이 단위(제곱 아님) 기준, 가장 큰 성분으로 먼저 나눠 overflow/subnormal 입력도 정규화. 영·비유한은 InvalidArgument. Vector2/3 같은 계약(UE는 2D/3D 정규화 실패 동작이 다름) |
| `IntVector.h` | `IntVec2`, `IntVec3` | overflow·0 나눗셈·INT32_MIN/-1은 Fatal(UE는 정의되지 않은 동작) |
| `Matrix4.h` | `Mat4`, `Mat4d` | 열 우선 저장 `m[column*4+row]`, 이동은 `m[12..14]`, 기본값 단위 행렬. float 곱·변환은 `platform::simd`. `TryInverse`는 double Laplace 전개, 행렬식 0·비유한·결과가 T로 비유한이면 실패 |
| `MatrixFactory.h` | 원근(대칭·비대칭·무한 원거리), 직교(대칭·비대칭), `DepthRange{Standard, Reversed}`, oblique clip(UE ClipProjection), LookAt/LookTo, 축 회전·quaternion 회전·역회전, 이동·scale·TR·TRS, RotationAboutPoint, Mirror | Rocks view 공간(카메라 +X 전방, +Y 왼쪽, +Z 위) → NDC(x 오른쪽, y 위, z [0,1]). 기본 깊이는 Reversed(near→1). API별 y 반전은 RHI 경계에서 한 번 적용. oblique clip은 `DepthRange`를 기본값 없이 받아 두 깊이 규약을 모두 지원(Reversed는 near를 `w - z = a·c`로 바꾼다)하며, 행렬 깊이 계수 부호와 맞지 않는 플래그·far 평면 너머 평면은 InvalidArgument, 직교 투영은 Unsupported로 거부한다(조용히 뒤집힌 행렬을 내지 않음). LookTo는 up과 forward 사이 sin이 `LookToMinimumSine`(64·machine epsilon) 미만이면 실패(외적 정확 0 판정만으로는 평행 입력이 임의 방향으로 성공했다)하고, 기준 근처에서도 직교 행렬이 되도록 Y축을 Gram-Schmidt로 한 번 더 직교화한다. 원근 깊이 계수는 `n·(f/(f−n))`로 계산해 큰 far에서 overflow하지 않음 |
| `Quaternion.h` | `Quat`, `Quatd` | 성분 (x,y,z,w), 오른손 회전, `a*b`는 b 먼저. 회전 함수는 단위 전제. 행렬→quaternion은 대각합 분기 방식, w>=0 정규화. `Slerp`는 최단 경로이며 비유한 입력은 비유한 결과로 드러낸다. `AngularDistance`는 `4·atan2(|a−b|, |a+b|)`(같은 반구로 맞춘 뒤)로 계산해 q와 −q는 정확히 0이고 작은 각도 구분한다(2·acos 방식은 float 같은 회전에서 9.8e-4 rad를 냈다). 결과는 [0, π]로 자른다(NaN은 전파). `TryInverse`는 가장 큰 성분으로 먼저 나눠 제곱 길이 overflow/underflow 입력도 처리 |
| `Rotator.h` | `Rotator<T>`(UE FRotator 대응) | 라디안, yaw(+Z, 양수=왼쪽), pitch(-Y축, 양수=기수 위), roll(+X, 양수=왼쪽 위). 적용 roll→pitch→yaw. gimbal lock에서는 roll=0으로 합침. quaternion 분해 결과의 yaw·roll은 (−π, π](−π는 +π로). UE는 도·왼손이라 값 교환 불가 |
| `Transform.h` | TRS(이동 double, 회전 float quaternion, scale float) | 점 변환 scale→rotation→translation. 계산은 double이며 회전은 double로 확장 후 재정규화. 비균일 scale+회전 합성의 shear는 근사하지 않고 `Unsupported`. 0 scale 역변환은 InvalidArgument. 반사는 x scale 부호 |
| `Rect.h` | `Box2<T>`(UE FBox2D), `IntRect`(UE FIntRect) | Box2는 닫힌 집합(`Contains`=UE IsInsideOrOn, `ContainsStrictly`=IsInside), 무효 플래그 대신 `TryMake`/`TryFromPoints`. IntRect는 반열린 픽셀 구간, 넓이 int64 |
| `Interval.h` | `Interval<T>`(UE TInterval) | 닫힌 구간, 무효 상태 대신 `TryMake`. 교집합이 없으면 NotFound, NaN을 포함하면 무효로 드러남 |
| `Float16.h` | `Float16`(UE FFloat16) | RTNE, 크기 65520 이상은 ±Inf, NaN 유지. `FromFloatClamped`는 ±65504로 포화하되 NaN은 NaN(UE SetClamped는 NaN을 최댓값으로 만들 수 있음). 변환은 `PlatformMath::FloatToHalf/HalfToFloat`(Generic 정확 구현, ARM64는 fcvtn/fcvtl, x64는 F16C가 baseline이 아니어서 Generic) |
| `MathBatch.h` | `math::TransformPoints/TransformVectors` | 원소 수 불일치·부분 겹침은 Fatal, 정확한 in-place 허용, 결과는 원소별 Mat4 변환과 동일 |
| `Color.h` | `Color`(8-bit), `LinearColor`(float, 선형 sRGB 원색) | 아래 3절 |
| `RandomStream.h` | PCG32 결정적 난수 | 아래 4절 |

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

UE 5.6 원본과의 차이(원본 확인): `TSphere::Intersects/IsInside`와 `FCapsuleShape::IsInside`는 기본 1e-4 허용치를, `LineBoxIntersection`·`LineExtentBoxIntersection`은 `BOX_SIDE_THRESHOLD=0.1`을, `SegmentPlaneIntersection`은 ±1e-4 t 허용치를, `SegmentDistToSegment*`는 절대 1e-4 임계값을 쓴다. UE `TPlane(A,B,C)`는 일직선 점에서 영 법선 평면을 만들지만 Rocks `TryFromPoints`는 실패한다. UE `TRay` 기본 방향은 +Z, Rocks는 +X다. UE `FOrientedBox`에는 교차 함수가 없다.

검증: 13개 도형 테스트와 33개 판정 테스트(리뷰 회귀 5개 포함)가 고정 seed 무작위 사례를 독립 double oracle(축별 최소화·황금분할 탐색·조밀 표본·해석해·SDF 근 찾기·독립 15축 분리축)과 비교하고 모든 겹침 쌍의 대칭을 확인한다. 미구현: frustum, swept shape, 접촉 깊이·manifold, Segment–Capsule/OBB, OBB–Sphere/Box, Capsule–Box/Triangle.

## 3. 색

`Color`는 채널당 UNorm8, `LinearColor`는 선형 sRGB 원색·D65·straight alpha이며 HDR 값을 허용한다. sRGB 변환은 IEC 61966-2-1 식을 double로 정확히 계산하고 alpha에는 적용하지 않는다. 256개 byte 모두 왕복한다. 저장 양자화는 NaN·음수→0, 1 이상·+Inf→255(D3D float→UNORM 규칙과 같은 방향)이며, `GetClamped`는 NaN을 숨기지 않는다. 16진 문자열은 `#` 선택, 3/4/6/8자리를 받고 잘못된 입력에서는 false와 함께 출력값을 바꾸지 않는다. 휘도는 Rec.709 가중치(0.2126, 0.7152, 0.0722)다.

UE 5.6 `Color.h`/`Color.cpp`와의 차이(원본 확인): UE sRGB 디코딩은 표 기반으로 256개 중 25개가 정확 반올림보다 1 ulp 낮다. UE `ToFColorSRGB`는 표 보간 인코더로 [2^-13, 1) 구간에서 547,648개 float가 정확 반올림과 ±1 차이 난다. UE `QuantizeRound`는 float 연산으로 반올림 경계 바로 아래 128개 값이 1 크다. UE `ReinterpretAsLinear`는 `1/255` 곱으로 256개 중 126개가 1 ulp 크다. UE `FromHex`는 4자리를 지원하지 않고 잘못된 글자를 검증하지 않는다. UE `GetLuminance`는 설정에 따라 0.3/0.59/0.11 또는 작업 색 공간 계수를 쓴다. UE `FLinearColor(FColor)`는 암묵적으로 sRGB를 디코딩하지만 Rocks는 `FromSRGB`/`ReinterpretAsLinear` 중 하나를 명시해야 한다. 메모리 배치(UE little-endian BGRA, Rocks RGBA)는 포맷 계약이 아니며 외부 형식은 `ToPacked*`를 쓴다.

## 4. 결정적 난수

`RandomStream`은 PCG32(XSH RR, 64-bit LCG 상태)다. 같은 seed·stream이면 모든 플랫폼에서 같은 정수열을 내며 `NextFloat01`(24-bit)·`NextDouble01`(53-bit)도 bit 단위로 같다. 경계 있는 정수는 거부 표본 추출로 편향이 없고, `NextInt32InRange`는 int32 전체 구간을 overflow 없이 처리한다. `NextFloatInRange`는 [min, max)를 지키며 단순 식이 max로 반올림되는 경우도 보정한다. 인스턴스는 스레드 안전하지 않고 암호용이 아니다. 참조 출력은 `imneme/pcg-c`의 `check-pcg32.out`(seed 42, stream 54)과 대조했다. UE `FRandomStream`(32-bit LCG, 23-bit fraction, 범위 편향·전체 구간 overflow·잘못된 범위의 조용한 Min 반환)과 다르다.

## 5. 검증

- 단위 테스트: `Tests/Platform/PlatformMathTests.cpp`(Generic↔CPU 구현 bit 대조, 정수 변환 경계·무작위 30만 건, InvSqrtEstimate 오차, scalar↔SIMD 레지스터 bit 대조 5만 건×2), `Tests/Core/Math*Tests.cpp`(벡터·행렬·투영·quaternion·Transform·Rotator·보간·Rect·Float16, 독립 Gauss-Jordan 역행렬 oracle·Python 정수 행렬식·half 65,536개 전수 왕복과 이진 탐색 oracle 등), `ColorTests.cpp`, `RandomStreamTests.cpp`.
- 음성 대조(자식 프로세스): `math-invalid-0..7` — NaN·범위 밖 정수 변환, IntVector overflow, 일괄 변환 크기 불일치·부분 겹침, 행렬 인덱스, 비단위 축, Clamp 범위 위반이 각각 정상 종료하지 않고 지정 진단으로 끝나는지 확인한다.
- ARM64는 `--target=aarch64-pc-windows-msvc`로 NEON 경로의 **컴파일만** 확인했다. 실행 결과는 아래 미검증이다.
- 독립 리뷰(2026-11-11, 수학·도형 각 1개 subagent): 수학 쪽은 SSE↔Generic 정수 변환(double 1.6억 건, float 전체 2^32), InvSqrtEstimate(양의 정규 float 전수, ARM FRSQRTE 에뮬레이션 포함), half 변환(F16C 하드웨어와 전수 일치), scalar↔SSE 레지스터(300만 세트)에서 불일치 0을 확인했고, 위 표의 LookTo 평행 판정·Reversed oblique·AngularDistance·quaternion 역원·NaN 전파·FMA 축약·큰 far overflow를 찾아 수정했다. 도형 쪽은 비단위 OBB 회전, capsule 옆면 시작, 삼각형 끝점, 평면 Inf 거리, Box NaN 소실을 찾아 수정했고 수정 후 리뷰어 재현 프로그램 전부(OBB 무작위 70만 쌍 등)에서 실패 0이다. 재현 입력은 회귀 테스트로 옮겼다.
- 수정분 재검토(같은 날, 별도 subagent): 끝점 보정이 회전된 삼각형의 공면 선분을 t = 1 Hit로 만드는 회귀, Rotator 테스트가 수정 경로를 타지 않음, `AngularDistance`가 π를 1–4 ulp 넘음, 그리고 기존 문제(회전된 삼각형 위 시작점 누락, LookTo 기준 근처 비직교, oblique의 직교 투영·far 너머 평면 허용, `InterpConstantTo(Inf, Inf)`의 NaN)를 찾았다. 모두 수정하고 재현 입력을 테스트로 옮겼으며, 재검토 probe(삼각형 무작위 약 64만 건, oblique 무작위 frustum 20,000×8 구성, LookTo 120만 건 등)에서 실패 0이다.

## 6. 미검증·후속

- macOS(x64·ARM64) 빌드·실행, ARM64 NEON 실행 결과와 `ScalarSimdBitExact`의 실제 성립 여부, Apple libm의 초월 함수·`pow` 결과 차이.
- 성능 측정(scalar 대 SIMD, batch 크기·정렬)은 실행하지 않았다. SIMD 경로가 항상 빠르다고 주장하지 않는다.
- double 4-lane 레지스터(`VectorRegister4Double` 대응), AVX2/FMA 선택 kernel과 CPU 기능 dispatch, SoA batch kernel.
- UE 5.6 `Core/Public/Math` 99개 헤더 중 미구현: Float16Color·PackedVector(반정밀 색·패킹 벡터), BoxSphereBounds, DualQuat, Range·RangeSet(정수·열린 경계 범위), InterpCurve·CurveEd, Int128·BigInt·GuardedInt, NumericLimits, Halton·Sobol·LFSR, SHMath, ConvexHull2d·PolynomialRootSolver, GenericOctree, UnitConversion, TwoVectors, TransformCalculus, BasicMathExpressionEvaluator, IntVector4·UInt 변형, ColorList(이름 색 목록), ISPC(`*.isph`) 경로. 우선순위는 [Core 상세 명세](../Design/CoreDetailedSpec.ko.md) §3.4를 따른다.
