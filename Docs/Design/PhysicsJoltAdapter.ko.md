# Rocks 물리 어댑터 경계: Jolt 좌표·단위 변환 계약

기준일: 2026-11-23, 개정 2(초안). 개정 2: 근거에 컴포넌트 쪽 물리 요구(컴포넌트 도입 CR16) 링크를 더했다. 개정 1: 4절에 에셋 단순 충돌 형상 대응(메시 에셋 스펙 MR5)과 8절 J20·J21을 더했다.

이 문서는 Rocks 물리 모듈이 Jolt Physics와 주고받는 값의 좌표·방향·단위 변환 계약을 정한다. 물리 모듈과 Jolt 연동은 구현되지 않았고, 이 문서도 코드나 의존성을 들이지 않는다. 상태 표기는 **설계**(이 문서가 정한 계약, 코드 없음)와 **검증 항목**(확인하지 못해 착수 전에 확인할 것, 8절의 J·U 번호)이다. Jolt 쪽 사실(좌우수, 기본 up 축, 단위, API 이름과 의미)은 이 개정에서 공개 문서·소스로 확인하지 않았으므로 모두 전제로 적고 검증 항목에 묶는다. 전제가 확인 결과와 다르면 이 문서를 고친 뒤 구현한다.

근거:

- 사용자 결정(2026-11-17): 물리 middleware 기준을 PhysX 5에서 Jolt로 되돌린다([전체 구현 계획](EngineImplementationPlan.ko.md) 개정 1.12, 16절).
- 사용자 요구(2026-11-17): 공간 좌표계·방향·회전 의미·단위는 UE 5.8과 같다. 물리 엔진 미구현이면 Jolt를 들이지 않고 연동 설계만 한다. cm↔m 변환은 물리 어댑터 경계에서 하고, 극성·축성 벡터·quaternion·winding을 구분하며, 확인하지 못한 단위는 검증 항목으로 둔다.
- Rocks 규약 구현: [수학 구현](../Implementation/Math.ko.md) 2절("좌표·단위 규약", "단위 경계 변환", "행렬·Transform·view 규약")과 2.1절(기하 앞면).
- UE 5.8.4(commit `f45729c0b6f0`, 로컬 클론) 원본. 경로는 `Engine/Source/Runtime/` 기준이다.
- 이 경계를 쓰는 컴포넌트 쪽 요구(body·충돌 채널·응답·프로필, overlap·hit 이벤트, sweep 이동, scene query, simulate physics, ragdoll physics asset)와 충돌 형상 컴포넌트(Box·Sphere·Capsule·Cylinder)는 [컴포넌트 도입](ComponentCatalog/04-Prerequisites.ko.md) CR16·3.10절·CM3에 있다. 물리 없는 영역 판정(`VolumeShape`, CR17)은 이 경계를 지나지 않는다.

## 1. 양쪽 규약

| 항목 | Rocks(구현됨, UE 5.8과 같음) | Jolt(전제 — 검증 항목) |
|---|---|---|
| 축·좌우수 | 왼손, +X 전방, +Y 오른쪽, +Z 위 | 오른손(J1), 기본 up 축 +Y(J2) |
| 길이 | cm | m(J3) |
| 질량·시간 | kg, 초 | kg, 초(J3) |
| 각도 | `Rotator` 도. quaternion 축-각·삼각함수·각속도는 라디안 | 라디안(J3) |
| 수 표현 | 월드 위치 double, 그 밖 float·double | float, 선택 build에서 double 위치(33.11절 S82 확인 내용, 이 개정 미재확인 — J10) |

Rocks의 외적, Hamilton 곱, quaternion 벡터 회전, quaternion → 행렬 식은 오른손 수학 라이브러리의 표준식과 같다(수학 구현 2절 — UE 식을 옮긴 probe와 대조). 다른 것은 축에 붙인 이름(+Y를 오른쪽으로 읽음)과 그에 따라 양의 회전을 왼손 회전으로 읽는 해석뿐이다. 이 사실이 2절의 축 사상을 정한다.

## 2. 축 사상 — 행렬식 +1만 쓴다

**설계.** Rocks 벡터 r을 Jolt 벡터 j로 바꾸는 축 사상은 고정 직교 행렬 A(j = A·r)이며 det A = +1이다. A는 어댑터의 상수 하나로 두고 모든 경계 함수가 그것만 쓴다.

