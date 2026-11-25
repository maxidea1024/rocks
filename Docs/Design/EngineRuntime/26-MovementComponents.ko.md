# 엔진 런타임 — 26. 이동 컴포넌트(D4)

[목차](README.ko.md)

## 26. 이동 컴포넌트(D4)

설계일 2026-11-25. [24장](24-ComponentData.ko.md) 데이터 먼저 묶음의 D4다. 충돌 sweep 없이 **끝까지 동작**한다. 모든 이동은 순간 이동(teleport)이고 충돌은 일어나지 않는다. sweep·충돌 응답·bounce는 물리(CR16) 때 붙인다. UE 경로 약어: `MC` = `Engine/Source/Runtime/Engine/Private/Components/MovementComponent.cpp`, `PMC` = `…/ProjectileMovementComponent.cpp`, `SA` = `…/Private/GameFramework/SpringArmComponent.cpp`, `IT` = `…/Private/Components/InterpToMovementComponent.cpp`.

### 26.1 클래스

| Rocks(World) | UE | 부모 | Tick 단계 |
|---|---|---|---|
| `MovementComponent` | `UMovementComponent` | `Component` | PrePhysics(UE `TG_PrePhysics` — `MC:28`) |
| `RotatingMovementComponent` | `URotatingMovementComponent` | `MovementComponent` | PrePhysics |
| `ProjectileMovementComponent` | `UProjectileMovementComponent` | `MovementComponent` | PrePhysics |
| `PathMoverComponent` | `UInterpToMovementComponent`(이름은 [컴포넌트 도입](../ComponentCatalog/06-Decisions.ko.md) CD6) | `MovementComponent` | PrePhysics |
| `SpringArmComponent` | `USpringArmComponent` | `SceneComponent` | PostPhysics(UE `TG_PostPhysics`) |

UE 이벤트 delegate(`OnProjectileStop`·`OnInterpToReverse` 등)는 CR7 이벤트 체계가 없어 protected virtual hook으로 둔다(파생이 재정의한다). CR7이 생기면 hook 자리에서 이벤트를 낸다.

### 26.2 기반 보강

**SceneComponent.**
- `SetWorldLocationAndRotation(location, rotation)`(UE `InternalSetWorldLocationAndRotation` — `SceneComponent.cpp:3265`): 월드 값을 부모 frame(부모 월드 × 부착 socket) 기준 상대 값으로 바꿔 `SetRelativeTransform`으로 쓴다. 알려진 차이: UE는 상대·부모 scale에 음수가 있으면 `FTransform::GetRelativeTransform`(반사로 회전을 뒤집는 행렬 경로 — `:3283`)을 쓴다. Rocks는 회전을 TRS 사슬(`WorldQuaternion`)로만 풀어 반사 부모 아래 상대 회전이 UE와 다를 수 있다(반사 부모를 쓰는 콘텐츠가 생기면 그 경로를 더한다). 위치는 부모 frame 행렬의 역(Rocks 월드 행렬과 짝), 회전은 (부모 `WorldQuaternion` × socket 회전)⁻¹ × 회전이다. 절대 위치·회전 성분은 월드 값을 그대로 둔다. Mobility 검사(23.3절)를 거친다. 부모 frame을 역변환할 수 없으면 `InvalidArgument`다.
- **부착 socket 규칙 변경:** UE는 socket 이름이 비어도 부모의 `GetSocketTransform(빈 이름)`을 쓴다(`SceneComponent.cpp:727–754`). 그래서 SpringArm의 이름 없는 자식이 팔 끝에 붙는다. Rocks도 `WorldMatrix`·`WorldScale3D`·`WorldQuaternion`에서 이름과 무관하게 부모의 `GetSocketTransform`을 쓴다(기본 구현은 빈 이름에 단위를 준다 — 다른 동작은 같다). 23.3절의 "부착 socket이 있으면"을 고친다.
- `NotifyChildSocketsChanged()`(protected, UE `UpdateChildTransforms`): socket Transform이 바뀐 컴포넌트가 자식들의 월드 행렬을 무효로 하고 Transform 통지를 보낸다.

