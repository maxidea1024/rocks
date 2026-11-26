# Rocks 물리 어댑터 경계: Jolt 좌표·단위 변환 계약

기준일: 2026-11-25, 개정 4. 개정 4: UE용 Jolt 공개 플러그인 UnrealJolt와 대조한 결과(11절)를 더하고 PA02·PA05에 되읽기·크기 판정을 더했다. 같은 개정에서 solver 접촉 충격량을 읽는 추적 가능한 수정(10.6절)을 더해 물리 모듈 PMU2를 닫았다. 개정 3: Jolt v5.7.0 소스로 검증 항목 J1–J21을 확인하고 UE 5.8.4 소스로 U1·U3을 확인해 1–6절을 확정·정정했다(8절 결과표). 라이브러리 반입 설계(10절, JL1)를 더했다. 같은 개정 안에서 리뷰어 1명의 원본 대조(치명 0, 중요 5, 경미 8)를 반영했다: 조향·궤도 입력 부호, compound 자식 scale, heightfield 양자화·재질 전치, 연결 시험의 할당 판정과 호출 순서, PJG 범위, 행 번호·단위 정정. 개정 2: 근거에 컴포넌트 쪽 물리 요구(컴포넌트 도입 CR16) 링크를 더했다. 개정 1: 4절에 에셋 단순 충돌 형상 대응(메시 에셋 스펙 MR5)과 8절 J20·J21을 더했다.

이 문서는 Rocks 물리 모듈이 Jolt Physics와 주고받는 값의 좌표·방향·단위 변환 계약과 Jolt 라이브러리 반입 방식을 정한다. 물리 모듈(CR16)과 어댑터는 구현되지 않았다. 상태 표기는 **설계**(이 문서가 정한 계약, 코드 없음), **확인**(고정 판 소스·문서로 확인함, 근거 경로·행은 8절), **미확인**(남은 검증 항목)이다. Jolt 경로는 `Jolt/`(원본 저장소의 라이브러리 폴더) 기준, UE 경로는 `Engine/Source/Runtime/` 기준이다.

근거:

- 사용자 결정(2026-11-17): 물리 middleware 기준을 PhysX 5에서 Jolt로 되돌린다([전체 구현 계획](EngineImplementationPlan.ko.md) 개정 1.12, 16절).
- 사용자 요구(2026-11-17): 공간 좌표계·방향·회전 의미·단위는 UE 5.8과 같다. cm↔m 변환은 물리 어댑터 경계에서 하고, 극성·축성 벡터·quaternion·winding을 구분하며, 확인하지 못한 단위는 검증 항목으로 둔다.
- 사용자 결정(2026-11-25): 스펙 확정(이 문서의 검증)과 Jolt 라이브러리 반입(10절)은 계획 순서와 무관하게 지금 한다. 물리 모듈·어댑터 구현은 계획 순서(M4)대로다. 2026-11-17 요구 중 "물리 엔진 미구현이면 Jolt를 들이지 않는다"는 이 결정으로 라이브러리 반입까지 풀렸다.
- Rocks 규약 구현: [수학 구현](../Implementation/Math.ko.md) 2절("좌표·단위 규약", "단위 경계 변환", "행렬·Transform·view 규약")과 2.1절(기하 앞면).
- Jolt Physics v5.7.0(2026-11-14 릴리스, commit `cf16769bb0ede9c45d57d06068ff0689e9702f5c`, MIT). UE 5.8.4(commit `f45729c0b6f0`, 로컬 클론).
- 이 경계를 쓰는 컴포넌트 쪽 요구(body·충돌 채널·응답·프로필, overlap·hit 이벤트, sweep 이동, scene query, simulate physics, ragdoll physics asset)와 충돌 형상 컴포넌트(Box·Sphere·Capsule·Cylinder)는 [컴포넌트 도입](ComponentCatalog/04-Prerequisites.ko.md) CR16·3.10절·CM3에 있다. 물리 없는 영역 판정(`VolumeShape`, CR17)은 이 경계를 지나지 않는다.

## 1. 양쪽 규약

| 항목 | Rocks(구현됨, UE 5.8과 같음) | Jolt(**확인**, 8절) |
|---|---|---|
| 축·좌우수 | 왼손, +X 전방, +Y 오른쪽, +Z 위 | 오른손, Y-up(J1·J2). up은 중력 벡터와 형상·캐릭터·차량 설정으로 바꿀 수 있고 전역 up 설정은 없다 |
| 길이 | cm | m(J3, SI 권장) |
| 질량·시간 | kg, 초 | kg, 초(J3) |
| 각도 | `Rotator` 도. quaternion 축-각·삼각함수·각속도는 라디안 | 라디안(J3) |
| 수 표현 | 월드 위치 double, 그 밖 float·double | float. `JPH_DOUBLE_PRECISION` build에서 위치만 double, 질의 결과는 base offset 기준 float(J10) |

Rocks의 외적, Hamilton 곱, quaternion 벡터 회전, quaternion → 행렬 식은 오른손 수학 라이브러리의 표준식과 같다(수학 구현 2절 — UE 식을 옮긴 probe와 대조). Jolt의 외적·Hamilton 곱·`Quat::sRotation`도 같은 표준식이다(J1). 다른 것은 축에 붙인 이름(+Y를 오른쪽으로 읽음)과 그에 따라 양의 회전을 왼손 회전으로 읽는 해석뿐이다. 이 사실이 2절의 축 사상을 정한다.

## 2. 축 사상 — 행렬식 +1만 쓴다

**확정.** Rocks 벡터 r을 Jolt 벡터 j로 바꾸는 축 사상은 고정 직교 행렬 A(j = A·r)이며 det A = +1이다. A는 어댑터의 상수 하나로 두고 모든 경계 함수가 그것만 쓴다.

- 채택안: 축 순환 j = (r.y, r.z, r.x), 역 r = (j.z, j.x, j.y). Rocks `Forward` (1,0,0) → Jolt (0,0,1), `Right` (0,1,0) → (1,0,0), `Up` (0,0,1) → (0,1,0). 순환 치환이라 det A = +1이고, 성분을 옮기기만 하므로 방향 변환 자체는 정확하다(반올림 없음).
- 근거(J2·J6·J7·J9 확인): Rocks `Up`이 Jolt 기본 up(+Y)과 겹치고, Rocks `Forward`가 Jolt 차량 기본 전방(+Z)과 겹친다. 그래서 기본 중력 방향, 캡슐·실린더·테이퍼드 형상의 축(+Y), heightfield 평면(XZ), 캐릭터 up(`CharacterBaseSettings::mUp` 기본 +Y), 차량 up·전방(`VehicleConstraintSettings::mUp` +Y·`mForward` +Z)과 바퀴 기본 방향을 형상 회전 없이 Jolt 기본값 그대로 쓴다.
- 행렬식 −1 사상(거울, 예: Y 성분 부호만 뒤집기)은 쓰지 않는다.

거울이 필요 없는 이유: 좌우수는 수치 계산에 들어가지 않는 해석이다. 두 라이브러리가 같은 수식을 쓰므로 det A = +1이면 (A·a) × (A·b) = A·(a × b)이고, Jolt가 계산한 운동을 A⁻¹로 되돌리면 Rocks 좌표에서 같은 식으로 계산한 운동과 같다(반올림 차이 제외). 오른손으로 읽는 쪽과 왼손으로 읽는 쪽은 같은 수치를 서로 거울상으로 해석할 뿐이며, 역학 법칙을 같은 외적 식으로 계산하는 한 어느 해석에서도 운동은 올바르다. "좌우수가 다르니 축 하나를 뒤집는다"는 처리는 오히려 거울 사상이 되어 아래 표의 오른쪽 열처럼 축성 벡터·quaternion·winding·heightfield를 모두 따로 보정해야 한다.

| 대상 | det A = +1 사상(채택) | 거울 사상 M, det M = −1(채택하지 않음, 참고) |
|---|---|---|
| 극성 벡터(위치·변위·선속도·선가속도·중력·힘·선형 충격량·법선) | j = A·r | j = M·r |
| 축성 벡터(각속도·토크·각운동량·각충격량 — 외적으로 정의) | j = A·r(극성과 같다) | j = −M·r |
| quaternion (v, w) | (A·v, w), 회전 행렬은 A·R·Aᵀ | (−M·v, w) |
| 관성 텐서 I | A·I·Aᵀ(주축 값이면 성분 재배치) | M·I·Mᵀ |
| 삼각형 정점 순서 | 그대로(법선이 A로 대응) | 두 정점을 바꿔야 앞면이 유지 |
| heightfield 표본 격자 | 축 재배치(전치, 4절) | 거울상 격자 |

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
| 밀도 | g/cm³(UE `UPhysicalMaterial::Density`, `PhysicsCore/Public/PhysicalMaterials/PhysicalMaterial.h:145-147`) | kg/m³(`ConvexShapeSettings::mDensity`, 기본 1000) | — | ×1000. `GramsPerCubicCentimeterToKilogramsPerCubicMeter`(K1) |
| 힘 | kg·cm/s²(U1 확인) | N | A | `KilogramCentimetersPerSecondSquaredToNewtons` |
| 선형 충격량 | kg·cm/s(U1 확인) | N·s | A | `KilogramCentimetersPerSecondToNewtonSeconds` |
| 토크 | kg·cm²/s²(U1 확인) | N·m | A | `KilogramCentimetersSquaredPerSecondSquaredToNewtonMeters` |
| 각충격량·각운동량 | kg·cm²/s | kg·m²/s | A | `KilogramSquareCentimetersPerSecondToKilogramSquareMetersPerSecond`(K1) |
| 관성 텐서 | kg·cm²(UE `Engine/Classes/Components/PrimitiveComponent.h:2863`) | kg·m² | A·I·Aᵀ | `KilogramSquareCentimetersToKilogramSquareMeters` |
| 회전 | 단위 quaternion (x, y, z, w) | 단위 quaternion (x, y, z, w)(J4) | (A·v, w) | 없음 |
| 법선(접촉·hit·평면) | 단위 벡터 | 단위 벡터 | A, 부호 규약은 5절 | 없음 |
| 평면 상수 | cm | m | — | `CentimetersToMeters` |
| 선형 스프링 강성·감쇠 계수 | kg/s²·kg/s(= N/m·N·s/m) | 같음(Jolt 문서 Architecture.md:524) | — | 없음(길이 차원이 약분된다) |
| 각 스프링 강성·감쇠 | kg·cm²/s²/rad·kg·cm²/s/rad | N·m/rad·N·m·s/rad | — | `KilogramSquareCentimetersPerSecondSquaredPerRadianToNewtonMetersPerRadian`·`…PerSecondPerRadianToNewtonMeterSecondsPerRadian`(K1) |
| 주파수 지정 스프링(`ESpringMode::FrequencyAndDamping`) | Hz·무차원 | 같음 | — | 없음 |
| 마찰·반발 계수, 감쇠 비율, 시간 | 무차원·초 | 같음 | — | 없음 |
| 관절 각도 한계 | 도(UE `Engine/Classes/PhysicsEngine/ConstraintTypes.h:77`·`:105` `Swing1LimitDegrees`·`TwistLimitDegrees`) | rad | — | 도 → rad는 Rocks 공개 API 층 |