- 기본안(J2가 +Y일 때): 축 순환 j = (r.y, r.z, r.x), 역 r = (j.z, j.x, j.y). Rocks `Forward` (1,0,0) → Jolt (0,0,1), `Right` (0,1,0) → (1,0,0), `Up` (0,0,1) → (0,1,0). 순환 치환이라 det A = +1이고, 성분을 옮기기만 하므로 방향 변환 자체는 정확하다(반올림 없음).
- 근거: Rocks `Up`이 Jolt 기본 up과 겹쳐, 기본 중력 방향·캡슐과 실린더 축·heightfield 평면처럼 up에 묶인 Jolt 기본값(J2·J6·J7)을 형상 회전 없이 쓴다.
- J2 결과가 +Y가 아니거나 Jolt가 up을 전역 설정으로만 받으면 A를 다시 고른다. 선택 기준은 det A = +1과 A·(0,0,1) = Jolt up 두 가지다. 항등 사상도 기준을 만족하면 쓸 수 있으며, 그때는 up에 묶인 형상을 고정 회전으로 감싼다.
- 행렬식 −1 사상(거울, 예: Y 성분 부호만 뒤집기)은 쓰지 않는다.

거울이 필요 없는 이유: 좌우수는 수치 계산에 들어가지 않는 해석이다. 두 라이브러리가 같은 수식을 쓰므로 det A = +1이면 (A·a) × (A·b) = A·(a × b)이고, Jolt가 계산한 운동을 A⁻¹로 되돌리면 Rocks 좌표에서 같은 식으로 계산한 운동과 같다(반올림 차이 제외). 오른손으로 읽는 쪽과 왼손으로 읽는 쪽은 같은 수치를 서로 거울상으로 해석할 뿐이며, 역학 법칙을 같은 외적 식으로 계산하는 한 어느 해석에서도 운동은 올바르다. "좌우수가 다르니 축 하나를 뒤집는다"는 처리는 오히려 거울 사상이 되어 아래 표의 오른쪽 열처럼 축성 벡터·quaternion·winding·heightfield를 모두 따로 보정해야 한다.

| 대상 | det A = +1 사상(채택) | 거울 사상 M, det M = −1(채택하지 않음, 참고) |
|---|---|---|
| 극성 벡터(위치·변위·선속도·선가속도·중력·힘·선형 충격량·법선) | j = A·r | j = M·r |
| 축성 벡터(각속도·토크·각운동량·각충격량 — 외적으로 정의) | j = A·r(극성과 같다) | j = −M·r |
| quaternion (v, w) | (A·v, w), 회전 행렬은 A·R·Aᵀ | (−M·v, w) |
| 관성 텐서 I | A·I·Aᵀ(주축 값이면 성분 재배치) | M·I·Mᵀ |
| 삼각형 정점 순서 | 그대로(법선이 A로 대응) | 두 정점을 바꿔야 앞면이 유지 |
| heightfield 표본 격자 | 축 재배치(전치 등, 4절) | 거울상 격자 |

## 3. 값 종류별 변환

공간 단위는 cm·kg·초다. 크기 변환 계수는 Core `Core/Math/UnitConversion.h`의 함수로만 적용하고 숫자로 흩어 쓰지 않는다(수학 구현 2절 "단위 경계 변환"). 계수는 물리량의 길이 차원으로 정한다(일괄 100배가 아니다). 방향 변환은 2절의 A다.