**Actor.** `virtual bool TryGetViewRotation(Rotator<double>& out)`(기본 거짓, UE `APawn::GetViewRotation`의 자리). Gameplay `Pawn`이 `GetControlRotation()`으로 재정의한다(Controller가 있으면 control rotation, 없으면 root 회전 — UE와 같고 관전 분기는 없다). World는 Pawn을 모르므로 virtual로 둔다.

**Core 수학(새 헤더, [Math 구현](../../Implementation/Math.ko.md)에 기록).**
- `Core/Math/InterpTo.h`: `VInterpTo(current, target, dt, speed)`(UE `UnrealMath.cpp:2584` — speed ≤ 0이거나 거리² < 1e-4면 target, 아니면 current + (target − current)·clamp(dt·speed, 0, 1)), `QInterpTo(current, target, dt, speed)`(`:2752` — speed ≤ 0이거나 부호 무시 성분 차 ≤ 1e-4면 target, 아니면 `CurveSlerp`(UE `FQuat::Slerp`)).
- `Core/Math/VectorUtility.h`: `ClampedToMaxSize(v, max)`(UE `GetClampedToMaxSize` — max < 1e-4면 0), `VectorPlaneProject(v, normal)`, `PointPlaneProject(p, origin, normal)`, `ToOrientationRotator(direction)`(UE `Vector.Rotation()` — yaw = atan2(y, x), pitch = atan2(z, √(x² + y²)), roll 0), `ToOrientationQuat(direction)`(UE `ToOrientationQuat` — 반각 식).
- 중력 상수 `DefaultGravityZ = −980`(cm/s², UE `UPhysicsSettings::DefaultGravityZ` — `PhysicsSettingsCore.cpp:29`)는 World 쪽 이름 있는 상수다. World 설정(UE `AWorldSettings::GlobalGravityZ`)과 물리 볼륨은 물리 단계다.

### 26.3 MovementComponent

필드(영속): `velocity`(cm/s), `constrainToPlane`·`planeConstraintNormal`·`planeConstraintOrigin`·`planeConstraintAxis`(`PlaneConstraintAxis { Custom, X, Y, Z, UseGlobalPhysicsSetting }`), `snapToPlaneAtStart`, `autoRegisterUpdatedComponent`(참), `autoUpdateTickRegistration`(참), `tickBeforeOwner`(참). 갱신 대상(`updatedComponent`)은 UE처럼 영속하지 않는다(Transient).

- 갱신 대상: `OnRegister`·`OnAwake`·`OnStart`에서 비어 있고 `autoRegisterUpdatedComponent`면 소유 Actor root를 쓴다(UE `OnRegister` + `InitializeComponent` — Rocks는 root가 PreAwake에서 생기는 Actor가 있어 OnStart에서도 본다). `SetUpdatedComponent(handle)`: SceneComponent가 아니면 `TypeMismatch`, 파괴 대기면 `InvalidArgument`(UE `IsValid` — `MC:70`).
  - 파괴된 대상은 다음 Tick에 기반 `OnTick`이 푼다. 회전·발사체 Tick은 기반 Tick을 먼저 부른다(UE는 파괴 대기 대상이 `ShouldSkipUpdate`를 통과해 `Super::TickComponent`가 푼다). 사라진 homing 대상은 substep 조건에서 빠진다(UE `HomingTargetComponent.IsValid()`).
  - Tick 간선: 갱신 대상의 OnTick이 이 컴포넌트 뒤, `tickBeforeOwner`면 소유 Actor OnTick도 뒤(UE `AddPrerequisite` — `MC:52–108`·`177–190`). 출처는 `TickEdgeSource::Framework`다(게임이 같은 쌍에 건 User 간선을 떼지 않고, 순환이면 Warning만 — 15.3절). 대상이 Tick을 켜지 않았으면(`InvalidArgument`) 걸지 않는다.
  - `autoUpdateTickRegistration`이면 대상이 있을 때만 Tick을 켠다(UE `UpdateTickRegistration`).
