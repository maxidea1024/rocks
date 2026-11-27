# 물리 모듈 — 6. Jolt 사상(CD3)

[목차](README.ko.md)

[충돌 데이터](../CollisionData.ko.md) 8절이 물리 단계로 넘긴 항목(CD3)이다. 좌표·단위 변환은 [물리 어댑터 경계](../PhysicsJoltAdapter.ko.md)를 따른다. Jolt 경로는 반입 원본 `Engine/Source/ThirdParty/Jolt/JoltPhysics-5.7.0/Jolt/` 기준이다.

## 6.1 층 구성

Jolt 충돌 필터는 세 겹이다: broadphase 층(`BroadPhaseLayer`), object 층(`ObjectLayer` — body마다 하나), 필터(`ObjectLayerPairFilter`·`ObjectVsBroadPhaseLayerFilter`, 질의용 `ObjectLayerFilter`·`BodyFilter`·`ShapeFilter` — `Physics/Collision/ObjectLayer.h:26–62`, `Physics/Body/BodyFilter.h`). **broadphase 층은 object 층만 보고 정한다**(`BroadPhaseLayerInterface::GetBroadPhaseLayer(ObjectLayer)` — `Physics/Collision/BroadPhase/BroadPhaseLayer.h:70`). 그래서 운동 종류와 참여 방식도 object 층 bit에 넣어야 한다(리뷰 반영 — 개정 1의 16 bit 배치는 Static과 Dynamic body를 같은 층에 넣었다).

**object 층(32 bit — `JPH_OBJECT_LAYER_BITS` 32, PMD12).**

| bit | 값 |
|---|---|
| 0–5 | object type(채널 0–63) |
| 6–7 | 운동 묶음: 0 Static, 1 Moving(Kinematic·Dynamic), 2 QueryOnly(시뮬레이션에 참여하지 않는 body), 3 예약 |
| 8–10 | 참여 표지: physics(시뮬레이션 반작용), query(질의 대상), probe(접촉 데이터만) — 실효 충돌 켬(CollisionData 2.2절)에서 |
| 11–31 | 응답 묶음 색인(장면의 응답 묶음 표 — 같은 64칸 응답을 쓰는 body가 공유, 참조 수를 센다) |

- 반입 정의 `JPH_OBJECT_LAYER_BITS`는 16(JL1)에서 32로 바꾼다. PH1에서 Jolt `ThirdParty.json`의 정의·consumerDefines와 어댑터 경계 10.2절을 함께 고친다(버전 ID 기능 bit가 바뀌므로 `VerifyJoltVersionID`가 불일치를 잡는다).
- 응답 묶음 색인 21 bit(약 200만 종)는 넘칠 수 없는 크기로 둔다(넘으면 진단 `ResponseSetCapacity`와 body 생성 실패).

**broadphase 층(3개).** object 층의 운동 묶음 bit로 정한다: `Static`, `Moving`, `QueryOnly`. QueryOnly는 시뮬레이션 쌍을 만들지 않으므로 broadphase 쌍 필터가 QueryOnly와 모든 층의 시뮬레이션 쌍을 바로 버린다(UE `QueryOnly` "keep data out of simulation tree" — `E/Classes/Engine/EngineTypes.h:1810`).

## 6.2 시뮬레이션 쌍 필터

`ObjectLayerPairFilter::ShouldCollide(a, b)`:

1. 둘 다 Static이면 거짓. 어느 한쪽이 QueryOnly이거나 physics·probe 표지가 없으면 거짓(시뮬레이션에 참여하지 않는다).
2. 두 층의 응답 묶음 색인 → 묶음을 찾는다. a 쪽 판정 = a 묶음에서 b object type의 응답, b 쪽 판정 = b 묶음에서 a object type의 응답. 결과 = 최소(UE 컴포넌트 대 컴포넌트 응답 — `E/Private/Components/SceneComponent.cpp:3089–3092`).
3. 최소가 Block이면 충돌. 그 밖(Overlap·Ignore)이면 시뮬레이션 접촉 없음. Overlap은 질의 기반 overlap(5.3절)이 다룬다 — 시뮬레이션은 Block만 본다(UE와 같다).

