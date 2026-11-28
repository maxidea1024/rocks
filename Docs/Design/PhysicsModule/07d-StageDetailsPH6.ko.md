# 물리 모듈 — 7.1 단계별 구현 세부(PH6)

[목차](README.ko.md) · [7. 단계·시험·결정](07-StagesDecisions.ko.md) · [PH5](07c-StageDetailsPH5.ko.md)

7장 단계 표(7.1절)의 PH6 하위 단계 구현 세부다. 절 번호는 7장 그대로다.

### 7.1.20 PH6 하위 단계(2026-11-28)

PH6은 일곱으로 나눈다(크기 — "순서대로", PH6b는 셋 — 2026-11-28). 각 하위 단계는 설계 갱신 → 구현 → 리뷰어 1명 → 커밋이다. 메시 로드 시스템(엔진 런타임 30.6절 — `StaticMeshComponent`가 메시 lease를 얻는 곳)이 아직 없으므로 메시에 기대지 않는 PH6a·PH6b를 먼저 한다.

| 하위 단계 | 내용 | 선행 |
|---|---|---|
| PH6a | 여러 형상 body(7.1.21절): World 형상 값을 원소 목록으로, 원소 종류 구·상자·캡슐·테이퍼드 캡슐, 원소 transform·`collisionEnabled`·`contributeToMass`, 원소별 UE scale 굽기, Jolt compound, 원소 충돌 켬 필터(질의·시뮬레이션·probe), `HitResult::elementIndex`, 컴포넌트 sweep 원소별(UE `ComponentSweepMulti`), `IsZeroExtent` 가상, 관성 주값, PA13 | PH5 |
| PH6b1 | weld 장면 쪽(7.1.22절): `WeldBody`·`UnweldBody`, body 부분(part) 표, 부분 원소를 루트 공간으로 굽기, 잎 user data의 부분 번호, 질의 hit·접촉 hit의 원래 컴포넌트, 부분별 무시 목록, 컴포넌트 sweep 제외 Actor(모든 부분), 부분별 overlap(UE `IsShapeBoundToBody`), 질량 다시 계산 | PH6a |
| PH6b2 | weld 컴포넌트 쪽(2.8절): `autoWeld` 상태 생성, 부착 규칙 `weldSimulatedBodies`, 떼기·상태 파괴의 unweld, 루트 시뮬레이션 전환의 `ApplyWeldOnChildren`, 직접 이동·형상 변경의 다시 weld, Kinematic 루트 규칙, 힘·속도·질량·잠듦·body 값 API를 루트로(UE `GetBodyInstance()` 기본 weld), 깨움 이벤트 전하기(7.1.19절) | PH6b1 |
| PH6b3 | weld 부분별 object type·응답·접촉 알림(UE `UpdatePhysicsFilterData`가 원래 body 값으로 형상마다 필터를 만든다) | PH6b2 |
| PH6c | `StaticMeshComponent` 단순 충돌: 메시 로드 정보의 충돌 사본(엔진 런타임 30.3절 `ApplyLoadedMeshInfo` 확장), `SimpleCollision` → 원소, convex(점 → Jolt hull — 실행 중 만들기는 편집기·개발 빌드만, 계획 16.4절), `useDefaultCollision`·`complexity` Default·SimpleAndComplex·SimpleAsComplex | PH6b3, 메시 로드 시스템(없으면 착수 때 런타임 세션에 요청) |
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

### 7.1.22 PH6b1 구현 세부(2026-11-28)

UE는 weld한 자식 body의 형상을 루트 입자에 더하고(`FBodyInstance::Weld` — `E/Private/PhysicsEngine/BodyInstance.cpp:1940–2033`), 형상마다 원래 body를 `ShapeToBodiesMap`(`FWeldInfo` — 자식 body·상대 transform)으로 기억한다. Rocks 장면은 body 칸에 부분(part) 표를 둔다.