- 기본 중력: Rocks 월드 기본값은 UE 프로젝트 설정 기본값과 같은 (0, 0, −980) cm/s²다(UE `PhysicsCore/Private/PhysicsSettingsCore.cpp:29` `DefaultGravityZ`). Chaos solver 자체 기본값 (0, 0, −980.665)(`Experimental/Chaos/Public/Chaos/PerParticleGravity.h:17`)은 설정이 적용되기 전 값이므로 따르지 않는다. Jolt 기본값 (0, −9.81, 0)(`Physics/PhysicsSystem.h:392`)에 맡기지 않고 어댑터가 `SetGravity`로 (0, −9.8, 0) m/s²를 넣는다.
- 정밀도: cm → m는 정확한 정수 계수로 한 번 나눈다(`UnitConversion.h` 계약). 같은 값을 m → cm → m로 왕복하면 비트가 같다는 보장이 없으므로, 한 필드의 값은 한쪽만 소유한다(6절).
- K1: 밀도, 각충격량·각운동량, 각 스프링 강성·감쇠 변환 함수는 범용 단위 변환이므로 물리 모듈에 따로 두지 않고 Core `UnitConversion.h`에 더했다(2026-11-25 구현 — [수학 구현](../Implementation/Math.ko.md) 2절 표).
- 밀도 기본값: Jolt 기본 1000 kg/m³(물)와 UE `UPhysicalMaterial` 기본 밀도가 다르면 Rocks 물리 재질 기본값(UE와 같음)을 변환해 넣는다. Jolt 기본값에 맡기지 않는다.

## 4. 기하·형상

- **삼각형 메시 앞면(J5 확인).** Jolt 메시·삼각형 앞면 법선은 `(V2 − V1) × (V3 − V1)`이다(`Physics/Collision/Shape/TriangleShape.cpp:203`, `MeshShape.cpp:442`의 `(v3 − v2) × (v1 − v2)`는 전개하면 같은 식). ray의 뒷면 판정은 이 법선과 ray 방향의 내적이 양수인 경우다(`TriangleShape.cpp:251`, `MeshShape.cpp:797`). Rocks 기하 `Triangle`·`Plane` 앞면 법선 `Cross(b − a, c − a)`(UE `FPlane`·Chaos `FTriangle`과 같은 식, `Core/Public/Math/Plane.h:441-446`, `Experimental/Chaos/Public/Chaos/Triangle.h:120-122`)와 같은 식이고 det A = +1이 외적을 보존하므로, **기하 앞면 순서의 삼각형을 그대로 넘긴다**. 렌더 메시 앞면은 `(C − A) × (B − A)`다(`MeshDescription/Private/MeshDescription.cpp:1311-1313`). UE는 정적 메시 충돌을 쿠킹할 때 `bFlipNormals = true`로 v0·v1을 바꿔 렌더 메시 순서를 Chaos 앞면 순서로 맞춘다(`Engine/Private/StaticMesh.cpp:9343`, `Engine/Private/PhysicsEngine/Experimental/ChaosCooking.cpp:41-42`). Rocks 물리 쿠킹도 렌더 메시 입력을 같은 교환으로 기하 앞면 순서로 바꾸며, 교환은 쿠킹의 이 한 곳뿐이다. Jolt 문서의 "counter clockwise"는 오른손으로 앞면을 마주 본 서술이고, 같은 삼각형을 Rocks(왼손)로 앞에서 보면 시계 방향으로 읽힌다(이름만 다르고 수치 식은 같다).
- **음수 행렬식 scale(거울 인스턴스, J8 확인).** Jolt는 음수 scale을 받고 형상 내부에서 법선과 polygon winding을 뒤집는다(`ScaleHelpers::IsInsideOut`, `Physics/Collision/Shape/ScaleHelpers.h:33`). 그래서 거울 인스턴스를 쿠킹에서 별도 형상으로 만들지 않고 scale로 넘긴다(개정 2까지의 "별도 형상" 방침을 바꿨다). 시험 PA11이 고정한다.
- **비균일 scale(J8 확인).** 형상마다 허용 범위가 다르다(`IsValidScale`): 구·캡슐·테이퍼드 캡슐은 절댓값 균일 scale만(`SphereShape.cpp:328-331`, `CapsuleShape.cpp:419-422`, `TaperedCapsuleShape.cpp:441-444`), cylinder는 Jolt XZ(= Rocks XY, 반경 방향)가 균일해야 하고 축(Jolt Y = Rocks Z)은 자유(`CylinderShape.cpp:399-402`), box·convex·mesh·heightfield는 비균일 허용이다. 허용 밖 scale은 UE 규칙으로 먼저 형상 크기에 굽고 넘긴다: UE 구 반경은 |scale| 최솟값을 곱하고(`Engine/Private/PhysicsEngine/BodySetup.cpp:2006-2013`), 캡슐은 반높이 = (length + 2·radius) · |Z| / 2(최소 0.1), 반경 = radius · max(|X|, |Y|)를 [0.1, 반높이]로 clamp, 원통 길이 = 2 · (반높이 − 반경)(최소 0.1)이다(`BodySetup.cpp:2167-2182`). **compound 자식(리뷰 반영):** Jolt compound는 회전한 자식이 있으면 비균일 scale을 받지 않는다(`CompoundShape.h:195-202`, `ScaleHelpers::CanScaleBeRotated`). 에셋 단순 충돌은 회전한 자식이 흔하므로, 비균일 scale은 UE `GetFinalScaled`처럼 자식 형상마다 크기(와 자식 중심 위치)에 굽고 compound에는 균일 scale(또는 단위 scale)만 넘긴다. scale이 바뀌면 그 body의 형상을 다시 만든다. Jolt `MakeScaleValid`(평균 scale)는 UE와 결과가 달라 쓰지 않는다. 비균일 scale 부모 아래 회전한 자식의 shear는 Rocks `Transform` 합성이 Unsupported로 거부하므로 물리에 들어오지 않는다(수학 구현 2절).
- **캡슐·실린더(J6 확인).** UE 캡슐은 로컬 Z축이 캡슐 축이다(`Engine/Classes/PhysicsEngine/SphylElem.h:13`). Rocks 물리 캡슐도 로컬 +Z축이다. Jolt 캡슐·실린더·테이퍼드 캡슐·테이퍼드 실린더 축은 +Y이고(`CapsuleShape.h:20`, `CylinderShape.h:21`, `TaperedCapsuleShape.h:23`, `TaperedCylinderShape.h:21`) A가 Rocks +Z를 Jolt +Y로 옮기므로 형상 회전이 필요 없다. Rocks cylinder(UE에는 없는 단순 충돌·질의 형상)의 사상, convex radius 정책, 안정성 경고는 [Cylinder 형상 설계](CylinderShape.ko.md) 4절(CY2)을 따른다.
- **heightfield(J7·U3 확인).** Jolt heightfield 표면은 `offset + scale · (x, h[y · N + x], y)`, 정수 x, y ∈ [0, N − 1]인 **정사각** N × N 격자다(`HeightFieldShape.h:50-55`·`:72-73`). Jolt 격자의 x는 Jolt X, y는 Jolt Z, 높이는 Jolt Y다. 칸마다 대각선은 (x, y)–(x + 1, y + 1)이다(삼각형 생성 `HeightFieldShape.cpp:1975-2003`, 활성 모서리 법선 계산 `:240-270`의 `T1A`·`T1B`). UE Chaos heightfield도 칸 점 0 = (x, y), 1 = (x + 1, y), 2 = (x, y + 1), 3 = (x + 1, y + 1)로 삼각형 (0, 1, 3)·(0, 3, 2)를 만들어 같은 대각선을 쓴다(`Experimental/Chaos/Public/Chaos/HeightField.h:501-513`, `Experimental/Chaos/Private/Chaos/HeightField.cpp:1817-1820`, 색인 `y · NumCols + x` — `HeightField.cpp:930`). A에서 Rocks X → Jolt Z, Rocks Y → Jolt X이므로 Rocks 표본 (i, j)(i = Rocks X 색인, j = Rocks Y 색인, UE 배열 `j · N + i`)는 Jolt 표본 (x = j, y = i), 배열 `i · N + j`로 간다 — **전치**다. 대각선 (i, j)–(i + 1, j + 1)은 전치에서 (j, i)–(j + 1, i + 1)로 가므로 같은 대각선으로 남는다. 높이는 `CentimetersToMeters`, 칸 간격(UE landscape scale)은 Rocks X 간격 → Jolt `scale.z`, Rocks Y 간격 → Jolt `scale.x`다. 칸 재질 색인((N − 1)² 배열, `HeightFieldShape.h:55`)도 같은 규칙으로 전치한다. 시험 PA07이 이를 고정한다.
  - **양자화(리뷰 반영).** Jolt는 높이를 블록(`mBlockSize`²) min/max 기준으로 `mBitsPerSample` bit에 양자화하며 기본값은 8 bit다(`HeightFieldShape.h:93-96`). UE landscape 높이는 16 bit다. 어댑터는 `mBitsPerSample` = 16을 지정한다(UE 원본 정밀도 이하로 잃지 않게). 메모리를 줄이려면 `CalculateBitsPerSampleForError`로 허용 오차(cm)에서 bit 수를 정하는 방안을 지형 단계(CM18)에서 비교한다. PA07의 높이 허용 오차는 이 양자화 간격에서 유도한다. Jolt는 표본 수를 블록 크기 배수로 올림하므로(`HeightFieldShape.cpp:441`) 어댑터는 UE 컴포넌트 크기(`ComponentSizeQuads + 1`)와 블록 크기의 관계를 쿠킹에서 확인하고, 올림이 생기면 늘어난 표본을 구멍이 아닌 가장자리 복제로 채운다.
  - **구멍(한계).** UE는 칸 단위 구멍(재질 색인 255 — `HeightField.cpp:83`)이고 Jolt는 표본 단위 구멍(`cNoCollisionValue`, 그 표본을 쓰는 삼각형 전부 제거 — `HeightFieldShape.h:23`·`:34`)이다. 칸 하나만 비우는 UE 구멍은 Jolt 표본 구멍으로 정확히 옮길 수 없다(표본 하나를 비우면 주변 최대 6개 삼각형이 빠진다). 처리 방식(구멍 칸 주변을 Jolt mesh 조각으로 대신하는 방안 등)은 지형 충돌 단계(CM18, CR35)에서 정하고, 그 전까지 구멍 있는 heightfield 쿠킹은 진단을 내고 거부한다.
  - 정사각 제약: UE landscape 충돌 컴포넌트는 정사각(`ComponentSizeQuads + 1`)이라 맞는다. 비정사각 입력은 쿠킹이 거부한다.
