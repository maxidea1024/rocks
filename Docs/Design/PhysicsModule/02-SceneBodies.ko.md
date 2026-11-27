# 물리 모듈 — 2. 장면과 body

[목차](README.ko.md)

## 2.1 장면 소유

- World마다 물리 장면이 하나다(UE `UWorld::PhysicsScene` — `E/Classes/Engine/World.h:1588`, 생성 `E/Private/World.cpp:2461–2464`·`6807–6843`, 파괴 `:1540–1561`). Tryout 세션 둘이면 장면도 둘이다(계획 16.4절 "World·Tryout session마다 분리한 physics system").
- 만드는 쪽은 Physics 모듈의 World subsystem `PhysicsWorldSubsystem`이다. `Initialize`에서 Jolt 장면을 만들어 World에 접점으로 꽂고(2.2절), `Deinitialize`에서 뗀 뒤 파괴한다. subsystem 정리는 초기화 역순이고(ERD8) 모든 Actor 정리 뒤이므로(16.5절 표), body는 장면보다 먼저 사라진다.
- 생성 판정(`shouldCreate` — ERD9): World 종류가 Game·Tryout·Editor·Preview면 만든다. Inactive는 만들지 않는다. UE는 `InitWorld`의 `bCreatePhysicsScene`(기본 참)으로 편집·미리보기 World에도 장면을 만든다(편집기 picking·미리보기 충돌이 쓴다).
- 프로세스 공유 자원(Jolt 할당 hook·`Factory`·`RegisterTypes`)은 장면마다 참조 수로 쥐어 첫 장면이 만들고 마지막 장면이 해제한다(어댑터 경계 10.4절 순서, PMD21 — 7.1.1절). 공유 형상 캐시의 수명은 PH6에서 정한다. 종료 순서는 계획 16.4절: 진행 중 step 완료 → World 장면 해제 → 공유 형상 해제 → 프로세스 공유 자원 해제.

## 2.2 World 쪽 접점

World는 Physics를 모르므로 World 모듈에 추상 접점을 둔다(UE `UWorld::GetPhysicsScene()`이 돌려주는 자리).

```cpp
// World/Physics/PhysicsScene.h (World 모듈 — Jolt 타입 없음)
class PhysicsScene {
public:
    virtual ~PhysicsScene() = default;
    // body 수명(2.3절) — PrimitiveComponent만 부른다.
    virtual Result<PhysicsBodyHandle> CreateBody(PrimitiveComponent&, const BodyCreateDesc&) noexcept = 0;
    virtual void DestroyBody(PhysicsBodyHandle) noexcept = 0;
    virtual Result<void> UpdateBody(PhysicsBodyHandle, BodyUpdate) noexcept = 0;      // 필터·운동 종류·scale·DOF
    virtual void SetBodyTransform(PhysicsBodyHandle, const Transformd&, TeleportMode) noexcept = 0;
    // 힘·속도·잠듦(2.6–2.7절)
    virtual Result<void> ApplyForce(PhysicsBodyHandle, const ForceCommand&) noexcept = 0;
    virtual void SetVelocity(PhysicsBodyHandle, const VelocityCommand&) noexcept = 0;
    virtual BodyKinematics ReadKinematics(PhysicsBodyHandle) const noexcept = 0;   // 위치·회전·속도·질량
    virtual void SetAwake(PhysicsBodyHandle, bool awake) noexcept = 0;
    virtual bool IsAwake(PhysicsBodyHandle) const noexcept = 0;
    // 질의(4장)
    virtual bool LineTrace(const LineTraceRequest&, QueryResults&) const noexcept = 0;
    virtual bool ShapeTrace(const ShapeTraceRequest&, QueryResults&) const noexcept = 0;
    virtual bool Overlap(const OverlapRequest&, QueryResults&) const noexcept = 0;
    virtual bool ComponentTrace(const PrimitiveComponent&, const ComponentTraceRequest&, QueryResults&) const noexcept = 0;
    virtual bool ComponentOverlap(const PrimitiveComponent&, const ComponentOverlapRequest&, QueryResults&) const noexcept = 0;
    // 침투 해소(5.2절)
    virtual bool ComputePenetration(const PrimitiveComponent&, const CollisionShape&, const Transformd&, MtdResult&) const noexcept = 0;
};

class World {
public:
    PhysicsScene* GetPhysicsScene() const noexcept;               // 없으면 null
    void SetPhysicsScene(PhysicsScene*) noexcept;                 // PhysicsWorldSubsystem만 부른다
};
```