**장면 API(`PhysicsScene`).**
- `BodyWeldDesc`(World): `component`·`actor`(자식), `geometry`(자식 원소 — 자식 컴포넌트 공간, scale 전), `relative`(자식 월드 transform의 루트 월드 기준 상대 — scale 포함, UE `TheirTM.GetRelativeTransform(MyTM)`), `collisionEnabled`(자식 실효 충돌 켬), `settings`(자식 body 값 — 질량 칸, nullptr이면 기본값).
- `WeldBody(root, desc)`: 루트 body에 부분을 더하고 형상을 다시 만든다(UE `Weld` → `PostShapeChange` → `UpdateMassProperties` — 질량은 아래 "질량"대로 다시 계산). 같은 컴포넌트가 이미 부분이면 relative·형상을 바꾼다(UE `SetWeldedBodyTransform` = `UnWeld` + `Weld`). 깨움: UE `Weld`는 깨우지 않는다 — 형상 교체만이다(Rocks도 `SetShape`를 깨우지 않게 부른다). 오류: 없는 handle(StaleHandle), 원소 없음·비유한 값·원소 수 합 > `BodyGeometryMaxElements`·부분 수 > `BodyWeldMaxParts`(InvalidArgument), 할당 실패. 실패하면 이전 형상·부분을 둔다.
- `UnweldBody(root, component)`: 그 부분을 빼고 형상을 다시 만든다(UE `UnWeld` → `PostShapeChange`). 없는 부분·handle은 무시한다.
- 루트 body를 지우면 부분도 함께 사라진다(UE는 루트 `OnDestroyPhysicsState`가 먼저 `UnWeldChildren` — 컴포넌트 쪽 PH6b2).

**부분 굽기(UE `AddShapesToRigidActor_AssumesLocked(this, Scale3D, …, RelativeTM)` — `GetFinalScaled(Scale3D, RelativeTM)`).** 루트 scale S, 부분 relative R(위치 t, 회전 q, scale s)에 대해 부분 원소는:
- 크기: S ⊙ s(원소 크기 규칙의 scale — UE `SetupNonUniformHelper(Scale3D * RelativeTM.GetScale3D())`).
- 중심: (R.TransformPosition(center)) ⊙ S(UE `RelativeTM.TransformPosition(Center) * Scale3D`).
- 회전: 거울(7.1.21절 `ScaledElementRotation`)을 둘로 나눠 적용한다 — relative scale s의 거울은 원소 축에, 루트 scale S의 거울은 relative 회전 뒤(루트 축)에: Mirror(q · Mirror(r, s), S)(UE `FKBoxElem`: `GetTransform() * RelativeTM * ScaleTransform(Scale3D)` — `BodySetup.cpp:2048–2064`). UE 캡슐·테이퍼드 캡슐은 `RelativeTM.GetRotation() * Mirror(r, Scale3D)`로 다르게 셈한다(`:2144–2165`·`:2249–2270`) — Rocks는 모든 원소에 상자 규칙(기하적으로 맞는 거울)을 쓴다(UE와 다른 점, 거울 루트 scale과 relative 회전이 함께 있을 때만 드러난다). 테이퍼드 캡슐 두 반경은 (S ⊙ s).z < 0이면 바꾼다.
- 원소 번호는 부분 순서(루트 0, weld 순서)로 이어 붙인 번호다(UE `ShapeIndex` — 루트 입자의 형상 배열 번호).
- 부분 원소의 충돌 켬은 `CollisionEnabledIntersection(자식 실효 충돌 켬, 원소 값)`로 굽는다(UE가 원래 body 값으로 필터를 만드는 것의 켬 몫 — 부분별 object type·응답은 PH6b3). 필터 때 루트 body 값(층)과 한 번 더 교집합한다(루트 body의 참여가 모든 부분을 가른다 — 한계).

**질량(UE `UpdateMassProperties` — `BodyInstance.cpp:3422–3480`).** 부분이 둘 이상이면 원래 body(부분)마다 그 잎만으로 7.1.15절 질량 규칙(재질 밀도·`RaiseMassToPower`·`massScale`·덮어쓰기·관성 배율·질량 중심 이동)을 따로 적용한다. 질량 중심 이동은 부분의 `MassModifierTransform`(relative 회전 · (relative scale ⊙ 루트 scale))으로 루트 공간에 옮긴다(UE `MassModifierTransform.TransformVector(COMNudge)`). 그다음 Chaos `CombineWorldSpace`(`CH/Private/Chaos/MassProperties.cpp:281–331`)처럼 질량을 더하고, 질량 가중 질량 중심에 평행축 정리로 관성을 합친다. 질량에 넣는 원소가 없는 부분은 빠진다(UE는 그 형상을 먼저 걸러 batch가 없다). 부분이 모두 빠지거나 루트만이면 7.1.21절(원시 질량 = 원소 묶음 값, 0이면 UE 대체값)이다. body 형상의 질량 중심은 합친 질량 중심으로 옮긴다(`OffsetCenterOfMassShape` — inner 질량 중심과의 차, 0이면 감싸지 않는다). 루트 질량 setter(7.1.18절)도 같은 계산이다.