- **convex·compound.** convex 점은 A와 cm → m로 바꾸고, compound 자식의 상대 변환은 이동을 A·(cm → m), 회전을 (A·v, w)로 바꾼다.
- **에셋 단순 충돌**(메시 에셋 스펙 MR5, 2026-11-20). 정적 메시의 `SimpleCollision`([메시·스켈레톤·애니메이션 에셋](MeshAnimationAssets/04-CommonSettings.ko.md) 4.6절)은 UE `FKAggregateGeom`과 같은 의미의 값이다. 형상 하나는 compound 자식 하나가 되고, 형상의 `center`·`rotation`(Rotator, 도)은 위 compound 자식 변환 규칙으로 바꾼다.

  | Rocks 형상 | 값의 의미(UE와 같음) | Jolt 형상 |
  |---|---|---|
  | `CollisionSphere` | 반경 | 구(반경 cm → m) |
  | `CollisionBox` | `size`는 축별 **전체** 길이 | 상자(반 크기 = size / 2, cm → m, 축은 A로) |
  | `CollisionCapsule` | `length`는 원기둥 부분 길이, 축은 로컬 +Z | 캡슐(반높이 = length / 2). 축 처리는 위 "캡슐·실린더" 항목 |
  | `CollisionTaperedCapsule` | `radius0`은 로컬 +Z 끝, `radius1`은 −Z 끝 반경, `length` | 테이퍼드 캡슐: `mTopRadius` = radius0, `mBottomRadius` = radius1, 반높이 = length / 2(J20 확인, 아래) |
  | `CollisionCylinder` | Rocks 추가 형상 | [Cylinder 형상 설계](CylinderShape.ko.md) 4절(CY2) |
  | `CollisionConvex` | 점·index(double cm), 자체 transform | convex hull(위 convex 규칙). 쿠킹 때 Jolt 형상으로 직렬화한다 |

  - **테이퍼드 캡슐(J20).** UE에서 `Radius0`은 +Z 끝, `Radius1`은 −Z 끝이다(`Engine/Private/PhysicsEngine/PhysDrawing.cpp:571-579` — 위 끝에 `ScaledRadius0`, 아래 끝에 `ScaledRadius1`). Jolt 테이퍼드 캡슐은 −Y 끝이 `mBottomRadius`, +Y 끝이 `mTopRadius`다(`TaperedCapsuleShape.h:23`). A가 +Z를 +Y로 옮기므로 radius0 → top, radius1 → bottom이다. **UE와 다른 점:** UE 5.8 Chaos는 테이퍼드 캡슐을 두 반경의 평균을 쓴 일반 캡슐로 바꿔 시뮬레이션한다(`Engine/Private/PhysicsEngine/Experimental/ChaosInterfaceUtils.cpp:213-215`). Rocks는 Jolt가 테이퍼드 캡슐을 직접 지원하므로 에셋 값의 모양 그대로 쓴다(값의 의미는 UE와 같고, 충돌 모양만 더 정확하다). UE와 접촉 결과가 달라지는 차이는 에셋 도구에 표시한다.
  - 형상 공통 필드(J21 확인): `collisionEnabled`는 body·질의 필터다. `contributeToMass`가 거짓이면 그 자식의 밀도를 0으로 넣는다 — Jolt compound 질량은 자식 질량의 합이고(`CompoundShape.cpp:68-91`, 질량 가중 질량 중심 `StaticCompoundShape.cpp:208-212`) 밀도 0을 막는 검사가 없다(`ConvexShape.h:27`·`:31`). 모든 자식이 질량에서 빠지면 쿠킹이 진단을 내고 거부한다(동적 body 질량 0). mesh 자식은 질량이 없으므로 이 필드를 보지 않는다. `restOffset`(UE `RestOffset`, 기본 0)은 Jolt에 대응 개념이 없어(접촉 여유는 전역 `mSpeculativeContactDistance`와 형상 convex radius뿐) 받지 않는다. 0이 아닌 값은 쿠킹이 경고하고 무시한다.
  - `CollisionComplexity`: `ComplexAsSimple`이면 렌더 메시 삼각형(`lodForCollision` LOD, 또는 `complexCollisionMesh`)을 Jolt mesh 형상으로 쓴다. Jolt mesh 형상은 동적 body에 쓸 수 없으므로 정적·kinematic body에만 허용하고, 동적 body면 단순 형상으로 내려가며 진단을 낸다(UE도 complex-as-simple 동적 body를 지원하지 않는다). 삼각형 앞면은 위 "삼각형 메시 앞면" 규칙을 따른다.

## 5. 이름이 붙은 방향과 부호 규약

- **이름 붙은 방향(J9 확인).** Jolt 설정에서 방향 이름과 기본값이 붙은 곳: 캐릭터 `CharacterBaseSettings::mUp` +Y·`mSupportingVolume` 평면 법선 +Y(`Physics/Character/CharacterBase.h:34`·`:39`), 차량 `VehicleConstraintSettings::mUp` +Y·`mForward` +Z(`Physics/Vehicle/VehicleConstraint.h:31-32`), 바퀴 `mSuspensionDirection` −Y·`mSteeringAxis` +Y·`mWheelUp` +Y·`mWheelForward` +Z(`Physics/Vehicle/Wheel.h:31-34`), 차동·안티롤바 `mLeftWheel`·`mRightWheel`(`Physics/Vehicle/VehicleDifferential.h:31-32`). 방향 값은 이름을 옮기지 않고 Rocks 방향을 A로 바꾼 수치로 넣는다. 채택한 A에서는 기본값이 Rocks Up·Forward·Down과 같다.
- **좌우 바퀴.** 같은 전방(+X)·위(+Z)에서 Rocks는 +Y를 오른쪽이라 부르고, A가 이를 Jolt +X로 옮긴다. Jolt 예제는 +X 쪽 바퀴를 "left"로 쓴다(`Samples/Tests/Vehicle/VehicleConstraintTest.cpp:70-72`·`:137-138` — 같은 +Z 전방·+Y 위를 오른손으로 읽으면 +X가 왼쪽). 그래서 **Rocks 오른쪽 바퀴는 Jolt `mLeftWheel`, Rocks 왼쪽 바퀴는 `mRightWheel`에 넣는다**. 어댑터는 이름이 아니라 바퀴 위치의 Rocks Y 부호로 배정하고, 시험 PA12가 고정한다.
- **조향·궤도 입력(리뷰 반영).** 같은 이름 문제가 운전 입력에도 있다. `WheeledVehicleController::SetDriverInput`의 `inRight` = +1은 Jolt 이름으로 "오른쪽"이고(`Physics/Vehicle/WheeledVehicleController.h:102`), 바퀴 조향각은 `−inRight · 최대 조향각`(`WheeledVehicleController.cpp:262`)을 `mSteeringAxis`(+Y) 둘레로 돌린 것이다(`VehicleConstraint.cpp:120`). 전방 +Z를 +Y 둘레로 음의 각만큼 돌리면 −X 쪽, 즉 Rocks 왼쪽으로 꺾인다. 그래서 **Rocks 오른쪽 조향 입력 +s는 Jolt `inRight` = −s로 넣는다**. 모터사이클(`MotorcycleController.cpp:156-157`, 같은 `−Sign(mRightInput)`)도 같다. 궤도 차량 `TrackedVehicleController::SetDriverInput`의 `inLeftRatio`·`inRightRatio`(`TrackedVehicleController.h:89-91`)는 Rocks 오른쪽 궤도 비율을 `inLeftRatio`에, 왼쪽을 `inRightRatio`에 넣는다. 시험 PA12가 조향 방향도 고정한다.
- 질의 hit 법선(J11 확인): Rocks 공개 hit 결과는 UE `FHitResult`와 같게 둔다. `ImpactNormal`은 맞은 표면에서 바깥을 향하고, `Normal`은 sweep한 형상 쪽(구라면 구 중심 쪽)을 향하며 선 질의에서는 `ImpactNormal`과 같다(`Engine/Classes/Engine/HitResult.h:55-69`). Jolt `CollideShapeResult::mPenetrationAxis`·`ContactManifold::mWorldSpaceNormal`은 "shape 2를 충돌에서 빼내는 방향"이다(`Physics/Collision/CollideShape.h:61`, `ContactListener.h:30`). shape cast(질의 형상 = shape 1, 맞은 형상 = shape 2)에서 `ImpactNormal` = A⁻¹·(−`mPenetrationAxis`.Normalized())다(Jolt 문서가 권하는 접촉 법선과 같다). ray 결과(`RayCastResult`)에는 법선이 없으므로 `Body::GetWorldSpaceSurfaceNormal(subShapeID, 점)`(바깥 방향)으로 얻는다. 어댑터는 이 부호를 한 곳에서만 맞춘다.
- 접촉점(J11 확인): Jolt 접촉점은 manifold `mBaseOffset` 기준 상대 좌표이고(`ContactListener.h:29`·`:34-35`), 질의 결과는 질의에 준 base offset 기준이다(`CollideShape.h:59-60`). 월드 위치는 (base offset + 상대 좌표)를 만든 뒤 A⁻¹, m → cm, 지역 원점 더하기 순서로 바꾼다. 침투 깊이는 겹치면 양수이고, `mMaxSeparationDistance` > 0일 때 떨어진 거리를 음수로 준다(`CollideShape.h:62`). Rocks 공개 값은 UE `FHitResult::PenetrationDepth`처럼 겹칠 때 양수(cm)다.
- 질의 방향 벡터(J13 확인): Jolt `RRayCast::mDirection`은 방향과 길이를 함께 담고 그 길이 밖은 보고하지 않으며(`Physics/Collision/RayCast.h:43`), 결과 `mFraction` ∈ [0, 1]은 hit = 시작 + fraction · (끝 − 시작)이다(`CastResult.h:25`). shape cast도 같다. 그래서 Rocks hit 거리(cm) = fraction · |Rocks 질의 벡터|(cm)이며, m 길이에서 다시 계산하지 않는다(반올림 왕복 방지). Rocks `Raycast`의 거리는 실제 거리(cm)다(수학 구현 2.1절).
- 볼록 형상 안에서 시작한 ray: Jolt는 `RayCastSettings::mTreatConvexAsSolid`가 참이면 fraction 0 hit을 낸다(`RayCast.h:84`). UE line trace는 시작점이 형상 안이면 `bStartPenetrating` hit을 낸다. 어댑터는 참으로 두고 fraction 0을 `bStartPenetrating`으로 옮긴다.