| 값 | Rocks 단위 | Jolt 단위(J3) | 방향 | 크기 변환 |
|---|---|---|---|---|
| 위치(월드·body·접촉점·질의 원점) | cm(월드는 double) | m | A·(r − o), o는 지역 원점(6절) | `CentimetersToMeters` |
| 길이 스칼라(반지름·반높이·convex radius·침투 깊이·여유 거리) | cm | m | — | `CentimetersToMeters` |
| box 반폭(축별 크기) | cm | m | A의 성분 재배치(부호 없음) | `CentimetersToMeters` |
| 선속도 | cm/s | m/s | A | `CentimetersPerSecondToMetersPerSecond` |
| 선가속도·중력 | cm/s² | m/s² | A | `CentimetersPerSecondSquaredToMetersPerSecondSquared` |
| 각속도 | rad/s(축성) | rad/s | A | 없음(길이 차원 없음). 도/초 API는 Rocks 공개 API 층에서 rad/s로 바꾼 뒤 어댑터에 들어온다 |
| 질량 | kg | kg | — | 없음 |
| 밀도 | g/cm³(UE `UPhysicalMaterial::Density`, `PhysicsCore/Public/PhysicalMaterials/PhysicalMaterial.h:145-147`) | kg/m³ | — | ×1000. `UnitConversion.h`에 함수가 없다(K1) |
| 힘 | kg·cm/s²(U1) | N | A | `KilogramCentimetersPerSecondSquaredToNewtons` |
| 선형 충격량 | kg·cm/s(U1) | N·s | A | `KilogramCentimetersPerSecondToNewtonSeconds` |
| 토크 | kg·cm²/s²(U1) | N·m | A | `KilogramCentimetersSquaredPerSecondSquaredToNewtonMeters` |
| 각충격량·각운동량 | kg·cm²/s | kg·m²/s | A | 계수 10000. 함수가 없다(K1) |
| 관성 텐서 | kg·cm²(UE `Engine/Classes/Components/PrimitiveComponent.h:2863`) | kg·m² | A·I·Aᵀ | `KilogramSquareCentimetersToKilogramSquareMeters` |
| 회전 | 단위 quaternion (x, y, z, w) | 단위 quaternion(성분 순서 J4) | (A·v, w) | 없음 |
| 법선(접촉·hit·평면) | 단위 벡터 | 단위 벡터 | A, 부호 규약은 5절 | 없음 |
| 평면 상수 | cm | m | — | `CentimetersToMeters` |
| 선형 스프링 강성·감쇠 계수 | kg/s²·kg/s(= N/m·N·s/m) | 같음 | — | 없음(길이 차원이 약분된다) |
| 각 스프링 강성·감쇠 | kg·cm²/s²/rad·kg·cm²/s/rad | kg·m² 계열 | — | 계수 10000. 함수가 없다(K1) |
| 마찰·반발 계수, 감쇠 비율, 주파수, 시간 | 무차원·Hz·초 | 같음 | — | 없음 |
| 관절 각도 한계 | 도(UE `Engine/Classes/PhysicsEngine/ConstraintTypes.h:77`·`:105` `Swing1LimitDegrees`·`TwistLimitDegrees`) | rad | — | 도 → rad는 Rocks 공개 API 층 |

- 기본 중력: Rocks 월드 기본값은 UE 프로젝트 설정 기본값과 같은 (0, 0, −980) cm/s²다(UE `PhysicsCore/Private/PhysicsSettingsCore.cpp:29` `DefaultGravityZ`). Chaos solver 자체 기본값 (0, 0, −980.665)(`Experimental/Chaos/Public/Chaos/PerParticleGravity.h:17`)은 설정이 적용되기 전 값이므로 따르지 않는다. 기본안 A에서 Jolt 중력은 (0, −9.8, 0) m/s²다.
- 정밀도: cm → m는 정확한 정수 계수로 한 번 나눈다(`UnitConversion.h` 계약). 같은 값을 m → cm → m로 왕복하면 비트가 같다는 보장이 없으므로, 한 필드의 값은 한쪽만 소유한다(6절).
- K1: 밀도, 각충격량·각운동량, 각 스프링 강성 변환 함수는 범용 단위 변환이므로 물리 모듈에 따로 두지 않고 Core `UnitConversion.h`에 더한다(물리 착수 전 Core 작업).

## 4. 기하·형상