**잎 user data.** 표지 bit | 부분 번호(bit 40–55) | 충돌 켬(bit 32–39) | 원소 번호(bit 0–31)다. 부분 수 상한 `BodyWeldMaxParts` = 65535(16 bit).

**질의.**
- hit의 `component`·`actor`는 잎 부분의 컴포넌트·Actor다(UE `ShapeToOriginalBodyInstance` — `CollisionConversions.cpp:161`). 질의 수집 기록에 부분 번호를 둔다: overlap 합치기(같은 컴포넌트 하나)는 (칸, 부분)마다, 같은 거리 순서는 (칸, 부분) 순이다.
- 무시 목록(`ignoreComponents`·`ignoreActors`)과 컴포넌트 질의의 제외는 부분마다 본다(UE 형상 filter data의 owner id가 원래 body 것 — `ShapeFilter`가 `mBodyID2`의 칸·잎 부분으로 판정). 부분이 하나뿐인 body는 지금처럼 body 필터에서 거른다.
- 컴포넌트 sweep(`TraceBody`)은 루트의 모든 부분 원소를 쓸고, 모든 부분의 Actor를 뺀다(UE "All actors pointed to by shapes should be ignored (This deals with welded Actors)" — `WorldCollision.cpp:531–543`).
- 컴포넌트 overlap(`OverlapBody`)은 인자 `part`(null = 루트 자신)의 원소만 쓴다(UE `IsShapeBoundToBody` — `BodyInstance.cpp:4440`). 원소마다 질의하고 (칸, 부분)마다 합친다(막음 OR). 자세는 그 부분 컴포넌트 기준이다: 원소 자리는 루트 공간 자리에 부분 원점(relative 이동 ⊙ 루트 scale)·relative 회전의 역을 적용한 것으로 따로 기억한다(같은 형상 — UE `RootSpaceToWeldedSpace`, `BodyInstance.cpp:4358–4376`).
- MTD(`ComputePenetration`)는 대상 body의 모든 원소다(7.1.21절 그대로).

**접촉 hit.** body 쌍마다 하나다(UE `GetPendingCollisionForContactPair` — `PhysScene_Chaos.cpp:1065`, 충격량은 쌍의 합). 컴포넌트·Actor는 묶음의 첫 접촉 원소(정렬 — 부분 쌍 순)의 부분 것이다(UE `GetBodyInstanceFromProxyAndShape` — `:1110–1114`). 알림 표지는 body 값이다(부분별 표지는 PH6b3). 컴포넌트 sweep은 자기 body를 통째로 뺀다(Actor 없는 부분도).

**결과 반영·깨움.** 루트 body만 결과 목록에 있다(weld 자식은 부착으로 따라간다). 깨움 이벤트 전하기는 컴포넌트 쪽(PH6b2 — UE `DispatchWakeEvents`가 weld 자식에 재귀).

**시험(`PhysicsWeldSceneTests.cpp` — Monolithic).** 루트(구) + weld 자식(위 상자, relative 위치·회전·scale) 형상·질량(부분마다 UE 규칙 뒤 합 — UnrealMass(구) + UnrealMass(상자))·질량 중심(수정한 질량 가중)·자식 질량 중심 이동, 셋 이상 부분의 가운데 unweld·다시 weld 뒤 범위·원소 번호, weld 루트의 `SetBodyGeometry`·scale 변경이 부분을 지킴, relative 회전 굽기와 부분 overlap, 부분별 무시 목록(shape sweep·overlap), 잠든 body weld는 깨우지 않음, 접촉 hit는 body 쌍마다 하나, 루트 scale과 relative scale의 곱(원소 크기)·중심 굽기, hit 컴포넌트·원소 번호(루트 원소 다음), 부분별 무시 목록(자식 무시 → 루트 hit만), overlap 부분별(루트 질의는 자식 원소를 안 씀)·(칸, 부분) 합치기, 컴포넌트 sweep의 부분 Actor 제외, 접촉 hit 컴포넌트, `UnweldBody` 뒤 원래 형상·질량, 같은 자식 다시 weld(relative 변경), 실패 원자성(비유한 relative).