## 6. 큰 월드·정밀도·소유

- Rocks 월드 위치는 double cm다. Jolt float build에서는 지역 원점 o(PhysicsRegion, 전체 구현 계획 16.5절)를 double에서 먼저 뺀 뒤 m로 바꾸고 float로 한 번 반올림한다. 100으로 나누어도 float 상대 정밀도(약 2^-24)는 그대로이고, 같은 위치의 표현 간격은 2의 거듭제곱 경계 위치 차이로 최대 2배 안에서만 달라진다.
- Jolt double 위치 build(`JPH_DOUBLE_PRECISION`, J10 확인 — 위치만 double, 질의는 base offset 기준 float 결과. Jolt 문서는 전부 double로 바꾼 단순 구현이 2배 넘게 느리다고 적고 필요한 곳만 double을 쓴다 — `Docs/Architecture.md:758-765`)를 쓰면 지역 원점 없이 double m로 넘길 수 있다. 채택은 비용 측정(전체 구현 계획 PC07)으로 정하며, 채택해도 서버 다중 physics system 분할은 별도 판단이다. 반입 기본값은 float build다(10.2절).
- 소유: 한 필드의 값은 한 방향으로만 흐른다(전체 구현 계획 16.2절 transform authority). Dynamic body의 위치·회전·속도는 step 결과를 Jolt → Rocks로만 바꾸고, Static·Kinematic 값은 Rocks → Jolt로만 바꾼다. 같은 tick에 Jolt에서 받은 값을 다시 Jolt에 넣는 경로를 두지 않아 cm↔m 왕복 반올림이 쌓이지 않게 한다.
- 길이 차원 설정값(J12 확인): `PhysicsSettings`의 `mSpeculativeContactDistance` 0.02, `mPenetrationSlop` 0.02, `mLinearCastThreshold` 0.75, `mLinearCastMaxPenetration` 0.25, `mManifoldTolerance` 1e-3, `mMaxPenetrationDistance` 0.2, `mBodyPairCacheMaxDeltaPositionSq` (0.001)², `mContactPointPreserveLambdaMaxDistSq` (0.01)², `mPointVelocitySleepThreshold` 0.03 m/s, `mMinVelocityForRestitution` 1.0 m/s(`Physics/PhysicsSettings.h:50-102`), `mInternalEdgeRemovalVertexToleranceSq` 1e-8 m²(`:79-80`), 상수 `cDefaultConvexRadius` 0.05, `cCapsuleProjectionSlop` 0.02, `cDefaultCollisionTolerance` 1e-4(`:10-19`), `cDefaultInternalEdgeRemovalVertexToleranceSq` 1e-8 m²(`:21-22`), body 기본 최대 선속도 500 m/s(`BodyCreationSettings`). `cDefaultPenetrationTolerance`(1e-4)는 무차원 비율이라 길이 변환 대상이 아니다(`:12-13`). 이 값들은 Jolt 기본값에 맡기지 않고 Rocks 설정(cm·cm/s, 기본값은 위 m 값 × 100)을 같은 경계 함수로 바꿔 넣으며 version manifest에 기록한다. 제곱 거리 값은 제곱 면적 함수(`SquareCentimetersToSquareMeters`)로 바꾼다. 무차원 값(`mBaumgarte`, 반복 횟수, cos 각도 값)은 그대로 넣고 manifest에만 기록한다.

## 7. 경계의 위치와 코드 규칙

- 변환은 Physics 모듈의 private adapter 안에서만 한다. Jolt 타입·상수는 adapter 밖으로 나가지 않는다(전체 구현 계획 3.1절 의존 표). Jolt 공개 헤더는 STL을 쓰므로(J14) adapter 번역 단위만 STL 정책의 좁은 예외로 등록하고, Rocks 공개 헤더는 Jolt 헤더를 include하지 않는다(10.3절).
- 경계 함수는 물리량마다 하나씩 이름에 물리량을 드러낸다(예: 위치, 선속도, 각속도, 힘, 토크, 관성). 같은 `Vec3`라도 물리량에 따라 계수가 달라 하나의 범용 변환 함수로 묶으면 잘못된 계수를 고를 수 있기 때문이다. 방향 변환 A는 det +1이라 극성·축성 벡터에 같은 행렬을 쓰지만 함수는 물리량별로 나눈다.
- Rocks 공개 물리 API(UE `UPrimitiveComponent` 대응, 미구현)의 단위: 길이 cm, 질량 kg, 관성 kg·cm², 각속도는 UE처럼 `…InRadians`·`…InDegrees` 쌍(`Engine/Classes/Components/PrimitiveComponent.h:1860-1873`), 토크·각충격량도 같은 쌍(`:1702-1714`, `:1802-1813`), 관절 각도 한계는 도.

## 8. 검증 결과

개정 3에서 Jolt v5.7.0(commit `cf16769b`)의 소스·문서와 UE 5.8.4 소스로 확인했다. 공개 UE 연동 플러그인(UnrealJolt 계열)은 읽지 않았다. Jolt 경로는 `Jolt/` 기준이고, `Docs/`·`Samples/`·`.github/`는 Jolt 저장소 최상위 기준이다.