- 함수 묶음은 이 장과 3–5장의 동작을 담는 최소 집합이다. 이름·인자 struct는 구현 때 확정한다(PH1). 원칙: 공개 인자는 Rocks 단위(cm·kg·초·도/rad 이름 구분), 결과는 값, 실패는 `Result`.
- 접점 교체는 World가 Initialized 상태(Actor 등록 전)일 때만 받는다. 등록된 컴포넌트가 있는데 장면이 바뀌면 body가 어긋나므로 거부한다(`InvalidArgument`).
- 접점 호출은 게임 lane에서만 한다(3.2절). debug build는 소유 스레드를 검사한다.

## 2.3 body 수명

컴포넌트 시스템 상태(엔진 런타임 23.2절 — `SystemStateKind::Physics`, UE `CreatePhysicsState`·`OnCreatePhysicsState`)로 만든다.

| 단계 | Rocks | UE |
|---|---|---|
| 판정 | `PrimitiveComponent::WantsSystemState(Physics)` = 장면이 있음 && 형상이 있음(`HasCollisionGeometry()` — 파생이 정한다) && (실효 충돌 켬 ≠ `NoCollision` 또는 `alwaysCreatePhysicsState`) | `ShouldCreatePhysicsState` — `E/Private/Components/PrimitiveComponent.cpp:2124–2131`, 장면 확인 `ActorComponent.cpp:2408` |
| 생성 | `CreateSystemState(Physics)`: 실효 충돌 값(`TryResolveCollision` — CD2)·`BodySettings`·형상(2.4절)·컴포넌트 월드 transform으로 `BodyCreateDesc`를 채워 `CreateBody`. handle은 컴포넌트의 NATIVE 필드 | `OnCreatePhysicsState` → `FBodyInstance::InitBody` — `PrimitiveComponent.cpp:882–973`, scale 0 보정 `:909–925` |
| 파괴 | `DestroySystemState(Physics)`: weld 해제(2.8절) → `DestroyBody` | `OnDestroyPhysicsState` → `UnWeld*` → `TermBody` — `PrimitiveComponent.cpp:1143–1169` |
| 시작 잠듦 | `startAwake`가 거짓이고 **소유 Actor가 아직 BeginPlay 전**이면 Dynamic body를 잠든 채 넣는다(게임 중 Spawn한 Actor도 등록은 BeginPlay 전이므로 잠든 채 시작한다). BeginPlay 뒤 등록이면 깨어서 넣는다 | `PrimitiveComponent.cpp:955–961`(`HasActorBegunPlay`) |

- scale 0은 아주 작은 값(`KINDA_SMALL_NUMBER` 대응 상수)으로 보정해 형상을 만든다. UE shipping 빌드는 세 축이 **모두** 0일 때만 보정한다(`PrimitiveComponent.cpp:909–925`). Rocks는 **어느 한 축**이라도 0이면 그 축을 보정한다(Jolt 형상은 0 크기 축을 받지 않는다 — `ScaleHelpers::cMinScale`, UE와 다른 점).
- 실패: `CreateBody` 실패(형상 생성 실패·용량 초과)는 시스템 상태 실패로 돌아가 등록 실패가 된다(ERD26). 형상 크기 0·음수는 실패가 아니라 UE처럼 clamp한다(구 반경·box 반폭 최소 `KindaSmallNumber`, 캡슐 0.1 cm 하한 — `ChaosInterfaceUtils.cpp:139–177`, 7.1.3절). 남는 실패는 비유한 값·용량·할당이다. 용량 한도(`PhysicsSettings::maxBodies`)는 Jolt `PhysicsSystem::Init`의 최대 body 수다.

### 2.3.1 바뀔 때

UE 표(`E/Private/PrimitiveComponentPhysics.cpp`·`SceneComponent.cpp`)를 따른다. "다시 만들기"는 `MarkSystemStateDirty(Physics)`(PreRender 끝 일괄 — ERD25)가 아니라 **즉시** 한다. 이유: 같은 프레임의 다음 질의·이동이 바뀐 충돌을 봐야 한다(UE도 setter 안에서 즉시 재생성한다 — `EnsurePhysicsStateCreated`, `PrimitiveComponent.cpp:998–1005`). 이를 위해 Component에 즉시 다시 만들기 API(`RecreateSystemStateNow(kind)`)를 더한다(엔진 런타임 23.2절 개정 — 7.4절 요청). 실패 정책은 ERD25와 같다: 영속 값 변경은 성공으로 남고, 다시 만들기가 실패하면 그 종류 없이 두고 Warning `SystemStateRecreateFailed`를 남긴다(setter는 성공을 돌려준다 — 데이터는 바뀌었다).