- 한쪽만 probe인 Block 쌍은 충돌로 통과시키고, 반작용은 접촉 단계에서 끈다(6.4절).
- `ObjectVsBroadPhaseLayerFilter`는 운동 묶음만 본다(Static 대 Static·QueryOnly 대 어느 것이든 거짓).
- 판정은 쌍마다 표 두 칸 조회다. 결과를 캐시하지 않는다(조회가 배열 두 칸이라 캐시 이득이 없다).

## 6.3 질의 필터

4.4절 판정을 Jolt 질의 필터 셋으로 나눈다.

| 단계 | Jolt 필터 | 판정 |
|---|---|---|
| 1 | `BroadPhaseLayerFilter` | 쓰지 않는다(기본 — 모든 broadphase 층) |
| 2 | `ObjectLayerFilter` | query 표지가 없으면 제외. Channel 질의: 질의 응답의 body object type 응답 × body 묶음의 질의 채널 응답 = 최소, Ignore면 제외. ObjectTypes 질의: object type bit. `ignoreTouches`·`ignoreBlocks`, Test·First의 겹침 제외도 여기서 한다(7.1.7절) |
| 3 | `BodyFilter` | 무시 목록(컴포넌트·Actor — 장면 칸이 기억한 값과 비교)과 `mobility`(칸의 운동 종류 — QueryOnly body는 층에 운동 묶음이 없다) |
| 4 | `ShapeFilter` | `traceComplex`(단순·complex 형상 구분 — compound 하위 형상 표지), weld 자식의 충돌 켬(PH6) |

- 단계 2의 최소 응답이 Overlap인지 Block인지는 결과 분류(`blockingHit`)에 쓰이므로, 필터는 통과시키고 수집기(`CollisionCollector`)가 body 층에서 다시 계산해 기록한다.
- `First` 질의는 "막는 것 중 가장 가까운 것"이어야 한다(겹침은 건너뜀). 수집기를 Rocks가 구현해 막음만 early-out 기준으로 쓴다. `All`은 모두 모은 뒤 4.5절 정렬·자르기.

## 6.4 접촉·probe·hit

- `ContactListener::OnContactAdded`·`OnContactPersisted`(step 안, worker thread 가능 — `Physics/Collision/ContactListener.h:74–108`)에서:
  - **probe**(어느 한쪽이 probe 표지): `ioSettings.mIsSensor = true`로 그 접촉의 반작용을 끈다(`ContactListener.h:49` — 접촉 데이터는 나오지만 밀어내지 않는다. CCD 경로에서도 지켜진다 — `Physics/PhysicsSystem.cpp:2078`). probe hit는 실제로 겹친 접촉(깊이 ≥ 0)만이다(7.1.12절).
  - 두 body 중 `notifyRigidBodyCollision`이 참인 쪽이 있으면 `ContactNotice`(body user data 둘, 하위 형상, 첫 접촉점, 법선, 깊이, probe 여부, CCD body가 끼면 추정 충격량)를 미리 잡은 배열 하나에 mutex로 복사한다(7.1.12절).
  - 게임 코드를 부르지 않고, 다른 body 상태를 읽지 않는다(계획 16.2절).