### 7.1.23 PH6b2 구현 세부(2026-11-28)

컴포넌트 쪽 weld다(UE `UPrimitiveComponent::WeldToImplementation`·`UnWeldFromParent`·`UnWeldChildren` — `E/Private/PrimitiveComponentPhysics.cpp:1040–1252`, `FBodyInstance::ApplyWeldOnChildren` — `BodyInstance.cpp:2641–2665`). PH6b1 장면 API를 부른다.

**상태.** `PrimitiveComponent`에 `weldRoot_`(내 원소를 가진 루트 컴포넌트 — null이면 weld 아님)와 `weldBody_`(그 루트 body handle)를 둔다. weld된 컴포넌트는 물리 시스템 상태가 있고(`HasSystemState(Physics)`) 자기 body가 없다(`BodyHandle()` null — UE `IsValidBodyInstance` 거짓, `WeldParent` 있음). 조회 `IsWelded()`·`WeldRoot()`, 물리 body `PhysicsBody()`(weld면 루트 body — UE `GetBodyInstance()` 기본 `bGetWelded`). `SetAutoWeld`·`GetAutoWeld`(UE `bAutoWeld` — 다음 물리 상태 생성부터), `WeldTo(parent, socket, weldToKinematicParent)`(UE `WeldTo` — 붙어 있지 않으면 KeepWorld로 붙인다), `UnWeldFromParent()`·`UnWeldChildren()`.

**루트 찾기(UE `GetRootWelded` — `:960–998`).** 부모부터 위로 primitive 컴포넌트를 따라가며, 자기 body가 없거나 weld된 것은 지나치고 처음 만난 자기 body 있는 것이 루트다. 부모가 primitive가 아니면 멈춘다(루트 없음).

**`WeldToImplementation(parent, weldSimulatedChild, weldToKinematicParent)`(UE `:1040–1120`).** 부모가 지금 부모가 아니면 거짓. 자기가 시뮬레이션하고 `weldSimulatedChild`가 거짓이면 거짓. 실효 충돌 켬이 QueryOnly·NoCollision이면 거짓. 먼저 `UnWeldFromParent()`. 루트가 없거나 body가 없으면 거짓. 이미 그 루트에 weld면 참. 루트가 Dynamic이 아니고 `weldToKinematicParent`가 거짓이면 `SetSimulatePhysics(false)` 후 거짓(UE — 자기 body로 계속). 그 밖은 `WeldBody(루트 body, {자기, 소유 Actor, 형상, relative = 자기 body 월드 transform의 루트 body 월드 기준 상대, 실효 충돌 켬, bodySettings})`다. 성공하면 내 body에 weld돼 있던 자손을 새 루트로 옮기고(UE `Weld` — 상대의 `ShapeToBodiesMap`을 다시 weld, `BodyInstance.cpp:1990–2006`) 내 body를 지운 뒤 참이다. 실패하면(형상 없음·`WeldBody` 오류) Warning `WeldFailed`와 — weld가 아니고 body가 없으면 — 자기 body를 만들고 거짓이다(body 없이 두지 않는다). relative는 UE `GetRelativeTransform`의 scale 나눗셈이다 — **UE와 다른 점:** 음수 scale의 UE 행렬 경로(`GetRelativeTransformUsingMatrixWithScale`)는 따르지 않고, 0 scale 축은 UE처럼 0이 아니라 ±KindaSmallNumber로 나눈다.

**자동 weld(UE `InitBody` — `BodyInstance.cpp:1525–1541`).** 물리 상태를 만들 때(`CreateSystemState(Physics)`) `autoWeld`·게임 World(Game·Tryout)·body 운동 종류가 Static이 아님(UE `IsStaticPhysics` — Movable만)·실효 충돌 켬이 QueryOnly가 아님·부모가 primitive면 `WeldToImplementation(부모, false, false)`를 먼저 해 보고, 참이면 자기 body를 만들지 않는다. 등록 순서상 부모 body가 아직 없으면 weld하지 않는다(UE와 같다 — 루트가 뒤에 시뮬레이션으로 바뀌면 `ApplyWeldOnChildren`이 붙인다).