| 바뀌는 것 | 동작 | UE |
|---|---|---|
| 실효 충돌 켬(`SetCollisionEnabled`·profile 이름) | `NoCollision`과 그 밖 사이를 넘으면 만들기·지우기, 그 밖이면 필터만 갱신(`UpdateBody`) | `PrimitiveComponentPhysics.cpp:1376–1393`·`1410–1418` |
| 응답·object type | 필터만 갱신 | 같음(필터 데이터 갱신) |
| `simulatePhysics` | 다시 만들지 않고 Kinematic ↔ Dynamic 전환(`UpdateBody`). 참으로 켜면(그리고 BeginPlay 때 이미 참이면) **부모에서 뗀다**(월드 transform 유지, 떼기의 overlap 갱신은 하지 않음) — 시뮬레이션하는 컴포넌트는 부착될 수 없다. weld 자식에게 `ApplyWeldOnChildren`(2.8절) | `:176–179`, `BodyInstance.cpp:2669–2724`·`2676–2686`, `PrimitiveComponent.cpp:4378–4392` |
| Mobility | 시스템 상태 전부 즉시 다시 만들기(UE는 컴포넌트 재등록 `FComponentReregisterContext` — Rocks는 등록 hook을 다시 부르지 않는다, 7.1.5절). Movable이 되면 자식도 Movable | `SceneComponent.cpp:3098–3116` |
| scale | 다시 만들지 않고 형상 갱신(Jolt `SetShape` — 깨움, 질량은 `updateMassWhenScaleChanges`면 다시 계산) | `SceneComponent.cpp:1865–1883`, `PrimitiveComponent.cpp:1079–1083`, `BodyInstance.cpp:2545–2575` |
| 형상 값(Box 크기 등) | scale과 같은 형상 갱신 | `UBoxComponent::SetBoxExtent` → `UpdateBodyScale(scale, true)` — `BoxComponent.cpp:28–47` |
| `BodySettings`의 질량·감쇠·DOF·CCD 등 | 해당 속성만 갱신(setter와 함께 PH5) | 각 setter |

## 2.4 형상

| 컴포넌트 | 형상 | 비고 |
|---|---|---|
| `BoxComponent` | 상자(반폭 = `boxExtent`) | D2 필드 |
| `SphereComponent` | 구 | |
| `CapsuleComponent` | 캡슐(반높이·반경, 축 로컬 +Z) | UE `CapsuleHalfHeight`는 반구 포함 — 어댑터가 원통 반높이로 바꾼다(`HalfHeight − Radius`) |
| `CylinderComponent`(CM3에서 더함) | 실린더 | CY2 — convex radius 정책(Cylinder 4.2절) |
| `StaticMeshComponent`(D2b 뒤) | 메시 단순 충돌(compound) 또는 complex-as-simple 메시 | 어댑터 경계 4절, 쿠킹 형상은 6.5절 |

- 형상은 `PrimitiveComponent`의 가상 `BuildCollisionGeometry(CollisionGeometryBuilder&)`로 얻는다(World 모듈, 값만 — Jolt 없음). Physics가 Jolt 형상으로 바꾼다.
- 같은 정의의 형상은 장면 안에서 공유한다(Jolt `RefConst<Shape>` — 키는 형상 종류·크기·convex radius 값). 메시 형상은 에셋 lease와 같이 산다(계획 16.4절 수명 순서).

## 2.5 운동 종류

| 조건 | Jolt 운동 종류 | UE |
|---|---|---|
| Mobility ≠ Movable | Static | `IsStaticPhysics()` — `E/Classes/Components/PrimitiveComponent.h:3261`(UE `bStaticWhenNotMoveable` 기본 참) |
| Movable && `simulatePhysics` && 형상이 시뮬레이션 가능(complex-as-simple 아님) | Dynamic | `ShouldInstanceSimulatingPhysics` — `PC/Private/BodyInstanceCore.cpp:27–29` |
| Movable, 그 밖 | Kinematic | `ChaosEngineInterface.cpp:2425–2447` |

- Static ↔ Movable 전환은 재등록(2.3.1절)이라 body를 새로 만든다. 그래서 Jolt `mAllowDynamicOrKinematic`(Static으로 만든 body에만 필요)은 쓰지 않는다. Kinematic ↔ Dynamic 전환은 Jolt가 Movable body에 바로 허용한다(CollisionData 8절 항목 해결).
- Static body에 `simulatePhysics`를 켜면 경고(`SimulateOnStaticBody`)만 남기고 Static으로 둔다(UE `BodyInstance.cpp:2697–2700`).