- **합치는 순서.** 고정 스텝마다 `Update` 뒤 (낮은 칸 번호, 높은 칸 번호, 하위 형상, 값) 순으로 정렬한다(worker 배정이 비결정적이므로 — 7.1.12절). 같은 스텝의 같은 쌍 접촉은 하나로 합친다(스텝이 다르면 따로 — 5.4절).
- **충격량.** UE처럼 solver가 실제로 쓴 충격량이다(PMD14). Jolt는 `OnContactAdded` 시점(해 풀기 전)에 충격량을 주지 않으므로(`ContactListener.h:106–108`) 고정 스텝마다 `Update`가 끝난 뒤 그 스텝에 모은 `ContactNotice`의 body 쌍마다 `PhysicsSystem::GetAppliedContactImpulse`(Rocks 추적 가능한 수정 — 어댑터 경계 10.6절)로 받는 body 쪽 충격량 벡터(접촉마다 법선 방향 — UE `E/Private/PhysicsEngine/Experimental/PhysScene_Chaos.cpp:1068–1070`과 같다)를 읽는다. 변환은 운동량 벡터(축 A, kg·m/s → kg·cm/s ×100 — 어댑터 경계 3절). CCD로만 닿은 쌍(함수가 거짓이거나 접촉점 0 — Jolt CCD 캐시 항목은 접촉점이 없다)은 listener에서 `EstimateCollisionResponse`(`Physics/Collision/EstimateCollisionResponse.h:37`)로 구해 둔 추정값을 쓴다. 같은 스텝에 discrete와 CCD가 섞인 쌍은 CCD 몫이 빠진다(한계 — 어댑터 경계 10.6절). 공개 구현 UnrealJolt는 늘 추정값을 쓴다(어댑터 경계 11.4절).
- **hit 문턱.** UE `MinDeltaVelocityForHitEvents`(기본 0 — `E/Private/PhysicsEngine/PhysicsSettings.cpp:31`)를 물리 설정에 둔다. 0보다 크면 두 Dynamic body의 선속도 변화(step 시작 → 끝, 성분마다)가 모두 이 값 이하인 접촉은 통지하지 않는다(0이면 모두, probe는 거르지 않는다 — 7.1.12절).
- `OnContactValidate`는 쓰지 않는다(필터 판정은 6.2절이 끝낸다).

## 6.5 body 값 사상

[충돌 데이터](../CollisionData.ko.md) 6.2–6.3절 표의 Jolt 열을 구현 규칙으로 확정한다.

| BodySettings | Jolt | 규칙 |
|---|---|---|
| `simulatePhysics`·Mobility | `EMotionType` | 2.5절. Static ↔ Movable은 재등록이라(2.3.1절) `mAllowDynamicOrKinematic`(Static으로 만든 body에만 필요 — `Physics/Body/BodyCreationSettings.h:99`)은 쓰지 않는다 |
| `startAwake` | `EActivation` | 2.3절(소유 Actor BeginPlay 전이면 잠든 채) |
| `enableGravity` | `mGravityFactor` 1 / 0 | |
| `overrideMass`·`massInKgOverride`·`massScale` | `mOverrideMassProperties`·`mMassPropertiesOverride` | 계산 질량(형상 밀도 — 6.6절)에 배율 → 덮어쓰기면 `ScaleToMass(override)` |
| `centerOfMassOffset` | `OffsetCenterOfMassShape`로 형상을 감쌈 | cm → m, 축 A |
| `inertiaTensorScale` | `EOverrideMassProperties::MassAndInertiaProvided` | 계산 관성의 주축값에 축별 배율(축 A로 재배치) |
| `updateMassWhenScaleChanges` | scale 갱신 때 질량 재계산 여부 | 참이면 새 형상으로 다시 계산, 거짓이면 이전 질량·관성을 `MassAndInertiaProvided`로 유지 |
| `linearDamping`·`angularDamping` | `mLinearDamping`·`mAngularDamping` | 값 그대로(1/s). Jolt 기본 0.05에 맡기지 않는다 |
| `overrideMaxAngularVelocity`·`maxAngularVelocity` | `mMaxAngularVelocity` | 도/s → rad/s. 덮어쓰기가 아니면 물리 설정 값(6.8절) |
| (없음) | `mMaxLinearVelocity` | 물리 설정 `maxLinearVelocity`(cm/s → m/s). 기본은 UE처럼 사실상 없음(PMD18 — 6.8절) |
| `continuousCollision` | `EMotionQuality::LinearCast` | |
| `smoothEdgeCollisions` | `mEnhancedInternalEdgeRemoval` | |
| `overrideSolverIterations`·`positionIterations`·`velocityIterations` | `mNumPositionStepsOverride`·`mNumVelocityStepsOverride` | 덮어쓰기면 값, 아니면 0(장면 기본) |
| `gyroscopicTorque` | `mApplyGyroscopicForce` | |
| `dofMode`·`lock*` | `mAllowedDOFs`(월드 축 bit) | 축 bit는 A로 재배치(Rocks X → Jolt Z, Y → X, Z → Y). 잠금 축은 **월드 축**이고 잠근 값은 body를 만들 때의 위치·자세다(PMD19 — UE `CreateDOFLock`은 body 대 월드 joint로, 월드 쪽 frame이 월드 X·Y 축과 생성 시점 위치다, `E/Private/PhysicsEngine/BodyInstance.cpp:856–925`, 생성은 `InitDynamicProperties` — `:4727`). 평면 방식(`YZPlane` 등)은 법선 방향 이동과 법선에 수직인 두 축 회전을 잠근다(UE `:890–903` — 평면 안 이동과 법선 축 회전만 남는다). **모든 자유도를 잠근 조합**(Jolt `EAllowedDOFs::None`은 허용되지 않는다 — `Physics/Body/AllowedDOFs.h:12`)은 진단 후 Kinematic처럼 다루지 않고 잠금을 적용하지 않는다(경고 `AllDegreesOfFreedomLocked`). **CustomPlane**(임의 법선 평면)은 월드 축 bit로 표현할 수 없어 Jolt `SixDOFConstraint`(body 대 고정)로 구현한다(PH5) |
| `generateWakeEvents` | `BodyActivationListener` | 2.7절 |
| `notifyRigidBodyCollision` | 접촉 수집 표지 | 6.4절 |
| `autoWeld` | compound | 2.8절 |
| `walkableSlope` | (물리 아님) | 캐릭터 이동(CM13) |