- **삼각형 메시 앞면.** det A = +1이라 정점 순서를 바꾸지 않아도 법선이 A로 대응한다. 남는 것은 앞면 규약의 차이다. Rocks 기하 `Triangle`·`Plane` 앞면 법선은 `Cross(b − a, c − a)`(UE `FPlane`·Chaos `FTriangle`과 같은 식, `Core/Public/Math/Plane.h:441-446`, `Experimental/Chaos/Public/Chaos/Triangle.h:120-122`)이고, 렌더 메시 앞면은 `(C − A) × (B − A)`다(`MeshDescription/Private/MeshDescription.cpp:1311-1313`). UE는 정적 메시 충돌을 쿠킹할 때 `bFlipNormals = true`로 v0·v1을 바꿔 렌더 메시 순서를 Chaos 앞면 순서로 맞춘다(`Engine/Private/StaticMesh.cpp:9343`, `Engine/Private/PhysicsEngine/Experimental/ChaosCooking.cpp:41-42`). Rocks 물리 쿠킹도 렌더 메시 입력을 같은 교환으로 기하 앞면 순서로 바꾼다. Jolt 메시 앞면의 수치 식(J5)이 `(B − A) × (C − A)`이면 기하 앞면 순서를 그대로 넘기고, 반대면 쿠킹에서 한 번 더 바꾼다. 교환은 쿠킹의 한 곳에서만 한다.
- **음수 행렬식 scale(거울 인스턴스).** 정점에 거울 변환이 들어가면 법선 식의 부호가 뒤집히므로 앞면을 보정해야 한다. Jolt가 음수 scale을 형상 scale로 받는지, 받으면 winding을 어떻게 다루는지(J8)를 확인하기 전에는 거울 인스턴스를 쿠킹 단계에서 별도 형상으로 만든다.
- **비균일 scale.** Rocks `Transform` scale은 형상 scale로 넘긴다(Jolt scaled shape의 지원 범위 J8). 비균일 scale 부모 아래 회전한 자식의 shear는 Rocks `Transform` 합성이 Unsupported로 거부하므로 물리에 들어오지 않는다(수학 구현 2절).
- **캡슐·실린더.** UE 캡슐은 로컬 Z축이 캡슐 축이다(`Engine/Classes/PhysicsEngine/SphylElem.h:13`). Rocks 물리 캡슐도 로컬 +Z축이다. Jolt 캡슐·실린더 축(J6)이 +Y이면 기본안 A가 Rocks +Z를 Jolt +Y로 옮기므로 형상 회전이 필요 없다. 아니면 고정 회전으로 감싼다. Rocks cylinder(UE에는 없는 단순 충돌·질의 형상)의 사상, convex radius 정책, 안정성 경고는 [Cylinder 형상 설계](CylinderShape.ko.md) 4절(CY2)을 따른다.
- **heightfield.** Rocks(UE landscape)의 표본 격자는 XY 평면이고 높이는 +Z다. 기본안 A에서 Rocks X는 Jolt Z, Rocks Y는 Jolt X가 되므로 Jolt 격자가 XZ 평면(J7)이면 표본 배열을 전치해 넘기고 높이는 `CentimetersToMeters`로 바꾼다. 칸마다 나누는 대각선 방향, 표본 색인 순서, 구멍 표시(J7)와 UE landscape 충돌의 대각선 선택(U3)이 맞지 않으면 같은 높이 표본에서 다른 표면이 나온다. 시험 PA07이 이를 고정한다.
- **convex·compound.** convex 점은 A와 cm → m로 바꾸고, compound 자식의 상대 변환은 이동을 A·(cm → m), 회전을 (A·v, w)로 바꾼다.
- **에셋 단순 충돌**(메시 에셋 스펙 MR5, 2026-11-20). 정적 메시의 `SimpleCollision`([메시·스켈레톤·애니메이션 에셋](MeshAnimationAssets/04-CommonSettings.ko.md) 4.6절)은 UE `FKAggregateGeom`과 같은 의미의 값이다. 형상 하나는 compound 자식 하나가 되고, 형상의 `center`·`rotation`(Rotator, 도)은 위 compound 자식 변환 규칙으로 바꾼다.

  | Rocks 형상 | 값의 의미(UE와 같음) | Jolt 형상 |
  |---|---|---|
  | `CollisionSphere` | 반경 | 구(반경 cm → m) |
  | `CollisionBox` | `size`는 축별 **전체** 길이 | 상자(반 크기 = size / 2, cm → m, 축은 A로) |
  | `CollisionCapsule` | `length`는 원기둥 부분 길이, 축은 로컬 +Z | 캡슐(반높이 = length / 2). 축 처리는 위 "캡슐·실린더" 항목 |
  | `CollisionTaperedCapsule` | 양 끝 반경 `radius0`·`radius1`, `length` | 테이퍼드 캡슐. 어느 끝이 `radius0`인지와 Jolt 위·아래 반경 대응은 J20 |
  | `CollisionCylinder` | Rocks 추가 형상 | [Cylinder 형상 설계](CylinderShape.ko.md) 4절(CY2) |
  | `CollisionConvex` | 점·index(double cm), 자체 transform | convex hull(위 convex 규칙). 쿠킹 때 Jolt 형상으로 직렬화한다 |

  - 형상 공통 필드: `collisionEnabled`는 body·질의 필터, `contributeToMass`가 거짓이면 질량 계산에서 뺀다(Jolt에서 그 자식의 밀도 0 또는 질량 재정의 — J21), `restOffset`의 Jolt 대응은 J21.
  - `CollisionComplexity`: `ComplexAsSimple`이면 렌더 메시 삼각형(`lodForCollision` LOD, 또는 `complexCollisionMesh`)을 Jolt mesh 형상으로 쓴다. Jolt mesh 형상은 동적 body에 쓸 수 없으므로 정적·kinematic body에만 허용하고, 동적 body면 단순 형상으로 내려가며 진단을 낸다(UE도 complex-as-simple 동적 body를 지원하지 않는다). 삼각형 앞면은 위 "삼각형 메시 앞면" 규칙을 따른다.