## 2.6 힘·속도·질량 API

`PrimitiveComponent`에 UE와 같은 이름의 메서드를 둔다(UE `PrimitiveComponent.h:1692–1931`·`2801–2894`). 단위는 Rocks 공개 물리 API 규칙(cm·kg·초, 각속도는 `…InRadians`·`…InDegrees` 쌍 — 어댑터 경계 7절). 뼈 이름 인자(`BoneName`)는 skeletal(CM7) 때 더한다.

| 묶음 | 메서드 |
|---|---|
| 힘·충격량 | `AddImpulse(impulse, velocityChange)`, `AddAngularImpulseInRadians/InDegrees`, `AddImpulseAtLocation`, `AddVelocityChangeImpulseAtLocation`, `AddForce(force, accelerationChange)`, `AddForceAtLocation`, `AddForceAtLocationLocal`, `AddTorqueInRadians/InDegrees`, `AddRadialImpulse`, `AddRadialForce`(`ignoreRadialImpulse`·`ignoreRadialForce`) |
| 속도 | `SetPhysicsLinearVelocity(v, addToCurrent)`, `GetPhysicsLinearVelocity`, `GetPhysicsLinearVelocityAtPoint`, `SetPhysicsAngularVelocityInRadians/InDegrees`, `GetPhysicsAngularVelocity…`, `SetPhysicsMaxAngularVelocity…` |
| 질량 | `GetMass`, `SetMassOverrideInKg(mass, override)`, `SetMassScale`·`GetMassScale`, `GetCenterOfMass`, `SetCenterOfMass(offset)`, `GetInertiaTensor` |
| 그 밖 | `SetSimulatePhysics`, `SetEnableGravity`·`IsGravityEnabled`, `SetLinearDamping`·`GetLinearDamping`, `SetAngularDamping`·`GetAngularDamping`, `SetUseCCD`, `SetGyroscopicTorqueEnabled`·`GetGyroscopicTorqueEnabled`, `SetConstraintMode`, `SetNotifyRigidBodyCollision` |

- 힘·충격량은 Dynamic body에만 효력이 있다(UE와 같다 — Static·Kinematic이면 무시). 물리가 없거나 body가 없으면 무시한다(UE와 같다). 충격량은 다음 step에 속도에 더한다(UE Chaos impulse velocity — 7.1.17절).
- 값 setter는 `BodySettings`(영속)를 바꾸고 body가 있으면 즉시 반영한다.

## 2.7 잠듦·깨움

- `WakeRigidBody()`·`PutRigidBodyToSleep()`·`RigidBodyIsAwake()`(UE `PrimitiveComponentPhysics.cpp:540–545`·`850–855`, `PrimitiveComponent.h:2929–2935`). Dynamic body에만 효력이 있다(`BodyInstance.cpp:3595–3614`).
- 잠듦·깨움 이벤트(`OnComponentWake`·`OnComponentSleep`)는 `generateWakeEvents`가 참인 body만 낸다(UE `PrimitiveComponent.cpp:3609–3657`). Jolt `BodyActivationListener`가 step 안에서 값만 복사하고, 결과 반영 때(3.4절) 게임 lane에서 발행한다. weld 자식에게도 전한다(UE와 같다).

## 2.8 weld

- `autoWeld`이고 Movable이며 질의 전용이 아니고 게임 World이며 부모 primitive에 body가 있으면, 자기 body를 만들지 않고 부모 body의 compound에 자기 형상을 상대 transform으로 더한다(UE `BodyInstance.cpp:1525–1541`·`1940–2027`).
- 부착(`AttachTo`)·떼기(`Detach`)·시스템 상태 파괴 때 weld·unweld 한다(UE `SceneComponent.cpp:2625–2631`·`2683–2685`).
- 루트가 Kinematic이면 자식의 `simulatePhysics`를 끄고 weld하지 않는다(UE `PrimitiveComponentPhysics.cpp:1040–1119`). 루트가 시뮬레이션으로 바뀌면 자식을 붙인다(`ApplyWeldOnChildren`).
- Jolt 구현: 부모 body 형상을 `MutableCompoundShape`로 두고 자식 형상을 추가·제거한다(질량 다시 계산). 질의 결과의 `Component`는 하위 형상 ID로 원래 자식 컴포넌트를 찾는다(UE `ShapeToBodiesMap`).
- weld는 PH6 단계다(7.1절). 그 전에는 `autoWeld`를 무시하고 경고 없이 자기 body를 만든다(설정 값만 보존).