| ID | 확인할 것 | 결과 | 근거 | 반영 |
|---|---|---|---|---|
| J1 | 좌우수 서술과 외적·quaternion 곱·벡터 회전 식 | 오른손 서술. 외적·Hamilton 곱·축-각 quaternion은 표준식 | `Docs/Architecture.md:748`, `Math/Vec3.inl:855-889`(스칼라 경로 `:886-889`), `Math/Quat.inl:72-86`, `:160-166` | 1·2절 |
| J2 | 기본 up 축과 up이 쓰이는 곳 | +Y. 전역 up 설정은 없고 중력(`SetGravity`)·heightfield(회전 형상으로 감쌈)·차량·캐릭터 설정마다 up을 준다. 기본 중력 (0, −9.81, 0) | `Docs/Architecture.md:748`, `Physics/PhysicsSystem.h:194`·`:392` | 2·3절 |
| J3 | 단위 | SI(m, rad, s, kg) 권장. 동적 물체 0.1–10 m, 속도 0–500 m/s, 중력 0–10 m/s², 정적 물체 0.1–2000 m 범위에서 정확. 모터 힘 N·토크 N·m, 스프링 N/m·N·m/rad | `Docs/Architecture.md:752`, `:511-524` | 3절. 0.1–10 m = 10–1000 cm 범위 밖 물체의 진단은 어댑터 구현 때 |
| J4 | quaternion 성분 순서 | (x, y, z, w), w가 실수부 | `Math/Quat.h:42` | 3절 |
| J5 | 삼각형 앞면 식·뒷면 처리 | 앞면 `(V2 − V1) × (V3 − V1)`, 뒷면 = 법선·ray 방향 내적 > 0. `EBackFaceMode`로 질의별 무시·충돌 선택 | `TriangleShape.cpp:203`·`:251`, `MeshShape.cpp:442`·`:797`, `Physics/Collision/BackFaceMode.h:10-14`(경로는 `Physics/Collision/Shape/` 기준, 아래 같음) | 4절: 기하 앞면 순서 그대로 |
| J6 | 캡슐·실린더 축 | +Y(캡슐·실린더·테이퍼드 캡슐·테이퍼드 실린더) | `CapsuleShape.h:20`, `CylinderShape.h:21`, `TaperedCapsuleShape.h:23`, `TaperedCylinderShape.h:21` | 4절: 형상 회전 없음 |
| J7 | heightfield 평면·색인·대각선·구멍 | XZ 평면 높이 Y, 정사각 N × N, `h[y · N + x]`, 대각선 (x, y)–(x + 1, y + 1), 표본 단위 구멍 `cNoCollisionValue` | `HeightFieldShape.h:23`·`:34`·`:50-55`·`:72-73`·`:93-96`, `HeightFieldShape.cpp:441`·`:1975-2003` | 4절: 전치, 16 bit 양자화, 구멍 한계 |
| J8 | scaled shape의 비균일·음수 scale | 음수 scale 지원(inside-out이면 법선·winding을 뒤집음). 비균일 허용 범위는 형상별 `IsValidScale` | `ScaleHelpers.h:23-51`, `SphereShape.cpp:328-331`, `CapsuleShape.cpp:419-422`, `TaperedCapsuleShape.cpp:441-444`, `CylinderShape.cpp:399-402`, `CompoundShape.h:195-202`, `CompoundShape.cpp:393-410` | 4절: 거울 인스턴스 별도 형상 방침 폐기, compound 자식 scale 굽기 |
| J9 | 캐릭터·차량 이름 붙은 방향 | 5절 목록. 예제의 "left" 바퀴는 +X, 조향 입력 `inRight` = +1은 −X로 꺾음 | `Physics/Character/CharacterBase.h:34`·`:39`, `Physics/Vehicle/VehicleConstraint.h:31-32`, `VehicleConstraint.cpp:120`, `Wheel.h:31-34`, `VehicleDifferential.h:31-32`, `WheeledVehicleController.h:102`, `WheeledVehicleController.cpp:262`, `TrackedVehicleController.h:89-91`, `Samples/Tests/Vehicle/VehicleConstraintTest.cpp:70-113`·`:137-142` | 5절: Rocks 오른쪽 = Jolt left, 조향 부호 반전 |
| J10 | double 위치 build, 플랫폼 간 결정성 build | `JPH_DOUBLE_PRECISION`(위치만 double). `JPH_CROSS_PLATFORM_DETERMINISTIC`은 약 8% 느리고 컴파일러·구성·OS·아키텍처·word 크기와 무관하게 결정적. 조건: 같은 API 호출 순서·같은 소스. broadphase 질의 결과 순서·listener 호출 순서·`GetActiveBodies` 순서는 결정적이지 않다. CI 해시 검사는 Linux·Windows·macOS·ARM·RISC-V·PowerPC·LoongArch·WASM이고 Android·iOS는 없다 | `Docs/Architecture.md:758-765`·`:789-811`, `.github/workflows/determinism_check.yml`(작업 17개) | 6절·10.2절. 16.4절 서술과 같다(S82 재확인) |
| J11 | 접촉·질의 법선 방향, 접촉점 기준, 침투 깊이 부호 | 법선 = shape 2를 빼내는 방향. 접촉점은 base offset 기준. 침투 깊이는 겹치면 양수, 떨어지면 음수(여유 거리 > 0일 때) | `Physics/Collision/CollideShape.h:59-62`, `ContactListener.h:29-35` | 5절 |
| J12 | 길이 차원 설정값과 기본값 | 6절 목록(`cDefaultPenetrationTolerance`는 무차원) | `Physics/PhysicsSettings.h:10-102` | 6절 |
| J13 | ray·shape cast 방향 길이·fraction | 방향 벡터 길이 = 최대 거리, fraction ∈ [0, 1] | `Physics/Collision/RayCast.h:43`·`:84`, `CastResult.h:22-25` | 5절 |
| J14 | 공개 header의 STL, 예외·RTTI, allocator·job·로그·assert hook | 공개 헤더가 `<functional>`·`<sstream>`·`<algorithm>`·`<atomic>`·`<mutex>`·`<shared_mutex>`·`<thread>`를 include하고 `std::function` 등을 이름으로 연다. 예외·C++ RTTI는 기본 끔(자체 RTTI). 할당 hook은 문맥 인자 없는 전역 함수 포인터 5개. 로그 `Trace`·assert `AssertFailed`도 전역 함수 포인터. job 연결은 `JobSystem`(또는 `JobSystemWithBarrier`) 상속이며 `QueueJob`·`QueueJobs`는 반환값이 없어 실패를 표현할 수 없고 취소 API가 없다 | `Core/Core.h:508-560`, `Core/Mutex.h:11-14`, `Core/Atomics.h:8`, `Core/Memory.h:13-35`, `Core/IssueReporting.h:10-16`, `Core/JobSystem.h:69-170`·`:308-314`, `Build/CMakeLists.txt:44`·`:48` | 7절·10.3절. 16.1절 node pool·즉시 실행·완료 대기 규칙이 그대로 맞다 |
| J15 | 고정 version과 license | v5.7.0, MIT(Copyright 2021 Jorrit Rouwe). 라이브러리 원본 557개 파일 전부 `SPDX-License-Identifier: MIT` | `LICENSE`, 파일 머리 | 10.1절 |
| J16 | cylinder를 질의 형상으로 받는지 | 받는다. cylinder는 `ConvexShape` 하위 형식이라 볼록 대 볼록 collide·cast 표에 등록되고(`ConvexShape.cpp:556-562`), 볼록 대 mesh·heightfield는 볼록 공통 경로(활성 모서리는 `mCheckActiveEdges`, 기본 참 — `PhysicsSettings.h:123`)다 | `CylinderShape.cpp:91`·`:411-413`, `ConvexShape.cpp:556-562` | Cylinder 형상 설계 4절 |
| J17 | `RayCylinder` 정확성, convex radius 영향 | ray는 convex radius를 무시하고 정확한 원기둥(반높이·반경)과 판정한다. 시작점이 안이면 fraction 0 | `CylinderShape.cpp:292-295`, `Geometry/RayCylinder.h:11-16`·`:51-56` | Cylinder 형상 설계 4절: body 형상(기본 convex radius)의 ray와 sweep이 모서리에서 다를 수 있음 |
| J18 | convex radius와 반높이·반경의 관계 | 잘림: convex radius = min(설정값, 반높이, 반경). 음수는 형상 생성 오류 | `CylinderShape.cpp:94`·`:108`·`:121`·`:125` | Cylinder 형상 설계 4절: 거부 없이 잘림 |
| J19 | 질량·관성, 부력, 캐릭터 형상 | 질량·관성은 형상 자체 계산(`GetMassProperties`, 밀도 kg/m³), 부력은 볼록 공통 `GetSubmergedVolume`, `CharacterVirtual`은 임의 `Shape`를 받는다 | `CylinderShape.h:59-60`, `ConvexShape.h:64-65` | Cylinder 형상 설계 4절 |
| J20 | 테이퍼드 캡슐 위·아래 반경, UE `Radius0`·`Radius1` | UE `Radius0` = +Z 끝, `Radius1` = −Z 끝. Jolt top = +Y. UE Chaos는 평균 반경 일반 캡슐로 시뮬레이션 | UE `Engine/Private/PhysicsEngine/PhysDrawing.cpp:571-579`, `Experimental/ChaosInterfaceUtils.cpp:213-215`, Jolt `TaperedCapsuleShape.h:23`·`:51-57` | 4절 |
| J21 | compound 자식 질량 제외, rest offset | 자식 밀도 0으로 제외(검사 없음, compound 질량 = 자식 질량 합). rest offset 대응 없음 | `ConvexShape.h:27`·`:31`, `CompoundShape.cpp:68-91`, `StaticCompoundShape.cpp:208-212`, `PhysicsSettings.h:50` | 4절 |
| U1 | UE `AddForce`·`AddImpulse`·토크·각충격량 단위 | kg·cm/s², kg·cm/s, kg·cm²/s², kg·cm²/s. UE API 값이 변환 없이 Chaos 입자 힘·충격량에 더해지고(`PhysicsCore/Private/ChaosEngineInterface.cpp:851-859`·`:932-949`, `bAccelChange`면 질량을 곱함), Chaos는 cm·kg로 적분한다(중력 −980 cm/s²를 같은 입자 가속도에 더함 — `Experimental/Chaos/Public/Chaos/PerParticleGravity.h:34`) | 소스 추적. 개정 2의 실행 측정 절차는 어댑터 PA04·PA05 대조 시험으로 대신한다 | 3절 |
| U2 | UE 에디터 물리량 표시 단위 | 미확인 | — | Rocks 에디터 단계(물리 착수 gate와 무관) |
| U3 | UE landscape 충돌 칸 대각선 | (x, y)–(x + 1, y + 1), Jolt와 같다 | `Experimental/Chaos/Public/Chaos/HeightField.h:501-513`, `Experimental/Chaos/Private/Chaos/HeightField.cpp:1817-1820` | 4절 |

남은 항목: U2(물리 gate와 무관). K1은 2026-11-25에 구현했다.

## 9. 수용 시험(PH2a부터 단계별 구현 — 물리 모듈 7.1.2절 배정)

시험은 왕복 확인만으로 갈음하지 않고 Rocks 쪽 기대값을 구체 수치로 고정한다. 정확 일치는 방향 사상(성분 재배치)과 정확히 표현되는 단위 변환에만 쓰고, 적분 결과는 step 크기에서 유도한 허용 오차로 비교한다.

| ID | 시험 | 기대 |
|---|---|---|
| PA01 | 방향 상수 사상 | `Forward`·`Right`·`Up`·반대 방향 6개가 A의 결과와 정확히 같고 A⁻¹로 정확히 돌아온다 |
| PA02 | 각속도·quaternion | Rocks +Z 축 각속도 +ω(rad/s, yaw +)로 t초 적분한 body의 전방이 Rocks `Quaternion::FromUnitAxisAngle(Up, ω·t)`의 전방과 같다(+X → +Y 쪽으로 돈다). 되읽은 각속도가 (0, 0, ω) rad/s 그대로다(cm 배율이 붙지 않는다 — 11.2절) |
| PA03 | 중력 | 기본 중력에서 1/60 s 60 step 자유 낙하 뒤 속도 (0, 0, −980) cm/s, 변위는 Jolt 반암시 Euler 식 −980·dt²·n(n + 1)/2 = −498.17 cm(연속 해 −490 cm와 g·dt·t/2 차이 — step에서 유도한 기대값) |
| PA04 | 힘·충격량 | 질량 2 kg body에 충격량 (0, 0, 200) kg·cm/s → 속도 변화 (0, 0, 100) cm/s. 힘 (0, 0, 1960) kg·cm/s²로 중력 상쇄 |
| PA05 | 토크·관성 | 주축 관성 I(kg·cm²) body에 토크 τ(kg·cm²/s²) → 각가속도 τ/I rad/s², 비대칭 관성 텐서가 A·I·Aᵀ로 같은 운동을 낸다. 크기 판정: τ = 10⁴ kg·cm²/s²가 Jolt에 1 N·m로 들어간다(배율 10⁻⁴ — 11.2절) |
| PA06 | 메시 앞면 | 수학 구현 2.1절의 렌더 메시 삼각형 A(100,0,0)·B(100,10,0)·C(100,0,10) cm를 쿠킹하고 (0, 2, 2) cm에서 +X로 ray(꼭짓점·모서리를 피한 내부 점): 앞면 hit, 거리 100 cm, `ImpactNormal` (−1, 0, 0) |
| PA07 | heightfield | 서로 다른 높이 표본 격자에서 알려진 (x, y) cm 위치 ray의 hit 높이가 Rocks 쪽 삼각 분할 기대값과 같다(대각선 포함, 비대칭 표본으로 전치 오류를 드러냄) |
| PA08 | 접촉 법선 부호 | 평면 위에 놓인 구의 접촉·sweep 결과 `ImpactNormal`이 +Z |
| PA09 | 큰 월드 | 원점에서 10⁹ cm 떨어진 지역에서 PA03·PA06이 같은 허용 오차로 통과 |
| PA10 | 캡슐 축 | 로컬 +Z 캡슐을 세운 body가 평면 위에 서서 쓰러지지 않고 중심 높이가 원통부 반높이 + 반지름(cm) |
| PA11 | 거울 인스턴스 | scale (1, −1, 1)인 PA06 삼각형 메시 인스턴스에 (0, −2, 2) cm에서 쏜 +X ray가 앞면 hit, `ImpactNormal` (−1, 0, 0)(Jolt inside-out 처리) |
| PA12 | 좌우 바퀴·조향 | Rocks Y > 0 바퀴가 Jolt `mLeftWheel`로, Y < 0 바퀴가 `mRightWheel`로 배정되고 한쪽만 구동한 차가 Rocks 기대 방향으로 돈다. Rocks 오른쪽 조향 입력에서 앞바퀴 전방이 +Y 쪽으로 꺾이고 차가 오른쪽으로 돈다. 궤도 차량의 오른쪽 궤도 비율을 줄이면 오른쪽으로 돈다 |
| PA13 | 테이퍼드 캡슐 | radius0 > radius1인 테이퍼드 캡슐을 +Z로 세운 body의 +Z 끝 ray hit 거리가 radius0 기준 기대값과 같다 |