**부착·떼기(UE `AttachToComponent` — `SceneComponent.cpp:2625–2633`, `DetachFromComponent` — `:2683–2686`).** `AttachmentRules`에 `weldSimulatedBodies`(기본 거짓 — UE C++ 규칙 상수와 같다)를 둔다. `SceneComponent`에 hook 둘을 더한다: `OnDetachingFromParent()`(부모에서 떼기 직전 — `Detach`, 다른 부모·socket에 붙이기의 링크 검증 뒤, 파괴 떼기 `DetachForDestroy`의 자기와 고아가 될 자식; 같은 부모·socket에 다시 붙이면 부르지 않는다 — UE "already attached"), `OnAttachedToParent(rules)`(붙인 뒤 transform 통지 다음). Primitive는 앞에서 떼는 부분 트리 안의 weld 가운데 루트가 그 밖인 것을 모두 푼다(자기가 weld가 아니어도 — body 없는 이 컴포넌트를 거친 자손), 뒤에서 `weldSimulatedBodies`이고 물리 상태가 있으면 `WeldToImplementation(부모, true, false)`를 한다. weld된 컴포넌트를 다시 합칠 때 루트가 더는 조상이 아니면 푼다.

**`UnWeldFromParent()`(UE `:1134–1238`).** weld가 아니면 끝. 루트 body에서 내 부분을 뺀다(`UnweldBody`). 내 물리 상태가 있고 파괴 대기·World 정리 중이 아니면 자기 body를 만든다(UE `InitBody` — UE는 자기가 무효·`GExitPurge`면 바로 끝낸다). 내 부분 트리 가운데 같은 루트에 weld된 것(UE `GetWeldedBodies` — body 없는 중간 컴포넌트를 거친 자손도)도 그 루트에서 빼고 자기 body를 만든다 — **UE와 다른 점:** UE는 이때 그 자손 body를 다시 만들지 않아(내가 시뮬레이션하지 않으면) body 없는 컴포넌트가 남는다. 그다음 내가 Dynamic이면 `ApplyWeldOnChildren()`이다.

**`UnWeldChildren()`·상태 파괴(UE `OnDestroyPhysicsState` — `PrimitiveComponent.cpp:1144–1161`).** `UnWeldChildren()`은 UE처럼 직접 자식마다 `UnWeldFromParent()`다. 물리 상태를 지울 때는 `UnWeldFromParent()`(자기 body는 만들지 않는다) 뒤 내 body에 weld된 자손을 부분 트리 전체에서 푼다(자기 body를 다시 만든다 — UE는 직접 자식만이라 body 없는 중간을 거친 자손이 남는다, Rocks는 stale handle을 남기지 않는다), 그다음 자기 body를 지운다.

**`ApplyWeldOnChildren()`(UE `BodyInstance.cpp:2641–2665`).** 자기 body가 있고 Dynamic일 때, 대상을 먼저 모은 뒤(weld됐거나 `autoWeld`인 자식만 내려간다 — UE `GetWeldedBodies(…, true)`) 실효 충돌에 Physics가 있는 자손을 이 body에 weld한다(UE `Weld` — 자손의 시뮬레이션 표지를 보지 않는다, 합치지 못하면 자기 body). `SetSimulatePhysics(true)`가 꺼짐 → 켬일 때 운동 종류를 바꾼 뒤(UE `SetInstanceSimulatePhysics` — `:2681–2685`), 충돌 켬 변경으로 물리 상태를 다시 만든 뒤(UE `SetCollisionEnabled` — `PrimitiveComponentPhysics.cpp:1386–1391`) 부른다.

