# 물리 모듈 — 7.1 단계별 구현 세부(PH6)

[목차](README.ko.md) · [7. 단계·시험·결정](07-StagesDecisions.ko.md) · [PH5](07c-StageDetailsPH5.ko.md)

7장 단계 표(7.1절)의 PH6 하위 단계 구현 세부다. 절 번호는 7장 그대로다.

### 7.1.20 PH6 하위 단계(2026-11-28)

PH6은 다섯으로 나눈다(크기 — "순서대로"). 각 하위 단계는 설계 갱신 → 구현 → 리뷰어 1명 → 커밋이다. 메시 로드 시스템(엔진 런타임 30.6절 — `StaticMeshComponent`가 메시 lease를 얻는 곳)이 아직 없으므로 메시에 기대지 않는 PH6a·PH6b를 먼저 한다.

| 하위 단계 | 내용 | 선행 |
|---|---|---|
| PH6a | 여러 형상 body(7.1.21절): World 형상 값을 원소 목록으로, 원소 종류 구·상자·캡슐·테이퍼드 캡슐, 원소 transform·`collisionEnabled`·`contributeToMass`, 원소별 UE scale 굽기, Jolt compound, 원소 충돌 켬 필터(질의·시뮬레이션·probe), `HitResult::elementIndex`, 컴포넌트 sweep 원소별(UE `ComponentSweepMulti`), `IsZeroExtent` 가상, 관성 주값, PA13 | PH5 |
| PH6b | weld(2.8절): 자식 형상을 부모 body compound에 더하기·빼기, 부착·떼기·상태 파괴, Kinematic 루트 규칙, 질의 결과의 원래 컴포넌트, 질량 다시 계산, 깨움 이벤트 전하기(7.1.19절), weld body 잠금은 부모 설정 | PH6a |
| PH6c | `StaticMeshComponent` 단순 충돌: 메시 로드 정보의 충돌 사본(엔진 런타임 30.3절 `ApplyLoadedMeshInfo` 확장), `SimpleCollision` → 원소, convex(점 → Jolt hull — 실행 중 만들기는 편집기·개발 빌드만, 계획 16.4절), `useDefaultCollision`·`complexity` Default·SimpleAndComplex·SimpleAsComplex | PH6a, 메시 로드 시스템(없으면 착수 때 런타임 세션에 요청) |
| PH6d | complex-as-simple·`traceComplex` 삼각형 메시(Jolt `MeshShape` — 정적·Kinematic만, 어댑터 경계 4절), MTD의 complex 하위 형상 거르기(7.1.14절 한계), 뒷면 모드(7.1.7절 한계), PA06·PA09(메시)·PA11 | PH6c |
| PH6e | 쿠킹 형상 너겟(계획 16.4절 cook — PC05: Jolt 판·schema·축 사상·단위 변환 판 불일치 거부), 장면 형상 공유(에셋 lease와 같은 수명 — 2.4절), 공유 형상 캐시 수명(7.1.1절 PH6 재검토) | PH6c·PH6d, 에셋 쿠킹 |

### 7.1.21 PH6a 구현 세부(2026-11-28)

UE는 모든 body가 `FKAggregateGeom` 원소들의 형상 묶음이다(shape 컴포넌트도 원소 하나짜리 BodySetup). Rocks도 컴포넌트 형상 값을 원소 목록으로 바꾸고, Physics가 원소마다 Jolt 형상을 만들어 묶는다.

**World 값(`World/Physics/BodyGeometry.h` 개정).**
- `BodyShapeElement`(UE `FKShapeElem` 대응, scale 전 cm·컴포넌트 공간): `kind`(`BodyGeometryKind` — Box·Sphere·Capsule·TaperedCapsule), `center`, `rotation`(`Quatd`), `halfExtent`(Box), `radius`(Sphere·Capsule, TaperedCapsule은 +Z 끝 — UE `Radius0`), `radius1`(TaperedCapsule −Z 끝 — UE `Radius1`), `halfHeight`(Capsule은 반구 포함 반높이 — UE `CapsuleHalfHeight` = `Length`/2 + `Radius`, TaperedCapsule은 (`Length` + `Radius0` + `Radius1`)/2 — UE `GetScaledHalfLength`의 scale 1 값), `collisionEnabled`(기본 QueryAndPhysics), `contributeToMass`(기본 참).
- `BodyGeometry`는 원소 배열(할당기 생성)이다. 원소 순서가 원소 번호(`elementIndex`)다. 메시(PH6c)는 UE 생성 순서(`E/Private/PhysicsEngine/Experimental/ChaosInterfaceUtils.cpp:137–293` — 구·상자·캡슐·테이퍼드 캡슐·convex)로 채운다.
- `PrimitiveComponent::BuildBodyGeometry(BodyGeometry&)`는 `Result<void>`를 돌려준다: 실패는 할당 오류(body를 만들지 않고 호출자가 진단), 성공에 원소 0개는 형상 없음(body 없음 — 지금과 같다). Box·Sphere·Capsule 컴포넌트는 중심 0·회전 없는 원소 하나를 더한다(UE shape 컴포넌트 BodySetup).
- `PrimitiveComponent::IsZeroExtent()` 가상(UE `UPrimitiveComponent::IsZeroExtent` — 기본 거짓, Box `BoxExtent.IsZero()`, Sphere 반경 0, Capsule 반경·반높이 0). 컴포넌트 sweep의 line trace 전환(UE `ComponentSweepMultiByChannel` — `E/Private/Collision/WorldCollision.cpp:500–504`)이 형상 값 대신 이것을 쓴다.