- 평면 제약(UE `MC:417–516`): 축 설정 X·Y·Z는 그 축, Custom은 저장한 법선(로드·편집 값도 정규화해 쓴다 — UE `OnRegister` 정규화 `MC:155`), UseGlobalPhysicsSetting은 Rocks에 전역 자유도 설정이 없어 제약 없음(법선 0)이다. 실효 법선을 축 설정에서 구하므로 `SetPlaneConstraintFromVectors`는 축을 Custom으로 바꾼다(UE `MC:458`은 축을 그대로 둔다 — UE는 법선을 저장하고 축 설정 setter가 덮어쓴다). `ConstrainDirectionToPlane`·`ConstrainLocationToPlane`·`ConstrainNormalToPlane`은 `constrainToPlane`일 때만 투영한다. `snapToPlaneAtStart`면 대상을 평면에 붙인다.
- `MoveUpdatedComponent(delta, rotation)`(UE `MoveUpdatedComponentImpl`, sweep 없음): delta를 평면 제약한 뒤 대상의 월드 위치 += delta, 월드 회전 = rotation(`SetWorldLocationAndRotation`). delta 0이고 회전이 같으면(1e-8, q·−q 모두 — UE `FQuat::Equals`) 아무것도 하지 않는다. 대상이 Movable이 아니면 실패한다(Mobility 검사).
- `UpdateComponentVelocity()`(대상의 `ComponentVelocity` = velocity), `StopMovementImmediately()`(velocity 0 + 위), `virtual MaxSpeed()`(0), `IsExceedingMaxSpeed(max)`(|v|² > max(0, max)²·1.01), `virtual GravityZ()`(`DefaultGravityZ`).
- `ShouldSkipUpdate()`: 대상이 없거나 Movable이 아니면 참(UE `MC:306` — 렌더 판정 `bUpdateOnlyIfRendered`는 렌더 단계).

### 26.4 RotatingMovementComponent

UE `RotatingMovementComponent.cpp:7–51`(기본 yaw 180 — `:15`). 필드: `rotationRate`(Rotator 도/초, 기본 (0, 180, 0)), `pivotTranslation`(cm, 0), `rotationInLocalSpace`(참). Tick: ΔQ = Rotator(rate × dt).ToQuaternion(), 새 회전 = local이면 Q × ΔQ, 아니면 ΔQ × Q, pivot이 0이 아니면 Δ위치 = Q·pivot − 새 Q·pivot, `MoveUpdatedComponent(Δ위치, 새 회전)`. velocity는 바꾸지 않는다.

### 26.5 ProjectileMovementComponent

UE `PMC:18–64` 기본값: `initialSpeed` 0, `maxSpeed` 0(제한 없음), `rotationFollowsVelocity`·`rotationRemainsVertical` 거짓, `initialVelocityInLocalSpace` 참, `forceSubStepping` 거짓, `simulationEnabled` 참, `projectileGravityScale` 1, `homing`(UE `bIsHomingProjectile`) 거짓·`homingAccelerationMagnitude` 0, `maxSimulationTimeStep` 0.05 [0.0166, 0.5], `maxSimulationIterations` 4 [1, 25], velocity (1, 0, 0). 충돌 데이터(`shouldBounce`·`bounciness` 0.6·`friction` 0.2·`bounceVelocityStopSimulatingThreshold` 5·`minFrictionFraction` 0·`bounceAngleAffectsFriction`·`sweepCollision` 참·`bounceAdditionalIterations` 1)는 데이터로만 둔다(sweep이 없어 쓰이지 않는다). homing 대상은 영속하지 않는 handle이다(UE `TWeakObjectPtr`).

- `OnAwake`(UE `InitializeComponent` — `PMC:83`): velocity가 0이 아니면, initialSpeed > 0일 때 방향 × initialSpeed, local이면 대상 월드 회전으로 돌린다, `rotationFollowsVelocity`면 대상 회전을 속도 방향으로(수직 유지면 yaw만), `UpdateComponentVelocity`.
- Tick(`PMC:141–334`, 충돌 분기 제외): 정지했거나 skip이면 끝. 남은 시간을 반복(최대 `maxSimulationIterations`, 1e-6 이상)으로 나눈다. 단계 시간 = substep이면 `SimulationTimeStep`(남은 시간 > 최대 단계고 마지막 반복이 아니면 min(최대, 남은 시간/2)), 아니면 남은 시간. 단계마다:
  - 이동량 = v0·t + (v1 − v0)·t/2(v1 = `ComputeVelocity(v0, t)` — velocity Verlet).
  - 회전 = 속도를 따르면(그리고 |v0| 성분이 1 cm/s를 넘으면) 방향 quaternion(수직 유지면 yaw만), 아니면 지금 회전.
  - `MoveUpdatedComponent`. hook이 멈췄으면 끝. velocity가 그대로면 v = `ComputeVelocity(v, t)`.
  - 끝에 `UpdateComponentVelocity`.