PC09(전체 구현 계획 16.4절)는 이 표의 통과를 뜻한다.

## 10. 라이브러리 반입(JL1)

엔진 코드를 바꾸지 않는 단계라 계획 순서와 무관하게 지금 한다(사용자 결정 2026-11-25). 방식은 기존 ThirdParty 모듈과 같다([글꼴 설계](Font/08-ThirdParty.ko.md) 8절 머리말): 폴더에 `ThirdParty.json`(출처·SHA-256·검증 방법·SPDX·고지·번역 단위·include·define·adapter 헤더·파일별 SHA-256·pragma 지시문·notes)과 `Jolt.Build.cs`(`ThirdPartyModuleRules`)를 두고, 원본은 `upstreamDirectory`에 byte 그대로 보존한다. `files`·`pragmaDirectives`는 갱신 도구(`Rocks.ToolTests third-party --root . --write`)로 계산한다.

### 10.1 판·출처·범위

- 판: Jolt Physics **v5.7.0**(2026-11-14 릴리스, 도입 시점 최신 안정판). UE 5.8 트리에는 Jolt가 없어 하한이 없다.
- 출처: GitHub 태그 압축 `https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.7.0.tar.gz`(SHA-256 `9a0a405f2375dc4bd9f72486c56e1cbc7794c6a62e42fc48fadea45dfc93dacc`). 릴리스 asset·서명이 없으므로 태그 commit `cf16769bb0ede9c45d57d06068ff0689e9702f5c`의 checkout과 압축 내용이 같은지를 검증으로 기록한다(msdfgen·nanosvg 선례).
- 위치: `Engine/Source/ThirdParty/Jolt/`, 원본 디렉터리 `JoltPhysics-5.7.0/`(`LICENSE`와 `Jolt/` 아래 라이브러리 소스·헤더). 엔진 기능 모듈이 쓰는 라이브러리라 Engine 층이다.
- 들이는 번역 단위: Jolt.cmake의 기본 소스 목록(`JOLT_PHYSICS_SRC_FILES` 첫 정의, `.cpp` 130개)에서 아래 6개를 뺀 **124개**. 헤더는 `Jolt/` 아래 `.h`·`.inl` 중 제외 폴더(`Compute/`·`Shaders/`·`Physics/Hair/`)를 뺀 전부다(Jolt.cmake 기본 목록에 있는 `Compute/*.h`·`Physics/Hair/*.h`도 들이지 않는다). 테스트·예제·뷰어·빌드 스크립트·문서는 들이지 않는다.
  - `Physics/Hair/` 4개(`Hair.cpp`·`HairSettings.cpp`·`HairShaders.cpp`·`RegisterHair.cpp`)와 `Compute/ComputeSystem.cpp`, 그리고 `Compute/`·`Shaders/` 폴더 전체: v5.7.0에 새로 들어온 GPU compute 머리카락 시뮬레이션이다. Jolt 자체 DX12·Vulkan·Metal 장치와 HLSL 셰이더를 만들어 Rocks RHI·셰이더 체계와 겹친다. 머리카락(groom)은 계획 P3이고, 그때 Rocks RHI 위에서 따로 정한다.
  - `Core/JobSystemThreadPool.cpp`: `std::thread` 자체 pool이다. 16.1절이 별도 무제한 pool을 금지하므로 번역 단위에서 빼 링크 단계에서 사용을 막는다(헤더는 들어오지만 쓰면 링크 오류).
- `Renderer/`의 번역 단위 4개는 `JPH_DEBUG_RENDERER`가 없으면 본문이 빈다. 헤더 쪽 include는 모두 `#ifdef JPH_DEBUG_RENDERER` 안에 있어 지금은 필요 없지만, 디버그 그리기를 켤 때 목록을 바꾸지 않도록 Jolt.cmake 기본 목록 그대로 둔다. `ObjectStream/SerializableObject.cpp`는 `JPH_OBJECT_STREAM`이 없어도 RTTI 구현(`JPH_IMPLEMENT_SERIALIZABLE_ABSTRACT`, `SerializableObject.cpp:11`)을 담아 꼭 필요하다(끄면 빠지는 것은 직렬화 함수뿐 — `SerializableObject.h:15-73`). 텍스트 직렬화 `ObjectStream*.cpp`·`TypeDeclarations.cpp`는 Jolt.cmake에서도 `ENABLE_OBJECT_STREAM` 조건부라 들이지 않는다. 쿠킹은 Jolt 이진 직렬화(`Shape::SaveBinaryState`, `StreamOut`)를 쓴다.

### 10.2 빌드 설정

| 항목 | 값 | 이유 |
|---|---|---|
| 언어 | 엔진과 같은 C++23 인자(Jolt 요구는 C++17), 예외·C++ RTTI 끔 | C++ 서드파티 공통 규칙([압축](Compression.ko.md) 3.3절). Jolt는 자체 RTTI라 `rtti`는 거짓. 상류는 C++17만 시험하므로(`Jolt.cmake:2`, CI에 C++20 이상 없음) C++23·엔진 경고 규칙에서 경고가 나면 추적 가능한 수정(`modifications`)으로 고친다 |
| 부동소수점 | Rocks 공통 `-ffp-contract=off` | 16.4절 |
| `JPH_CROSS_PLATFORM_DETERMINISTIC` | 켬 | 아래 결정 |
| `JPH_DOUBLE_PRECISION` | 끔 | float build + PhysicsRegion이 기본안(16.5절), double 채택은 PC07 측정 뒤 |
| `JPH_ENABLE_ASSERTS` | 모든 구성에서 켬 | Rocks `ROCKS_ASSERT`가 모든 구성에서 켜져 있는 것과 같다. 이 정의는 구조체 배치를 바꾸므로(`MotionProperties.h:325-328`, `BroadPhaseQuadTree.h:63-66`, `Core/Mutex.h:162`, `PhysicsLock.h:136`의 assert 전용 필드) 라이브러리와 소비자가 같아야 하고, 버전 ID 기능 bit에도 들어간다. 구성별 정의를 manifest가 받지 않으므로 한 값으로 고정한다. 비용은 PC07과 함께 측정한다 |
| `JPH_OBJECT_LAYER_BITS` | 32(PH1 — 2026-11-25에 16에서 바꿨다) | object 층 하나에 채널 6 bit·운동 묶음 2 bit·참여 표지 3 bit·응답 묶음 색인 21 bit를 담는다([물리 모듈](PhysicsModule/06-JoltMapping.ko.md) 6.1절 PMD12 — broadphase 층은 object 층만 보고 정하므로 운동 묶음이 층에 들어가야 한다). 버전 ID 기능 bit가 바뀌므로 소비자 정의가 어긋나면 `VerifyJoltVersionID`가 잡는다 |
| `JPH_DEBUG_RENDERER`·`JPH_PROFILE_ENABLED`·`JPH_EXTERNAL_PROFILE` | 끔 | 디버그 그리기는 물리 디버그 단계, 프로파일 연결(Trace)은 물리 모듈 단계에서 정한다 |
| `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED` | 끔 | Rocks는 FP 예외 trap을 켜지 않는다 |
| `JPH_DISABLE_CUSTOM_ALLOCATOR`·`JPH_DISABLE_TEMP_ALLOCATOR` | 끔(할당 hook과 임시 할당기를 쓴다) | 16.4절 할당 hook |
| GPU compute(`JPH_USE_DX12`·`JPH_USE_VK`·`JPH_USE_MTL`·`JPH_USE_CPU_COMPUTE`) | 끔 | 10.1절 |
| SIMD 정의(`JPH_USE_SSE4_1`·`AVX`·`AVX2`·`FMADD` 등) | 주지 않음 | Jolt가 컴파일러 기준 매크로로 고른다(`Core/Core.h:142-231`). Rocks 빌드는 ISA 인자를 금지하므로 x64는 SSE2, ARM64는 NEON이다. FMA는 결정성 build가 끈다(`Core/Core.h:150`·`:192`) |
| `JPH_DEBUG` | 라이브러리·소비자 모두 정의하지 않음 | Jolt.cmake는 Debug 구성에서 정의하고(`Jolt.cmake:843`), 정의되면 `Core/Core.h:13`이 assert를 자동으로 켜며 `JPH_IF_DEBUG` inline이 달라진다. 구성별 정의를 쓰지 않으므로 모든 구성에서 같게 둔다 |
| `consumerDefines` | `JPH_CROSS_PLATFORM_DETERMINISTIC`, `JPH_ENABLE_ASSERTS`, `JPH_OBJECT_LAYER_BITS=32` | Jolt 버전 ID(`JPH_VERSION_ID`, `Core/Core.h:22-88`)의 기능 bit(`JPH_DOUBLE_PRECISION`, `JPH_CROSS_PLATFORM_DETERMINISTIC`, `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED`, `JPH_PROFILE_ENABLED`, `JPH_EXTERNAL_PROFILE`, `JPH_DEBUG_RENDERER`, 두 allocator 끔, `JPH_OBJECT_LAYER_BITS`, `JPH_ENABLE_ASSERTS`, `JPH_OBJECT_STREAM`)와 구조체 배치에 드는 정의는 소비자에도 같아야 한다. 소비자는 `RegisterTypes` 전에 `VerifyJoltVersionID()`로 확인한다(`RegisterTypes`는 bit가 다르면 바로 abort — `RegisterTypes.cpp:66-94`) |
| 스레드·FP 인자 | 비Windows는 `-pthread`(`Jolt.cmake:1037-1040`, Rocks 빌드가 이미 thread 라이브러리를 링크하는지 JL1 빌드로 확인), `/fp:fast` 계열 금지(`Build/CMakeLists.txt:204-207` — Rocks 빌드가 이미 거부) | CMake 밖에서 빌드하므로 Jolt.cmake가 거는 인자를 따로 맞춘다. `NDEBUG`·`-fno-strict-aliasing`은 Jolt 소스가 쓰지 않아 필요 없다 |
| CMake 기본값과 다른 점 | `FLOATING_POINT_EXCEPTIONS_ENABLED` 기본 켬(`Build/CMakeLists.txt:40`) → 끔, `ENABLE_OBJECT_STREAM` 기본 켬(`:107`) → 끔, `DEBUG_RENDERER_IN_DEBUG_AND_RELEASE`·`PROFILER_IN_DEBUG_AND_RELEASE` 기본 켬 → 끔, `USE_AVX2` 등 기본 켬 → 끔 | manifest notes에도 적는다 |
| `globalState` | 참 | `Factory::sInstance`, 할당·`Trace`·`AssertFailed` 전역 함수 포인터, `RegisterTypes` |