**원소 scale 굽기(UE `GetFinalScaled` — `E/Private/PhysicsEngine/BodySetup.cpp:2006–2290`).** 컴포넌트 월드 scale S(0인 축은 KindaSmallNumber — 지금 규칙)에 대해:
- 중심 = `center` × S(성분 곱, 부호 유지 — 거울 인스턴스는 중심이 뒤집힌다). 회전은 S에 음수 성분이 없으면 `rotation` 그대로다(UE quaternion 곱 경로). 음수 성분이 있으면 UE `FTransform::Multiply`의 행렬 경로(`ConstructTransformFromMatrixWithDesiredScale` — `Core/Public/Math/TransformNonVectorized.h:1277–1304`)처럼 축 i = σᵢ·normalize(S ⊙ R·eᵢ)(σ = scale 부호)를 행으로 둔 행렬의 회전이다(|S|가 고르면 정확한 거울 Σ·R·Σ).
- 구: 반경 × min |S|(최소 KindaSmallNumber). 상자: 반폭 × |S| 원소 축별(UE `X *= Scale3DAbs.X` — 원소가 회전해 있어도 원소 축에 곱한다, UE와 같은 근사)·최소 KindaSmallNumber, margin은 지금 상자 규칙. 캡슐: 지금 규칙(반높이 × |Sz| 최소 0.1, 반경 × max(|Sx|, |Sy|)를 [0.1, 반높이], 원통 반길이 최소 0.05).
- 테이퍼드 캡슐(UE `FKTaperedCapsuleElem::GetScaledRadii`·`GetScaledCylinderLength` — `:2249–2290`): 반높이 h = `halfHeight` × |Sz|(최소 0.1), r0·r1 = 반경 × max(|Sx|, |Sy|)를 [0.1, h]로, 선분 길이 L = max(0.1, 2h − (r0 + r1)). Jolt `TaperedCapsuleShapeSettings`(반높이 L/2, top = r0, bottom = r1 — 축 A로 +Z → +Y, 어댑터 경계 4절 J20)를 쓴다: |r0 − r1| ≥ L이면(한 구가 다른 구 안) 큰 구 하나를 그 끝 중심에, r0 = r1이면 캡슐이다(Jolt `Create`와 같은 판정). Jolt 테이퍼드 캡슐 형상 원점은 선분 중점이다(두 끝 중심 `mTopCenter`·`mBottomCenter`는 질량 중심 공간 값이고 `GetCenterOfMass`가 그만큼 옮긴다 — `TaperedCapsuleShape.cpp:123–124`) — 원점 보정이 없다. 반경을 반높이로 자르면 선분 길이가 줄어 두 끝 반높이가 다를 수 있다(UE 식 그대로 — UE는 평균 반경이라 드러나지 않는다). **UE와 다른 점:** UE 5.8은 두 반경 평균의 일반 캡슐로 바꾼다(`ChaosInterfaceUtils.cpp:197–221`). 어댑터 경계 4절 결정대로 모양 그대로 쓴다. Sz < 0이면 r0·r1을 바꾼다(거울 회전의 +Z는 σz·S∘R·ẑ라 원래 +Z 끝이 −Z로 간다 — UE는 평균이라 드러나지 않는 차이, Rocks는 기하 그대로).
- PA13(테이퍼드 캡슐 — 어댑터 경계 9절)을 이 경로로 다시 돌린다: 두 끝 지지점(+Z 방향 끝점 = 중심 + (L/2 + r0)·축)·거울 scale.