- `ShouldUseSubStepping`: `forceSubStepping` 또는 중력 ≠ 0 또는 homing 대상이 있다.
- `ComputeVelocity(v, t)` = `LimitVelocity(v + a·t)`, a = (0, 0, `GravityZ`) + 이번 갱신 힘 + homing(정규화(대상 위치 − 위치) × 크기). `LimitVelocity`: maxSpeed > 0이면 `ClampedToMaxSize`, 그다음 평면 제약. `GravityZ` = 기본 × scale(scale 0이면 0).
- `AddForce`·`ClearPendingForce(immediate)`(UE와 같다), `StopSimulating()`(velocity·힘 0, `UpdateComponentVelocity`, 대상 해제 → Tick 꺼짐, hook `OnProjectileStop()`).

### 26.6 PathMoverComponent

UE `UInterpToMovementComponent`(`IT:14–581`). 이름과 달리 점 사이를 **선형**으로 보간한다(UE와 같다).

- 제어점 `PathControlPoint { position(cm), positionIsRelative(참) }`(UE `FInterpControlPoint`)의 영속 배열 `controlPoints`. 점별 시작 시간·비율·다음까지 거리는 영속하지 않는 캐시다.
- 필드: `duration` 1(0.1 이상 — UE ClampMin), `behaviour`(`PathMoverBehaviour { OneShot, OneShotReverse, LoopReset, PingPong }`, 기본 OneShot), `pauseOnImpact` 거짓·`sweep` 참(충돌 데이터 — 쓰이지 않는다), `forceSubStepping` 거짓, `maxSimulationTimeStep` 0.05, `maxSimulationIterations` 8, `speedMultiplier` 0(UE 0 초기화 — 0보다 클 때만 곱한다).
- `OnStart`(UE `BeginPlay` → `FinaliseControlPoints`): 시작 위치 = 지금 위치, 시간 배율 = 1/max(duration, 0.1)(로드 값이 setter 하한을 거치지 않았을 때 0으로 나누지 않게), 제어점 캐시(거리·누적 비율 — `IT:475–523`), 대상을 점 0으로 옮긴다.
- Tick(`IT:47–188`, 충돌 분기 제외 — 충돌 시간은 늘 1): 남은 시간 = dt × (speedMultiplier > 0 ? speedMultiplier : 1). 반복마다 목표 시간 = clamp(지금 + 단계 × 배율 × 방향, 0, 1), Δ = `ComputeMoveDelta(목표)`, velocity = Δ/단계, 이동, `CalculateNewTime`(1 이상: OneShot 정지·hook `OnPathStop(1)`, LoopReset 0·hook `OnPathReset(0)`, 그 밖 1·방향 반전·hook `OnPathReverse(1)`; 0 미만: OneShotReverse 정지·`OnPathStop(0)`, PingPong 0·반전), 남은 비율이 있으면(반사 최대 2번) 그 시간을 더 돈다.
- `ComputeMoveDelta(t)`: t가 속한 점 i, α = (t − 시작 i)/비율 i, 점 위치는 **지금 컴포넌트 월드 회전**으로 돌리고 상대면 시작 위치를 더한다(UE 그대로), 선형 보간 위치 − 지금 위치.
- `RestartMovement(direction)`, `ResetControlPoints()`, `AddControlPoint(position, relative)`, `StopMovementImmediately()`(정지 + hook `OnPathStop(지금 시간)`).

### 26.7 SpringArmComponent