## 5. 이름이 붙은 방향과 부호 규약

- Jolt 설정 중 위·앞·왼쪽·오른쪽 같은 이름이나 기본 방향 값이 붙은 곳(캐릭터 up, 차량 앞·위, 좌우 바퀴 지정 — 존재와 의미는 J9)은 이름을 그대로 옮기지 않고 Rocks 방향을 A로 바꾼 수치로 정한다. 같은 전방(+X)·위(+Z)에서 `Cross(Up, Forward)` = +Y를 Rocks는 오른쪽이라 부르지만 오른손으로 읽으면 왼쪽이다. 두 엔진은 같은 수치 방향을 서로 반대 이름으로 부를 수 있다.
- 질의 hit 법선: Rocks 공개 hit 결과는 UE `FHitResult`와 같게 둔다. `ImpactNormal`은 맞은 표면에서 바깥을 향하고, `Normal`은 sweep한 형상 쪽(구라면 구 중심 쪽)을 향하며 선 질의에서는 `ImpactNormal`과 같다(`Engine/Classes/Engine/HitResult.h:55-69`). Jolt 질의·접촉 결과의 법선 방향(어느 body 쪽을 향하는지, J11)은 어댑터가 한 번만 부호를 맞춘다.
- 접촉점: Jolt 결과가 기준 위치에 대한 상대 좌표인지(J11)에 따라 월드 위치를 만든 뒤 A⁻¹, m → cm, 지역 원점 더하기 순서로 바꾼다. 침투 깊이의 부호 규약도 J11에서 확인한다.
- 질의 방향 벡터: Jolt ray·shape cast의 방향 벡터 길이가 최대 거리를 뜻하는지(J13)에 따라 hit 분수를 cm 거리로 바꾸는 식이 정해진다. Rocks `Raycast`의 거리는 실제 거리(cm)다(수학 구현 2.1절).

## 6. 큰 월드·정밀도·소유

- Rocks 월드 위치는 double cm다. Jolt float build에서는 지역 원점 o(PhysicsRegion, 전체 구현 계획 16.5절)를 double에서 먼저 뺀 뒤 m로 바꾸고 float로 한 번 반올림한다. 100으로 나누어도 float 상대 정밀도(약 2^-24)는 그대로이고, 같은 위치의 표현 간격은 2의 거듭제곱 경계 위치 차이로 최대 2배 안에서만 달라진다.
- Jolt double 위치 build(J10)를 쓰면 지역 원점 없이 double m로 넘길 수 있다. 채택은 비용 측정(전체 구현 계획 PC07)으로 정하며, 채택해도 서버 다중 physics system 분할은 별도 판단이다.
- 소유: 한 필드의 값은 한 방향으로만 흐른다(전체 구현 계획 16.2절 transform authority). Dynamic body의 위치·회전·속도는 step 결과를 Jolt → Rocks로만 바꾸고, Static·Kinematic 값은 Rocks → Jolt로만 바꾼다. 같은 tick에 Jolt에서 받은 값을 다시 Jolt에 넣는 경로를 두지 않아 cm↔m 왕복 반올림이 쌓이지 않게 한다.
- 길이 차원을 가진 Jolt 설정값(침투 허용치, 접촉 여유 거리, 최대 속도 등 — 목록과 기본값 J12)은 Jolt 기본값에 맡기지 않고 Rocks 설정(cm)을 같은 경계 함수로 바꿔 넣으며 version manifest에 기록한다.