**Jolt 형상.**
- 원소 하나이고 중심 0·회전 없음 → 그 형상 그대로(지금 경로와 같은 Jolt 형상). 원소 하나에 이동·회전 → `RotatedTranslatedShape`. 둘 이상 → `StaticCompoundShape`(Jolt `StaticCompoundShapeSettings::Create`와 같은 선택 — `StaticCompoundShape.cpp:28–60`). 자식 위치는 A·(cm → m), 회전은 (A·v, w)(어댑터 경계 4절 compound 규칙).
- 원소 형상(잎)마다 user data = 표지 bit | 원소 충돌 켬 | 원소 번호다(질의·시뮬레이션 필터와 hit 원소 번호가 읽는다). compound·감싸기 형상은 user data 0(표지 없음 — 필터 통과)이다. 형상을 body끼리 공유하지 않으므로(PH6e 전) 잎 user data가 body마다 독립이다.
- 질량: 잎 밀도 = 재질 밀도, `contributeToMass` 거짓이면 0(Jolt compound 질량·질량 중심은 자식 질량 가중 — 어댑터 경계 4절 J21). 원시 질량이 0이면(모든 원소가 빠짐) UE 대체값(지름 50 cm 구·1 g/cm³: 질량 523 kg, 관성 대각 5.24e5 kg·cm², 질량 중심 0 — `CH/Private/Chaos/MassProperties.cpp:474–483`)에 7.1.15절 2–5를 그대로 적용한다. 질량 중심 이동 감싸기(`OffsetCenterOfMassShape`)는 compound 바깥이다.
- 비유한 원소 값·원소 수 > `BodyGeometryMaxElements`(65535 — 메시 `CollisionMaxElements`와 같은 값)는 body를 만들지 않는다(`InvalidArgument`). Jolt 형상 생성 실패(compound 깊이·테이퍼드 캡슐 결과 오류)도 그 오류로 같다. scale 변경의 다시 만들기가 실패하면 이전 형상을 두고 Warning `BodyShapeRebuildFailed`다.
- 질량에 넣는 원소가 없으면 질량 중심을 body 원점에 둔다(원소 하나·compound 아닌 형상은 Jolt가 그 원소의 질량 중심을 두므로 `OffsetCenterOfMassShape`로 되돌린다).
- 원소의 충돌 켬이 서로 다른 body는 Jolt manifold reduction을 끈다: 합친 manifold는 첫 하위 형상 ID 하나만 남겨(`Body.h:88–91`) 이음매에 걸친 접촉의 원소별 probe 판정이 섞인다. 형상 교체 때 다시 맞춘다.

**원소 충돌 켬(UE `NewShapeHelper` — `ChaosInterfaceUtils.cpp:103–133`).** 원소의 실효 값 = `CollisionEnabledIntersection(body 실효 충돌 켬, 원소 값)`이다. 시뮬레이션 = 실효에 Physics 또는 Probe, 질의 = Query, probe = Probe(UE `bEnableSim |= bEnableProbe`). body 값은 object 층 참여 bit에서 다시 만든다(body 충돌 변경이 형상을 다시 만들지 않게).
- 질의: 장면 질의(trace·sweep·overlap)에 Jolt `ShapeFilter`를 건다 — 대상 잎(`shape2`)의 실효 질의가 꺼져 있으면 거른다(UE 질의는 `QueryEnabled` 형상만). 질의 쪽 형상(`shape1`)은 보지 않는다.
- 시뮬레이션: Jolt `PhysicsSystem::SetSimShapeFilter`(`SimShapeFilter.h`) — 두 잎 중 하나라도 실효 시뮬레이션이 꺼져 있으면 접촉을 만들지 않는다. probe: 접촉 콜백이 두 잎의 실효 probe를 보고 body probe와 같이 다룬다(밀지 않음·실제 겹침만 — 6.4절).
- body 전체 값(층)은 지금처럼 broadphase에서 거른다. 원소 필터는 그 뒤 잎 단계다.

**질의 결과 원소 번호.** `HitResult::elementIndex`(UE `ElementIndex` — `CollisionConversions.cpp:148–150`, 기본 0 — UE `FHitResult::Init`의 0 채움)는 hit 잎의 원소 번호다(Jolt `Shape::GetSubShapeUserData`). UE는 uint8(255 미만 check)이고 Rocks는 `int32`다(원소 상한 65535 — `CollisionMaxElements`). hit 이벤트(접촉 — 5.4절)는 UE처럼 채우지 않는다(0).