UE `SA:18–289`. 필드(영속): `targetArmLength` 300, `socketOffset`·`targetOffset` 0, `probeSize` 12·`doCollisionTest` 참(충돌 데이터 — sweep이 없어 쓰이지 않는다), `usePawnControlRotation` 거짓, `inheritPitch`·`inheritYaw`·`inheritRoll` 참, `enableCameraLag`·`enableCameraRotationLag` 거짓, `useCameraLagSubstepping` 참, `cameraLagSpeed`·`cameraRotationLagSpeed` 10([0, 1000]), `cameraLagMaxTimeStep` 1/60([0.005, 0.5]), `cameraLagMaxDistance` 0(0 이상). 상태(이전 위치·회전·socket 상대 위치·회전)는 영속하지 않는다.

- socket `SpringEndpoint`(UE `SocketName`): `HasSocket`은 이 이름, `GetSocketTransform`은 이름과 무관하게 팔 끝 상대 Transform이다(UE `SA:262` — 26.2절 socket 규칙과 짝).
- `OnRegister`: 이전 값을 지연 없이 채운다(UE `UpdateDesiredArmLocation(false, false, false, 0)` — `SA:239`). Rocks는 `AddComponent`(OnRegister) 뒤에 부착하므로 `OnStart`에서 한 번 더 채운다(UE는 `SetupAttachment`가 OnRegister 전이다 — 그러지 않으면 지연이 원점·단위 회전에서 쓸려 온다).
- Tick(PostPhysics) = `UpdateDesiredArmLocation(doCollisionTest, enableCameraLag, enableCameraRotationLag, dt)`(`SA:93–225`):
  - 목표 회전 = 컴포넌트 월드 회전, `usePawnControlRotation`이고 소유 Actor가 `TryGetViewRotation`을 주면 그 값, 절대 회전이 아니면 상속하지 않는 성분을 상대 회전 값으로.
  - 회전 지연: substep이면 (목표 − 이전).GetNormalized()/dt로 쪼개 `QInterpTo`, 아니면 한 번 `QInterpTo`.
  - 팔 시작 = 컴포넌트 월드 위치 + targetOffset(월드 공간). 위치 지연은 같은 방식의 `VInterpTo`, `cameraLagMaxDistance` > 0이면 시작에서 그 거리로 자른다.
  - 끝 = 시작 − 회전 앞쪽 × 길이 + 회전 × socketOffset. 충돌 시험은 하지 않는다(sweep 없음 — 결과는 끝 그대로).
  - socket 상대 Transform = (회전, 끝)을 컴포넌트 월드 기준으로 바꾼 값, `NotifyChildSocketsChanged`.

### 26.8 결정·시험

| ID | 항목 | 결정 | 근거 |
|---|---|---|---|
| ERD46 | 충돌 없는 이동 | 모든 이동은 teleport, 충돌 데이터는 영속만 | 물리(CR16) 전 끝까지 동작 |
| ERD47 | UE delegate | protected virtual hook | CR7 없음 |
| ERD48 | 이름 없는 socket | 부모 `GetSocketTransform(빈 이름)`을 늘 쓴다 | UE 부착 규칙, SpringArm |
| ERD49 | Pawn 회전 | Actor virtual `TryGetViewRotation` | World는 Gameplay를 모른다 |
| ERD50 | 공용 수학 | VInterpTo·QInterpTo·ClampedToMaxSize·평면 투영·방향 회전을 Core에 | 공용 기능 중복 금지 |

시험 `World/Tests/WorldMovementComponentTests.cpp`(`Movement_` 접두): 갱신 대상 자동 등록·Tick 간선, 평면 제약 셋과 snap, 정지·속도·최대 속도, Static 대상 거부; 회전(local·world·pivot); 발사체 초기 속도(local·속도), 중력 궤적(Verlet — 손계산), substep 분할, 최대 속도, homing, 회전 따름·수직 유지, `StopSimulating`; PathMover 네 동작(OneShot 정지 위치·hook, Reverse, LoopReset, PingPong 반전), 상대·절대 점, speedMultiplier; SpringArm 팔 끝 위치·자식 카메라 위치(이름 없는 부착), socketOffset·targetOffset, inherit 끄기, 위치·회전 지연(substep 포함, 최대 거리), Pawn control rotation(Gameplay 시험). Core 수학은 `Core/Tests`에 UE 식 대조 시험을 더한다.