## 7. 경계의 위치와 코드 규칙

- 변환은 Physics 모듈의 private adapter 안에서만 한다. Jolt 타입·상수는 adapter 밖으로 나가지 않는다(전체 구현 계획 3.1절 의존 표).
- 경계 함수는 물리량마다 하나씩 이름에 물리량을 드러낸다(예: 위치, 선속도, 각속도, 힘, 토크, 관성). 같은 `Vec3`라도 물리량에 따라 계수가 달라 하나의 범용 변환 함수로 묶으면 잘못된 계수를 고를 수 있기 때문이다. 방향 변환 A는 det +1이라 극성·축성 벡터에 같은 행렬을 쓰지만 함수는 물리량별로 나눈다.
- Rocks 공개 물리 API(UE `UPrimitiveComponent` 대응, 미구현)의 단위: 길이 cm, 질량 kg, 관성 kg·cm², 각속도는 UE처럼 `…InRadians`·`…InDegrees` 쌍(`Engine/Classes/Components/PrimitiveComponent.h:1860-1873`), 토크·각충격량도 같은 쌍(`:1702-1714`, `:1802-1813`), 관절 각도 한계는 도.

## 8. 검증 항목

착수 전에 고정 commit의 Jolt 공개 문서·소스로 확인한다. 공개 UE 연동 플러그인(예: UnrealJolt 계열)은 위치 계수·quaternion·winding·heightfield 변환의 비교 대상으로만 읽고, 이 문서의 식과 시험으로 검증하지 않은 코드를 옮기지 않는다(라이선스도 따로 확인).

| ID | 확인할 것 | 영향 |
|---|---|---|
| J1 | 좌우수 서술과 외적·quaternion 곱·벡터 회전 식이 표준식인지 | 2절 전제(식이 같으면 해석은 무관) |
| J2 | 기본 up 축과 up이 쓰이는 곳(기본 중력, 캐릭터, 차량, heightfield, 캡슐·실린더) | A 선택 |
| J3 | 길이·질량·시간·각도 단위와 설정값 단위 | 3절 계수 |
| J4 | quaternion 성분 순서·저장 배치 | 3절 회전 |
| J5 | 삼각형 앞면 수치 식, 질의·접촉의 뒷면 처리 | 4절 쿠킹 교환 |
| J6 | 캡슐·실린더 축 | 4절 |
| J7 | heightfield 평면·표본 색인 순서·칸 대각선·구멍 표시 | 4절, PA07 |
| J8 | scaled shape의 비균일·음수 scale 지원과 winding 처리 | 4절 |
| J9 | 캐릭터·차량 설정의 이름 붙은 방향(좌우 바퀴 등)과 기본값 | 5절 |
| J10 | double 위치 build의 이름·비용, 플랫폼 간 결정성 build(33.11절 S82 재확인) | 6절, 전체 구현 계획 16.4·16.5절 |
| J11 | 접촉 manifold·질의 결과의 법선 방향, 접촉점 기준, 침투 깊이 부호 | 5절 |
| J12 | 길이 차원 설정값 목록과 기본값 | 6절 |
| J13 | ray·shape cast 방향 벡터 길이의 의미와 hit 분수 | 5절 |
| J14 | 공개 header의 표준 라이브러리 사용, 예외·RTTI, allocator·job·로그·assert hook | 전체 구현 계획 2.1·16.4절 |
| J15 | 고정 version과 license 고지 | 전체 구현 계획 16.4절 |
| J16 | `NarrowPhaseQuery::CastShape`·`CollideShape`가 cylinder를 질의 형상으로 받는지, 결과 법선·거리 의미(J11 연동), cylinder 대 mesh·heightfield의 활성 모서리 처리 | Cylinder 형상 설계 4절 |
| J17 | `RayCylinder`의 정확성(끝면·모서리·시작점이 안), convex radius가 ray 판정에 주는 영향 | Cylinder 형상 설계 4절 |
| J18 | cylinder 반높이·반경과 convex radius의 관계(잘림인지 거부인지), 0 근처 동작, 얇은 cylinder 접촉 품질(v5.3.0 이후), convex radius 0 sensor의 침투 깊이 안정성 | Cylinder 형상 설계 4절 |
| J19 | cylinder 질량·관성 계산식과 단위 변환, 부력(`GetSubmergedVolume`) 근사, 캐릭터(`CharacterVirtual`) 형상으로 쓸 수 있는지 | Cylinder 형상 설계 4절 |
| J20 | 테이퍼드 캡슐의 위·아래 반경과 축 방향, UE `FKTaperedCapsuleElem`의 `Radius0`·`Radius1`이 어느 끝인지(UE 원본 확인 포함) | 4절 에셋 단순 충돌 |
| J21 | compound 자식별 질량 제외(UE `bContributeToMass`)와 rest offset(UE `RestOffset`)의 Jolt 대응 | 4절 에셋 단순 충돌 |
| U1 | UE `AddForce`·`AddImpulse`·`AddTorqueInRadians`·`AddAngularImpulseInRadians`의 실제 단위. 소스 주석에 단위가 없다("Magnitude indicates strength of force" — `Engine/Classes/Components/PrimitiveComponent.h:1753-1759`). 확인 방법(미실행): UE 5.8에서 질량 m body에 매 tick `AddForce(0, 0, 980·m)`를 걸어 중력 −980에서 정지가 유지되면 kg·cm/s², `AddImpulse(0, 0, 100·m)` 뒤 속도 변화가 100 cm/s면 kg·cm/s, 관성 I(kg·cm²)의 주축에 `AddTorqueInRadians` τ를 걸어 각가속도 τ/I면 kg·cm²/s² | 3절 |
| U2 | UE 에디터의 물리량 표시 단위 | Rocks 에디터 표시 |
| U3 | UE landscape 충돌의 칸 대각선 선택 | 4절, PA07 |