## 6.6 물리 재질과 질량

- Jolt에서 마찰·반발은 **body 값**이다(`BodyCreationSettings::mFriction` 기본 0.2·`mRestitution` 기본 0 — `Physics/Body/BodyCreationSettings.h:107–108`, 접촉 결합 `Physics/Constraints/ContactConstraintManager.h:555–556`). Jolt `PhysicsMaterial`에는 값이 없다(디버그 이름·색뿐). 그래서 재질 값은 body 생성 때 `mFriction`·`mRestitution`에 넣는다.
- 물리 재질 에셋(UE `UPhysicalMaterial` — 마찰·반발·밀도·결합 방식·표면 종류)은 에셋 단계에서 들인다. 그 전에는 UE 기본값(마찰 0.7, 정지 마찰 0, 반발 0.3, 밀도 1 g/cm³ — `PC/Private/PhysicalMaterial.cpp:45–49`)을 모든 body에 쓴다. 밀도는 형상 밀도로 넣는다(g/cm³ → kg/m³ — K1 `GramsPerCubicCentimeterToKilogramsPerCubicMeter`).
- 결합 방식(UE `EFrictionCombineMode` Average·Min·Multiply·Max — `PC/Public/PhysicsSettingsEnums.h:14–19`, 마찰·반발 둘 다 기본 Average: `UPhysicsSettingsCore`가 값을 정하지 않아 0 = Average)은 `PhysicsSystem::SetCombineFriction`·`SetCombineRestitution`으로 맞춘다(Jolt 기본은 마찰 기하평균·반발 최대 — 그대로 두면 UE와 다르다). 결합 함수는 상태 없는 함수 포인터라(`ContactConstraintManager.h:53` `CombineFunction`) 모드는 **프로세스 전역**이다(물리 설정 값 — UE도 프로젝트 설정 하나다). body별 재질의 결합 모드 덮어쓰기(UE `bOverrideFrictionCombineMode`)는 재질 에셋 단계에서 body user data로 함수 안에서 고른다.
- 반발 문턱: UE `BounceThresholdVelocity`(200 cm/s — `PC/Private/PhysicsSettingsCore.cpp:42`)를 Jolt `PhysicsSettings::mMinVelocityForRestitution`(m/s)에 넣는다(Jolt 기본 1 m/s = 100 cm/s와 다르다).