**컴포넌트 질의(UE `ComponentSweepMultiByChannel` — `WorldCollision.cpp:485–570`, `FBodyInstance::OverlapMulti` — `BodyInstance.cpp:4381–4462`).**
- `TraceBody`(컴포넌트 sweep)는 원소마다 그 잎 형상을 (body 회전 · 원소 위치·회전)으로 따로 쓸고 결과를 이어 붙인 뒤 시간순으로 정렬한다(UE: 형상마다 `GeomSweepMulti`, `OutHits.Append`, `Sort(FCompareFHitResultTime)`). 같은 시간은 원소 순서(안정 정렬 — 결정성, UE `Sort`는 불안정)다. 반환은 어느 원소라도 막는 hit이 있으면 참이다. UE처럼 원소 질의 켬을 보지 않는다(UE는 자기 형상을 모두 쓴다 — convex 아닌 형상만 뺀다, PH6d). 같은 컴포넌트 hit이 원소마다 따로 남을 수 있다(UE `todo: should these be made unique?` — 같다).
- impactNormal 규칙(7.1.8절)의 질의 형상 종류는 쓸는 원소의 종류다(테이퍼드 캡슐은 Capsule — UE Chaos가 캡슐로 만들기 때문).
- `OverlapBody`(컴포넌트 overlap)는 body 형상 전체로 한 번 질의한다(지금 경로 — Jolt가 하위 형상마다 판정하고 수집기가 컴포넌트마다 합친다). UE는 형상마다 결과를 이어 붙여 같은 컴포넌트가 여러 번 나오고, 소비자(`UpdateOverlaps`의 `AddUniqueOverlapFast` 등)가 합친다 — Rocks는 합친 결과를 낸다(차이 — 공개 overlap 결과의 중복만 다르다).
- MTD(`ComputePenetration`)는 지금처럼 body 형상 전체이고 원소 질의 켬을 보지 않는다(UE `Overlap_Geom`은 대상 body의 단순 형상을 모두 쓴다 — `PC/Private/PhysInterface_Chaos.cpp:1137–1146`). complex 거르기는 PH6d다.

**관성 주값.** `GetInertiaTensor`(7.1.18절)는 UE `GetLocalInertiaTensor`(Chaos `I()` — 주축 공간)와 같게: 관성(Rocks 축, kg·cm²)의 비대각² 합 / (대각합/3)² < `SMALL_NUMBER`(1e-8)면 대각 그대로, 아니면 Chaos `TransformToLocalSpace`(`CH/Private/Chaos/MassProperties.cpp:17–48`)의 고유값 식(삼각함수 닫힌 꼴)으로 (m00, m11, m22)다 — UE 식 순서 그대로 m00 최대·m11 최소·m22 가운데(각이 [0, π/3]). 대각합/3 ≤ `SMALL_NUMBER`면 대각 그대로다. 판정은 UE와 같은 kg·cm² 값으로 한다.

**질량 변경.** 칸은 원소 묶음(감싸기 전)과 원소 잎 형상을 쥔다. 질량 setter(7.1.18절)는 원소 묶음에 질량 중심 감싸기만 다시 씌우므로 형상을 다시 만들지 않는다(실패 경로가 없어 Warning `BodyMassUpdateFailed`를 뺐다). scale·형상 값 변경은 원소 값·잎 형상·묶음을 모두 새로 만든 뒤 한 번에 바꾼다(실패하면 이전 것을 둔다).

**한계·나중.**
- 원소 종류 Cylinder(CY2)·Convex(PH6c)는 이 단계에 없다(`BodyGeometryKind`에 없음 — 들어오면 원소를 만들지 않고 `InvalidArgument`).
- 원소 `restOffset`은 받지 않는다(어댑터 경계 4절 — Jolt 대응 없음).
- 형상 공유는 PH6e다(지금은 body마다 만든다).

**시험.**
- `Physics/Tests/PhysicsCompoundSceneTests.cpp`(Monolithic — 장면 직접): 원소 둘(상자 + 구, 위치·회전) 질량·질량 중심 손계산(Jolt 균일 밀도 → 7.1.15절), `contributeToMass` 거짓 원소 제외, 모두 제외 대체값, 비균일 scale 원소별 굽기(회전한 상자 원소 축, 캡슐·구 규칙), 거울 scale 중심, 원소 충돌 켬(질의 전용 원소는 시뮬레이션 접촉 없음·질의 hit 있음, 물리 전용 원소는 질의 hit 없음·접촉 있음, probe 원소는 밀지 않음), `elementIndex`, 관성 주값(대각선 위 두 구 — UE 고유값 식과 순서), 원소 충돌 켬이 섞인 body의 manifold reduction 끔·형상 교체 때 다시 맞춤, probe 원소의 접촉 통지, `SetBodyGeometry` compound 다시 만들기, 회전한 원소의 거울 scale(UE 거울 회전), 원소 하나짜리 대체 질량 중심.
- PA13(원소 경로 — `PhysicsCompoundSceneTests`): 끝점(ray로 +Z·−Z 끝까지 거리), 선분 중점 = 원소 중심, 반경을 반높이로 자른 한 구 포함, 같은 반경 캡슐, 거울 scale 반경 교환.
- 컴포넌트 경로(`Physics/Tests/PhysicsCompoundTests.cpp`): 원소 둘을 주는 시험 native 컴포넌트로 body 생성·컴포넌트 sweep 원소별(두 원소가 다른 시간에 닿음 — 시간순·원소 순서), `IsZeroExtent`(Box·Sphere·Capsule 값과, 크기 0 컴포넌트 sweep이 line trace라 같은 Actor 형제를 맞힘), scale 변경의 compound 다시 만들기.