## 9. 수용 시험(미구현)

시험은 왕복 확인만으로 갈음하지 않고 Rocks 쪽 기대값을 구체 수치로 고정한다. 정확 일치는 방향 사상(성분 재배치)과 정확히 표현되는 단위 변환에만 쓰고, 적분 결과는 step 크기에서 유도한 허용 오차로 비교한다.

| ID | 시험 | 기대 |
|---|---|---|
| PA01 | 방향 상수 사상 | `Forward`·`Right`·`Up`·반대 방향 6개가 A의 결과와 정확히 같고 A⁻¹로 정확히 돌아온다 |
| PA02 | 각속도·quaternion | Rocks +Z 축 각속도 +ω(rad/s, yaw +)로 t초 적분한 body의 전방이 Rocks `Quaternion::FromUnitAxisAngle(Up, ω·t)`의 전방과 같다(+X → +Y 쪽으로 돈다) |
| PA03 | 중력 | 기본 중력에서 자유 낙하 1초 뒤 속도 (0, 0, −980) cm/s, 변위 −490 cm(적분 오차 허용) |
| PA04 | 힘·충격량 | 질량 2 kg body에 충격량 (0, 0, 200) kg·cm/s → 속도 변화 (0, 0, 100) cm/s. 힘 (0, 0, 1960) kg·cm/s²로 중력 상쇄 |
| PA05 | 토크·관성 | 주축 관성 I(kg·cm²) body에 토크 τ(kg·cm²/s²) → 각가속도 τ/I rad/s², 비대칭 관성 텐서가 A·I·Aᵀ로 같은 운동을 낸다 |
| PA06 | 메시 앞면 | 수학 구현 2.1절의 렌더 메시 삼각형 A(100,0,0)·B(100,10,0)·C(100,0,10) cm를 쿠킹하고 원점에서 +X로 ray: 앞면 hit, 거리 100 cm, `ImpactNormal` (−1, 0, 0) |
| PA07 | heightfield | 서로 다른 높이 표본 격자에서 알려진 (x, y) cm 위치 ray의 hit 높이가 Rocks 쪽 삼각 분할 기대값과 같다(대각선 포함) |
| PA08 | 접촉 법선 부호 | 평면 위에 놓인 구의 접촉·sweep 결과 `ImpactNormal`이 +Z |
| PA09 | 큰 월드 | 원점에서 10⁹ cm 떨어진 지역에서 PA03·PA06이 같은 허용 오차로 통과 |
| PA10 | 캡슐 축 | 로컬 +Z 캡슐을 세운 body가 평면 위에 서서 쓰러지지 않고 중심 높이가 원통부 반높이 + 반지름(cm) |

PC09(전체 구현 계획 16.4절)는 이 표의 통과를 뜻한다.