## 6.7 job·할당·hook

- 할당·로그·assert·버전 확인·시작 순서: 어댑터 경계 10.4절.
- job: `JobSystemWithBarrier`를 상속한 `TaskJobSystem`이 `CreateJob`·`FreeJob`·`QueueJob`·`QueueJobs`를 구현한다. `QueueJob`은 Task `WorkerScheduler::Submit`으로 보낸다. 미리 잡은 job 노드 풀(크기 = `cMaxPhysicsJobs` 2048 — `Physics/PhysicsSettings.h:25`)을 쓰고, 풀이 차면 제출 thread에서 바로 실행한다(계획 16.1절). `GetMaxConcurrency`는 Task가 물리에 준 worker 수(계획 16.1절 "Game·Render·IO 예약분 제외")다.
- worker의 FP 상태(FTZ·DAZ·반올림)가 같은지 PH1 시험이 확인한다(어댑터 경계 10.4절).
- 임시 할당: World마다 `TempAllocatorImpl`(크기는 물리 설정 `tempAllocatorBytes`, 기본 10 MiB — Jolt 예제 값, 측정 후 조정).

## 6.8 물리 설정

설정 struct `PhysicsSettings`(Physics 모듈, branch Engine, 섹션 Physics — UE `UPhysicsSettings`·`UPhysicsSettingsCore`):

| 필드 | 기본값 | UE |
|---|---|---|
| `defaultGravityZ` | −980 cm/s² | `DefaultGravityZ`(`PC/Private/PhysicsSettingsCore.cpp:29`) |
| `maxAngularVelocity` | 3600 도/s | `MaxAngularVelocity`(`:43`) |
| `maxLinearVelocity` | `UnlimitedLinearVelocity` = √FLT_MAX cm/s(이름 있는 상수 — 사실상 없음) | body마다 `SetMaxLinearVelocity(√FLT_MAX)`(`E/Private/PhysicsEngine/BodyInstance.cpp:1331`, 제곱해도 넘치지 않는 최대 — `PC/Private/ChaosEngineInterface.cpp:693–701`). 임시 상한 CVar `p.HackMaxVelocity2`는 기본 −1(끔 — `CH/Private/Chaos/PBDRigidsEvolutionGBF.cpp:53–54`) |
| `bounceThresholdVelocity` | 200 cm/s | `BounceThresholdVelocity`(`:42`) |
| `minDeltaVelocityForHitEvents` | 0 cm/s | `MinDeltaVelocityForHitEvents`(`E/Private/PhysicsEngine/PhysicsSettings.cpp:31`) |
| `defaultDegreesOfFreedom` | `Full3D`(`SettingsDof { Full3D, YZPlane, XZPlane, XYPlane }`) | `ESettingsDOF`(`E/Classes/PhysicsEngine/PhysicsSettings.h:73–78`·`:278`, 값을 정하지 않아 0 = Full3D) |
| `frictionCombineMode`·`restitutionCombineMode` | Average·Average | `UPhysicsSettingsCore` |
| `maxBodies`·`maxBodyPairs`·`maxContactConstraints` | 측정 후 | Jolt `PhysicsSystem::Init` |
| `tempAllocatorBytes` | 10 MiB | Jolt |
| 길이 차원 Jolt 설정(`speculativeContactDistance` 등) | Jolt 기본 × 100(cm) | 어댑터 경계 6절 J12 |

- 이 struct의 등록은 Physics 모듈 `RegisterSettings`다(HostSettings가 설정 struct 모듈의 타입을 등록 — ModuleLoader 3.10절 보강).