**결정성 build를 켜는 이유(결정).** Rocks는 이미 Core에 `-ffp-contract=off`를 걸어 플랫폼 간 수치 일치를 우선한다. x64 기준 ISA에 FMA가 없으므로 결정성 build가 x64에서 바꾸는 것은 rsqrt 근사 대신 정확한 역제곱근(`Math/Math.h:122-149`)과 −0.0 처리(`Math/Vec3.inl:674-695`·`:902-923`)이고, ARM64에서는 FMA도 끈다(`Core/Core.h:150`). 비용은 옵션 전체로 약 8%다(Jolt 문서 `Docs/Architecture.md:796`, 플랫폼별 분해 없음). 서버 권한 보정(17.2절)이 기본이라도 같은 입력에서 같은 결과가 나오면 보정 빈도(PC08)와 replay가 좋아진다. 16.4절의 "채택은 비용과 PC04로 정한다"는 반입 기본값을 켬으로 두고 PC04·PC07 측정 결과가 비용을 정당화하지 못하면 끄는 것으로 바꾼다. 켜도 Android·iOS 해시는 Jolt CI에 없으므로 PC04 전에는 플랫폼 간 결정성으로 표기하지 않는다. 버린 대안: 끔으로 시작해 나중에 켬 — 쿠킹 결과·replay 형식이 기본값에 묶이기 전에 정하는 편이 바꿀 때 비용이 적다.

### 10.3 STL·정책·소비자

- Jolt 공개 헤더는 STL을 include하고 `std::` 이름을 `JPH` 이름공간으로 연다(J14). Rocks 공개 헤더는 Jolt 헤더를 include하지 않는다. 물리 모듈(CR16) 단계에서 adapter 번역 단위만 STL 정책의 좁은 예외로 등록한다(msdfgen adapter 선례 — STL은 유불리 판단 뒤 최소 사용). 원본 디렉터리에는 Rocks 정책 검사를 적용하지 않는다(기존 ThirdParty와 같다).
- JL1의 소비자는 연결 시험 실행 파일 하나다(Physics 모듈은 CR16 단계). 시험 파일은 Jolt 헤더를 include하되 `std::`를 직접 쓰지 않는다.
- 서버 타깃도 물리를 쓰므로(전용 서버) Jolt는 ClientOnly가 아니다.

### 10.4 hook 연결 계약(물리 모듈이 구현, JL1은 시험만)

- 할당: `JPH::Allocate`·`Reallocate`·`Free`·`AlignedAllocate`·`AlignedFree`는 문맥 인자 없는 전역 함수 포인터다(`Core/Memory.h:28-32`). Physics tag 고정 함수 다섯 개로 Rocks 할당기에 연결하고, `RegisterTypes` 전·`Factory` 생성 전에 설정한다. 메모리 부족은 진단 후 fatal(16.4절). 임시 할당(`TempAllocatorImpl`)은 Physics 모듈이 고정 크기 블록을 미리 잡는다.
- 로그·assert: `JPH::Trace`(printf 형식)와 `JPH::AssertFailed`를 구조화 로그에 연결한다. assert 실패는 시험 실패로 판정하고 hook은 거짓(중단하지 않음)이 아니라 Rocks assert 경로로 보낸다. hook 안에서 lock 획득·게임 코드 실행 금지(16.4절).
- job: `JobSystemWithBarrier`를 상속한 adapter가 `QueueJob`·`QueueJobs`를 Task 모듈 `WorkerScheduler`에 제출한다. 반환값이 없어 실패를 표현할 수 없으므로 미리 잡은 node pool을 쓰고, 포화 시 제출 thread에서 즉시 실행한다. 취소 API가 없으므로 종료는 진행 중 step 완료를 기다린다(16.1절 규칙 그대로, J14).
- FP 상태: Jolt step을 실행하는 모든 worker의 FP 상태(FTZ·DAZ·반올림 방식)가 같아야 한다(다르면 broadphase bounds 불일치 — `Physics/Body/Body.h:298-302`). Task 모듈 worker의 FP 상태가 같다는 것을 어댑터 시험으로 확인한다.
- 시작 순서: 할당 hook 설정 → `VerifyJoltVersionID()`(거짓이면 등록 실패, `RegisterTypes`는 bit가 다르면 진단 없이 abort하므로 먼저 확인) → `Factory::sInstance` 생성(`RegisterTypes`가 assert로 요구 — `RegisterTypes.cpp:97`) → `RegisterTypes`. 종료는 `UnregisterTypes` → `Factory::sInstance` 삭제·null(`RegisterTypes.cpp:195-203`) 순서다.

### 10.5 JL1 완료 조건

1. `Jolt` 모듈이 Windows에서 빌드된다. macOS·Android는 CI에서 빌드된다(Linux는 Program 타깃 — 글꼴 FT1 완료 조건 1과 같다).
2. 연결 시험(EngineTests 외부 라이브러리 시험 `Programs/FoundationTests/Engine/ThirdPartyJoltTests.cpp`, CPU — 실행 파일이 라이브러리를 직접 링크하고 Jolt 헤더를 경고 0으로 include한다):
   - 할당 hook을 세는 함수로 설정 → `VerifyJoltVersionID()` 참 → `Factory` 생성 → `RegisterTypes` → … → `UnregisterTypes` → `Factory` 삭제 뒤 hook으로 받은 할당의 잔여가 0이고 hook이 한 번 이상 불렸다. `StateRecorderImpl`(`std::stringstream`, `StateRecorderImpl.h:47`)과 `JobFunction`(`std::function`, `Core/JobSystem.h:147`)은 전역 `operator new`를 쓰므로 "모든 할당이 hook으로 온다"는 판정은 하지 않는다. Physics 모듈이 전역 할당 경로를 어떻게 다룰지는 그 단계에서 정한다(한계).
   - `Trace`·`AssertFailed` hook 설정(호출되면 시험 실패).
   - `PhysicsSystem`(단일 thread `JobSystemSingleThreaded`, `TempAllocatorImpl`)에 정적 box 바닥과 동적 구 하나를 넣고 1/60 s로 60 step: 자유 낙하 구간 속도가 중력 적분 기대값과 허용 오차 안에서 같고, 구가 바닥 위에 멈춘다(높이 = 반지름, 허용 오차는 `mPenetrationSlop`).
   - 같은 장면을 두 번 돌려 `StateRecorderImpl` 상태 byte가 같다(같은 binary 반복성 — PC03의 최소형). 플랫폼 간 해시 비교는 PC04다.
   - 위에서 아래로 쏜 ray가 바닥에 맞고 fraction이 기대 거리와 같다, cylinder(convex radius 0)를 질의 형상으로 `CastShape`가 결과를 낸다(J16 확인의 실행 확인).
   - 좌표 사상·단위 변환은 adapter가 없으므로 이 시험에 넣지 않는다(PA01–PA13은 물리 모듈 단계).
3. format·policy·ThirdParty 검사(`third-party` 정책, manifest 해시) 통과, 바뀐 시험만 실행.
4. 고지: MIT 고지(`binaryNoticeRequired` 참)가 배포 고지 묶음에 들어간다.

**JL1 구현 결과(2026-11-25).** Windows x64 Debug(Clang 22.1.8)에서 번역 단위 124개가 엔진 경고 규칙으로 경고 0이고, 추적 가능한 수정이 필요 없었다(C++23에서도 원본 그대로 — 기능 수정 1건은 뒤에 더했다, 10.6절). 연결 시험 2개(`ThirdParty_JoltSimulatesQueriesAndReleasesHookedMemory`, `ThirdParty_JoltTriangleFrontFaceFollowsRightHandCross`)가 통과했다: 자유 낙하가 반암시 Euler 식과 1e-3 안에서 같고, 구가 y = 0.5 m에서 잠들며, 두 번 돌린 상태 byte가 같고, ray·cylinder cast의 fraction·법선 부호(J11·J13·J16)와 삼각형 앞면(J5)이 기대대로이며, hook 할당 잔여가 0이다. format·policy·`third-party` 검사도 통과했다. Android(2026-11-25, NDK 29.0.13113456 Clang 20, API 26): arm64·x86_64 교차 빌드에서 Jolt 124개 번역 단위가 경고 0이고(arm64는 NEON 경로·결정성 build의 FMA 끔 그대로 컴파일), x86_64 에뮬레이터(API 35)에서 연결 시험 2개가 통과했다. arm64 실행은 기기가 없어 확인하지 못했다. Android 빌드를 막던 다른 서드파티 넷(Minimp3·Ogg·HarfBuzz·ICU — Windows에서는 나지 않던 경고·누락 헤더)은 추적 가능한 수정으로 고쳤다(d927b021). macOS는 CI에서 확인한다. 시험을 쓰다 확인한 사용 주의: `ObjectVsBroadPhaseLayerFilterTable`은 생성할 때 레이어 표를 읽어 자기 표를 만들므로 표를 먼저 채워야 한다(늦게 채우면 충돌 쌍이 하나도 없어 물체가 바닥을 통과한다). Physics 모듈의 레이어 설정도 같은 순서를 지킨다.

### 10.6 추적 가능한 수정: solver 접촉 충격량(2026-11-25)

Jolt는 solver가 접촉점마다 쓴 충격량(non-penetration lambda)을 다음 스텝 warm start용으로 접촉 캐시에 저장하지만(`Physics/Constraints/ContactConstraintManager.cpp`의 `StoreAppliedImpulses`, 캐시 칸 `CachedContactPoint::mNonPenetrationLambda` — 수정한 파일이라 행 번호 대신 이름으로 적는다) 읽는 공개 API가 없다. UE hit 이벤트의 `NormalImpulse`는 solver 충격량을 접촉마다 법선 방향 벡터로 더한 값이라(`Engine/Private/PhysicsEngine/Experimental/PhysScene_Chaos.cpp:1068–1070`, 물리 모듈 5.4절) 추정값(`EstimateCollisionResponse`)으로는 같은 값을 낼 수 없다. 그래서 읽기 함수를 추적 가능한 수정으로 더했다(`ThirdParty.json` `modifications` 3개 파일 + `Rocks.patch`).