**변경 따라가기.** weld된 컴포넌트의 transform이 직접 바뀌면(부모에서 전해진 변경이 아니면 — UE `OnUpdateTransform`의 `PropagateFromParent` — `PrimitiveComponent.cpp:1061–1076`) 같은 루트에 다시 weld한다(UE `SetWeldedBodyTransform` = `UnWeld` + `Weld`). `SceneComponent`가 전파 중 표지 `IsTransformUpdateFromParent()`를 준다. 형상 값 변경(`UpdateBodyGeometry`)도 다시 weld한다. 충돌 켬 변경: NoCollision 경계를 넘으면 물리 상태를 다시 만든다(파괴 → 생성 — unweld 뒤 자동 weld 판정, 루트는 그 뒤 `ApplyWeldOnChildren`). 넘지 않으면 weld된 컴포넌트는 새 켬으로 다시 weld한다(부분 충돌 켬 — UE `UpdatePhysicsFilterData`가 `WeldParent`로 간다). 채널·응답 변경은 PH6b3이다.

**API를 루트로(UE `GetBodyInstance()` 기본 weld).** weld된 컴포넌트의 힘·충격량·토크·방사 힘(자기 `ignoreRadial*` 표지를 먼저 본다)·속도 get/set·`WakeRigidBody`·`PutRigidBodyToSleep`·`RigidBodyIsAwake`·질량·질량 중심·관성·각속도 상한·body 값 setter와 getter(중력·감쇠·CCD·gyroscopic·질량 배율·덮어쓰기·질량 중심 이동)는 루트 컴포넌트의 같은 메서드를 부른다(UE는 루트 body 값을 바꾼다). 자기 값을 쓰는 것: `SetConstraintMode`(UE `GetBodyInstance(NAME_None, false)`), 깨움 이벤트 표지(UE `ShouldDispatchWakeEvents`는 자기 body 값), 충돌 setter, `SetSimulatePhysics`, `SetNotifyRigidBodyCollision`. 물리 호출 경고(시뮬레이션·물리 충돌 없음)는 루트 body 값으로 판정하고 부른 컴포넌트 slot으로 남긴다(UE `WarnInvalidPhysicsOperations_Internal`).

**질의.** weld된 컴포넌트의 컴포넌트 sweep은 hit 없음이다(UE `GetPhysicsObjectById`가 null — 형상이 없다, `PrimitiveComponentPhysics.cpp:1598–1611`). 컴포넌트 overlap은 루트 body의 내 부분 원소다(`OverlapBody(루트 body, 자기)` — UE `OverlapMulti`의 weld 경로). MTD도 hit 없음이다(body 없음).

**깨움 이벤트(UE `DispatchWakeEvents` — `PrimitiveComponent.cpp:3609–3637`).** 루트 컴포넌트가 깨움·잠듦을 발행한 뒤 직접 자식 가운데 `weldRoot_`가 자기인 것에 같은 이벤트를 재귀로 전한다(각자의 `generateWakeEvents`로 거른다). 손자는 그 부모가 루트가 아니면 전해지지 않는다(UE와 같다).

**진단.** `WeldFailed`(Warning — WeldBody 실패, 자기 body를 다시 만든다).

**시험(`Physics/Tests/PhysicsWeldTests.cpp` — 컴포넌트 경로).** `WeldTo`·같은 부모 다시 붙이기, weld 자식을 가진 컴포넌트를 다른 루트에 weld(자손 이동), body 없는 가운데 컴포넌트(루트 상태 다시 만들기·가운데 떼기), 풀기 연쇄, 충돌 켬 왕복·부분 충돌 켬, weld 자식 시뮬레이션 켬·형상 변경·sweep hit 없음, Kinematic 루트에 `weldSimulatedBodies`(자식 시뮬레이션 꺼짐), 게임 World 밖 autoWeld 없음, 그리고 시뮬레이션하는 루트 + `autoWeld` 자식: 자식 body 없음·`IsWelded`·질의 hit 컴포넌트가 자식, 루트가 Kinematic이면 weld 안 함, QueryOnly 자식은 weld 안 함, `weldSimulatedBodies` 부착으로 weld, 떼기·자식 파괴·루트 상태 파괴로 unweld(자식 body 다시 생김), 루트 시뮬레이션 켬으로 `ApplyWeldOnChildren`, 직접 이동하면 다시 weld(새 자리 hit), 부모 이동은 다시 weld하지 않음, API 전달(자식 `AddImpulse`가 루트 속도를 바꿈, 자식 `SetEnableGravity`가 루트 값을 바꿈, `GetMass`는 루트 질량), weld 자식 overlap, 깨움 이벤트 전달.