- `PhysicsSystem::GetAppliedContactImpulse(body1, body2, outImpulseOnBody2, outNumContactPoints)`: body2가 body1에게서 받은 충격량(월드 벡터, kg·m/s)과 접촉점 수. 캐시 법선이 BodyID가 큰 body의 지역 좌표라 그 body를 lock해 현재 회전을 읽고, `ContactConstraintManager::GetAppliedContactImpulse`가 `WereBodiesInContact`와 같은 읽기 캐시(`mCache[mCacheWriteIdx ^ 1]`)에서 쌍의 manifold 사슬(`mFirstCachedManifold` → `mNextWithSameBodyPair`)마다 Σλ × 월드 법선을 더한다. 쌍이 없으면(두 body가 잠들어 캐시에서 빠진 때 포함) 거짓·0.
- 같은 파일의 캐시 재사용 경로(`GetContactsFromCache`)가 이번 스텝에 constraint를 만들지 않은 manifold(listener가 접촉 단위로 `mIsSensor`·질량 배율 0을 준 경우, constraint 버퍼가 찬 경우)의 복사된 충격량을 0으로 지운다. 새 manifold 경로는 원래 0이다. 지우지 않으면 적용되지 않은 이전 스텝 값이 보고된다. 부수 효과로 그런 manifold의 다음 warm start도 0에서 시작한다.
- 시점: `StoreAppliedImpulses`는 쓰기 캐시에 쓰고, step 끝의 `FinalizeContactCacheAndCallContactPointRemovedCallbacks`가 두 캐시를 바꾸므로 `Update`가 끝난 뒤 읽기 캐시가 그 step의 값이다. `Update` 중(contact callback 안)에는 이전 step 값이라 부르지 않는다. 시간 간격 0인 `Update`는 step하지 않아 이전 값이 남는다.
- 한계
  - `Update` 한 번에 collision step이 둘 이상이면 마지막 collision step의 값이다(Rocks는 고정 스텝마다 collision step 1 — 물리 모듈 3.1절).
  - CCD(`LinearCast`)가 `JobResolveCCDContacts`에서 직접 준 충격량은 캐시에 남지 않아 들어 있지 않다. CCD 접촉 기록은 contact listener가 있을 때만 만들어지고 body 쌍 사슬에 이어지지 않으므로, CCD로만 닿은 쌍은 함수가 거짓이다. 같은 스텝에 같은 쌍이 discrete로도 닿았으면 함수는 참이고 CCD 몫만 빠진다(listener는 두 경우를 구분하지 못한다 — CCD도 같은 `OnContactAdded`를 부른다). 넣으려면 `JobResolveCCDContacts`의 lambda를 캐시 밖 스텝별 배열에 모아야 한다(그 job은 캐시를 바꾸는 job과 동시에 돈다). 지금은 한계로 둔다.
  - listener가 질량 배율을 1이 아닌 값으로 주면 lambda는 물리적 운동량 변화가 아니다(Rocks는 배율을 쓰지 않는다).
  - 법선은 캐시 법선을 step 뒤 회전으로 되돌린 것이라 그 스텝 회전량(ω·dt)만큼 어긋날 수 있다(Jolt가 다음 스텝에 캐시를 다시 쓸 때와 같은 계산).
- 시험(연결 시험 파일, Windows x64 Debug 통과 2026-11-25)
  - `ThirdParty_JoltReportsAppliedContactImpulse`: 잠듦을 끄고 10 m에서 떨어뜨린 구(접촉점 1)와 정육면체(면 접촉 4점) 각각 330 스텝 모두에서 받은 충격량 수직 성분이 m·(v후 − v전 + g·dt)와 0.1 N·s 안에서 같고 수평 성분이 수직 크기의 10⁻⁴ 안이다. 첫 접촉이 실제로 파고드는 스텝이거나 그 한 스텝 전이고 그 뒤 끊기지 않으며, 충돌 스텝이 들어 있고(최대값 > m·1 m/s), 정지 뒤 m·g·dt이며, 순서를 바꿔 물으면 부호만 반대다. collision step 2로 정지시킨 구는 m·g·dt/2(마지막 collision step 값)다.
  - 다루지 않은 것: dynamic 대 dynamic, CCD, LargeIslandSplitter(큰 island 분할 경로 — 리뷰에서 원본으로 확인만 했다). 물리 모듈 PH 단계 시험에서 다룬다.
- 판을 올릴 때: upstream에 같은 기능이 생기면 수정을 지우고 그 API로 바꾼다.

## 11. 다른 구현 대조: UnrealJolt

UE 플러그인으로 Jolt를 붙인 공개 구현 UnrealJolt(commit `369ef176e166383ec2ac1659e4419243b0fa2922`, MIT, 약 4,200줄, 2026-11-25)를 이 문서와 [물리 모듈](PhysicsModule/README.ko.md) 설계에 대조했다. 소스를 읽어 확인했고 실행하지 않았다. 코드는 가져오지 않는다. 근거로만 쓰고, 우리 쪽 판정은 9절 수용 시험과 물리 모듈 시험으로 한다. 경로는 플러그인의 `Source/UnrealJolt/` 기준이다.

### 11.1 좌표 사상 — 거울 사상(행렬식 −1)

UnrealJolt는 2절에서 버린 거울 사상을 쓴다: 위치·벡터 `j = (x, z, y) · 0.01`(`Helpers.h:49`·`:71`), 회전 `(−qx, −qz, −qy, qw)`(`:81`).

- 극성 벡터(위치·선속도·힘·충격량)는 맞다. 회전 quaternion도 맞다(거울 사상에서 회전축은 축성 벡터라 `−M·a`로 바뀐다).
- 삼각형 winding을 뒤집는다(`Private/JoltSubsystem.cpp:694`·`:1793`·`:1885`). heightfield는 표본을 `y·N + x`로 전치 없이 넣는다(`:1555–1670`). 둘 다 거울 사상에서는 맞다(UE X → Jolt X, UE Y → Jolt Z이므로 행·열이 그대로다).
- 고정 원점 이동(`ToJoltPos`의 `WorldOrigin`)이 있다. 큰 월드 판단(6절)과 같은 방향이다.

### 11.2 거울 사상에서 난 오류

축성 벡터를 따로 다루지 않아 아래가 틀렸다(읽어서 본 것 — UnrealJolt의 실행 확인은 아니다). 2절 결정(행렬식 +1만 써서 부호 처리를 없앤다)의 근거다.

| 값 | UnrealJolt | 맞는 변환(거울 사상) | Rocks(A) |
|---|---|---|---|
| 각속도 설정·읽기 | 벡터 변환 그대로 → cm 배율 0.01·100이 붙고 부호가 안 바뀜(`Private/JoltSubsystem.cpp:1969`·`:1974`·`:1982`) | 배율 없음, `−M·ω` | 배율 없음, 부호 그대로 재배치(3절) |
| 토크 | 벡터 변환 그대로 → 배율 0.01, 부호 그대로(`:2241`) | 배율 10⁻⁴(kg·cm²/s² → N·m), `−M·τ` | 배율 10⁻⁴, 재배치만(3절·K1) |

단위 배율 오류는 사상 종류와 무관하다. PA02(되읽기 포함)·PA05(크기 판정)로 막는다.

### 11.3 층·필터

- UnrealJolt는 사용자가 이름 붙인 object 층과 broadphase 층을 설정에 두고, 층마다 `CollidesWith` 목록으로 대칭 표를 만든다(양방향 OR — `Private/JoltLayerTable.cpp:25–70`). 필터 셋은 이 표만 본다(`Public/JoltFilters.h`).
- UE 채널·응답(Block·Overlap·Ignore, 컴포넌트마다 다른 응답, 양쪽 최소)은 쓰지 않는다. 그래서 같은 층 쌍이 컴포넌트에 따라 다르게 반응할 수 없다.
- Rocks는 UE 모델을 그대로 둔다: object 층 bit에 채널·운동 묶음·참여 표지·응답 묶음 색인을 넣고 필터가 응답 묶음을 읽어 최소를 낸다(물리 모듈 6.1·6.2절). 표 기반 필터가 Jolt에서 문제없이 돈다는 점만 같다.

### 11.4 접촉 충격량(PMU2)

- `OnContactAdded`에서 `EstimateCollisionResponse`로 접촉점마다 충격량을 추정하고 ×100(N·s → kg·cm/s)해 MPSC 큐에 넣는다(`Private/JoltContactListener.cpp:30–60`). `OnContactPersisted`는 미끄러짐 효과용 접선 속도만 본다(`:63–`).
- 물리 모듈 PMD14(추정값을 `normalImpulse`에 넣는다)·listener는 값만 복사한다는 규칙(물리 모듈 3.2절)과 같은 방법이다. solver가 누적한 실제 충격량을 읽는 구현은 없다. Rocks는 Jolt 접촉 캐시를 읽는 수정(10.6절)으로 실제 충격량을 쓰고, 추정은 CCD로만 닿은 쌍에만 쓴다(PMU2 닫음).

### 11.5 kinematic 이동과 결과 반영

- kinematic body를 `SetPositionAndRotation`(teleport)으로만 옮기고 `MoveKinematic`을 쓰지 않는다(`Private/JoltSubsystem.cpp:2289–2297`). 이러면 kinematic body의 속도가 바뀌지 않아(기본 0) 밀린 dynamic body가 이동 속도를 받지 못하고 위치 보정으로만 밀려난다. Rocks는 UE처럼 `MoveKinematic`을 쓰고 목표 없는 스텝은 속도 0으로 둔다(물리 모듈 3.3절).
- 결과 반영은 60 Hz 누적기 뒤 body마다 직전·현재 transform을 기록하고, 남은 비율로 보간한 값을 Actor에 `TeleportPhysics`로 쓴다(`:370–445`). 그래서 게임 코드가 보는 transform은 물리 값보다 최대 한 스텝 늦다. Rocks는 결과 반영에 마지막 스텝 값을 그대로 쓰고 보간은 렌더 단계에서 따로 한다(물리 모듈 3.4절·3.2절 보간 — UE 기본 동기 모드와 같다).
- step은 subsystem Tick 안에서 동기로 돈다(`Private/JoltSubsystem.cpp:374–380` → `Private/JoltWorker.cpp:37–46`). Rocks도 동기 step이다(물리 모듈 3.2절). job은 Jolt 예제 `JobSystemThreadPool`을 쓰고 UE task 연결은 TODO로 남겨 두었다(`Private/JoltWorker.cpp` 생성자 주석). Rocks는 반입에서 그 파일을 빼고 Task 모듈에 연결한다(10.1절, 물리 모듈 6.7절).
