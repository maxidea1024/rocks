# 엔진 런타임 — 23. 컴포넌트 기반 보강(CM0)

[목차](README.ko.md)

## 23. 컴포넌트 기반 보강(CM0)

설계일 2026-11-24. [컴포넌트 도입](../ComponentCatalog/05-BundleOrder.ko.md) 5절의 첫 묶음 CM0을 구현하는 상세 설계다. 요구는 CR1·CR2·CR3·CR4·CR5·CR6·CR8a·CR11이다([컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) 4.1·4.3절). 착수 시점은 원래 "A6 뒤 화면 마일스톤 첫 단계"였다. 사용자 결정(2026-11-24)으로 S7–S14 뒤로 앞당겼다. CM0은 World 단계 작업이라 렌더·에셋을 기다리지 않는다. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`.

### 23.1 범위와 나누기

| 단계 | 내용 | 요구 |
|---|---|---|
| CM0a | Component 시스템 상태 수명·다시 보내기, 컴포넌트 타입 규칙, 태그, EditorOnly 런타임 규칙 | CR1·CR5(런타임 부분)·CR6 |
| CM0b | SceneComponent: 부착 규칙·socket·절대 Transform·Mobility·가시성·속도·bounds·Transform 변경 hook, 부착 부모 영속 | CR2·CR3 |
| CM0c | `CameraComponent`와 CameraDirector 연결(UE `CalcCamera`) | CR11 |

이미 된 것과 이번에 하지 않는 것:
- CR4(`IsReadyToStart`)는 S9b에서 끝났다(17.4절).
- CR8a는 결정(CD11)이고 구현은 물리·애니메이션 컴포넌트와 함께다. Tick 단계 대응표는 8절, 엔진 단계 자리는 15.4절에 있다.
- CR7(이벤트 slot 표)과 CR8(World 필드 setter·통지 CU5)은 아직 없다. CM0은 이 둘이 없는 자리에서 동작한다(23.4절 Transform 변경 통지는 virtual hook으로 시작한다).
- CR5의 쿠킹 쪽은 에셋 세션 일이다: 클래스 지정자, 패키지 Exports의 EditorOnly 비트, 쿠킹 제외. CM0은 런타임 규칙만 한다(23.2절).
- CR11의 `ViewDesc`는 `RenderData` 모듈의 타입이다([렌더러](../Renderer.ko.md) 3절). 그 모듈은 RS0에서 생긴다. CM0은 `CameraView`까지만 만들고, `ViewDesc`로 바꾸는 일은 RS0의 `WorldIntegration`이 맡는다.

### 23.2 CM0a — Component

**시스템 상태(CR1, UE `CreateRenderState_Concurrent`·`CreatePhysicsState`·`MarkRenderStateDirty`).**

```cpp
enum class SystemStateKind : uint8 { Render, Physics, Audio, Count };

class Component {
protected:
    virtual bool WantsSystemState(SystemStateKind) const noexcept { return false; }
    virtual Result<void> CreateSystemState(SystemStateKind) noexcept { return Success; }
    virtual void DestroySystemState(SystemStateKind) noexcept {}
public:
    bool HasSystemState(SystemStateKind) const noexcept;
    // 다음 갱신 지점(PreRender 단계 끝)에 Destroy → Create로 다시 만든다(UE MarkRenderStateDirty → DoDeferredRenderUpdates).
    Result<void> MarkSystemStateDirty(SystemStateKind) noexcept;
    // 그 자리에서 다시 만든다(UE RecreatePhysicsState·EnsurePhysicsStateCreated).
    Result<void> RecreateSystemStateNow(SystemStateKind) noexcept;
};
```

- **만드는 때.** `OnRegister`가 성공하면 그 바로 뒤에 종류 순서(Render → Physics → Audio)대로, `WantsSystemState`가 참인 것만 만든다. 하나라도 실패하면 이미 만든 것을 역순으로 지우고 `OnUnregister`를 부른 뒤 등록을 실패로 돌린다(부분 등록 해제 — UE `RegisterComponentWithWorld` 실패 경로와 같은 짝 맞춤).
- **지우는 때.** 등록 해제(파괴·Actor 정리)에서 `OnUnregister` 전에 역순으로 지운다. 꺼짐(`OnDisable`)은 시스템 상태를 지우지 않는다. 등록은 유지되기 때문이다. 다만 Render는 가시성으로 감춘다(23.4절).
- **다시 보내기.** `MarkSystemStateDirty`는 World의 갱신 목록에 한 번만 넣는다(중복 무시 — 목록에 들어 있는지는 표시 비트와 따로 둔다. 즉시 다시 만들기가 비트를 지워도 두 번 들어가지 않는다). 처리 지점은 PreRender 단계 끝, 파괴 정리 전이다. UE `SendAllEndOfFrameUpdates`와 같은 자리다. 처리는 그 종류가 있으면 Destroy → Create이고, Create가 실패하면 그 종류가 없는 채로 둔다(Warning 진단 `SystemStateRecreateFailed`). 등록 전이거나 파괴 대기인 Component에서 부르면 아무것도 하지 않고 성공한다.
- **즉시 다시 만들기.** `RecreateSystemStateNow(kind)`는 갱신 지점을 기다리지 않고 그 자리에서 다시 만든다(UE `RecreatePhysicsState` — `ActorComponent.cpp:2594-2605`, `EnsurePhysicsStateCreated` — `PrimitiveComponent.cpp:998-1005`). 같은 프레임의 다음 질의·이동이 바뀐 상태를 봐야 하는 setter(충돌 켜기·형상 바꾸기·simulate 켜기 — PhysicsModule 2.3.1절)가 부른다.
  - 그 종류가 있으면 Destroy하고, `WantsSystemState(kind)`가 참이면 Create한다. 그래서 없던 상태도 만든다(충돌을 켜서 원하게 된 경우 — UE `CreatePhysicsState`도 `ShouldCreatePhysicsState`를 본다). 원하지 않게 되었으면 지우기만 한다. `MarkSystemStateDirty`는 있는 상태만 다시 만드는 것과 다르다.
  - 그 종류의 대기 중 표시(`MarkSystemStateDirty`)는 지운다(방금 새로 만들었다). 갱신 지점이 그 Component를 처리하는 중이고 그 종류가 아직 남았으면 그것도 지운다(두 번 하지 않는다). 다른 종류 표시는 그대로다.
  - Create가 실패하면 그 종류 없이 두고 Warning `SystemStateRecreateFailed`를 남긴 뒤 그 오류를 돌려준다. 값을 바꾼 setter는 그 오류를 무시하고 성공을 돌려준다(영속 값은 바뀌었다 — ERD25와 같은 정책).
  - 등록 전·해제 뒤·파괴 대기면 아무것도 하지 않고 성공한다(등록이 그때의 값으로 만든다).
  - hook 안 규칙: Create·Destroy는 hook 범위(그 안에서 World 진입점 Initialize·Tick 등은 `Busy`)에서 부른다. 같은 Component의 같은 종류를 그 Create·Destroy 안에서 다시 부르면 `Busy`다(무한 재귀·이중 생성 방지). 갱신 지점(`MarkSystemStateDirty` 처리)과 등록·해제가 부르는 hook도 같다(등록·해제 중에는 `registered_`가 거짓이라 아무 일 없음). 다른 종류는 부를 수 있다.
  - 범위 밖 종류는 `InvalidArgument`다.
- **처리 중 표시.** 다시 만들기 hook(Create·Destroy)이 다른 종류나 다른 Component를 표시하면, 그것은 다음 갱신 지점(다음 프레임)에 처리한다. 종류 사이의 표시가 순환해도 한 지점에서 끝없이 돌지 않게 하려는 것이다.
- **한계 — 우회 파괴.** World API를 거치지 않고 `Object::Destroy`로 파괴된 Component는 World가 다음 정리에서 장부와 간선만 고친다(7.4절). 객체가 이미 죽어 hook을 부를 수 없으므로 시스템 상태를 지우지 못한다. 시스템 상태를 갖는 Component는 반드시 World API(`DestroyComponent`·Actor 파괴)로 지운다. 시스템 쪽 장부(렌더·물리)는 자기 쪽 handle 만료로 정리한다. 이 계약은 RS0·물리 연결 때 그 시스템 문서에 함께 적는다.
- **대량 등록 묶음.** CR1의 대량 등록 묶음(UE `FRegisterComponentContext`)은 등록이 많은 렌더 경로가 생길 때(CM1a·RS0) 측정과 함께 더한다. 지금은 Component마다 바로 만든다.
- 시스템 상태를 갖는 Component는 아직 없다. 시험용 파생으로 계약만 확인한다. 실제 사용처는 CM1a(정적 메시)·CM3(충돌)·CM4(오디오)다.

**타입 규칙(CR6).** 규칙은 Component 타입의 정적 정보다. Unity `DisallowMultipleComponent`·`RequireComponent`에 대응하고 UE에는 없다.

```cpp
struct ComponentTypeRules {
    bool unique = false;                 // Actor당 하나
    Span<const TypeId> required;         // 먼저 있어야 하는 Component 타입(파생 포함)
    Span<const TypeId> excluded;         // 함께 있을 수 없는 타입(파생 포함)
};
// 파생이 재정의한다(인스턴스 virtual — World는 만든 직후 등록 전에 묻는다).
virtual ComponentTypeRules TypeRules() const noexcept { return {}; }
```

- `AddComponent`는 객체를 만든 뒤, 등록하기 전에 소유 Actor의 기존 Component(파괴 대기 제외)에 규칙을 대 본다.
  - 새 타입이 `unique`이고 기존이 그 타입이나 그 파생이면, 또는 기존이 `unique`이고 새 것이 기존 타입이나 그 파생이면 `AlreadyExists`(기반 타입과는 겹치지 않는다)
  - `required`의 타입이 하나라도 없으면 `DependencyFailed`
  - 새 Component의 `excluded`나 기존 Component의 `excluded`에 걸리면 `InvalidArgument`
  - 실패하면 만든 객체를 정리하고 hook을 하나도 부르지 않는다.
- 필수 동반은 자동으로 더하지 않는다. Unity는 자동으로 더하지만 Rocks는 거부한다. 추가 순서가 명시적이고, 자동 추가가 다시 규칙을 부르는 연쇄를 만들지 않게 하려는 것이다. 순서가 필요한 쪽(prefab 원형)이 먼저 더한다.
- 이미 있는 Component를 지워 `required`가 깨지는 경우는 막지 않는다. UE·Unity도 파괴를 막지 않는다. 의존하는 쪽이 `OnTick` 등에서 직접 확인한다.

**태그·EditorOnly(CR5 런타임).**
- **태그는 미룬다.** UE `ComponentTags`(`TArray<FName>`)에 대응하는 태그는 `Name`이 필요하다. 그런데 World Registry에는 NamePool이 없다(`WorldDesc::objects.names` 기본 null — `Name` 필드는 pool 없는 문맥에서 만들 수 없다). 태그 사전(EIP 20.1절)도 계획만 있다. 그래서 태그는 World 이름 기반(엔진 NamePool을 World에 빌려주거나 태그 사전)이 생길 때 그 기반과 함께 넣는다. 영속 필드라 Component 생성자가 allocator를 받게 바뀌는데, 그 변경도 그때 한다(Component 파생 생성자 전부).
- `virtual bool IsEditorOnly() const`(UE `bIsEditorOnly`). Game·Tryout World에서 `AddComponent`하면 만들지 않고 `Unsupported`를 돌려준다. Editor·Preview World에서는 만든다. 쿠킹 제외는 에셋 세션이 클래스 지정자로 한다(위). 런타임은 그 결과와 별개로 같은 판정을 한 번 더 한다.

### 23.3 CM0b — SceneComponent

**부착 규칙(UE `FAttachmentTransformRules` — `E/Classes/Engine/EngineTypes.h:62`).**

```cpp
enum class AttachmentRule : uint8 { KeepRelative, KeepWorld, SnapToTarget };
struct AttachmentRules {
    AttachmentRule location = AttachmentRule::KeepRelative;
    AttachmentRule rotation = AttachmentRule::KeepRelative;
    AttachmentRule scale = AttachmentRule::KeepRelative;
    static AttachmentRules KeepRelativeTransform() noexcept;
    static AttachmentRules KeepWorldTransform() noexcept;
    static AttachmentRules SnapToTargetNotIncludingScale() noexcept;   // 위치·회전 Snap, scale KeepWorld
    static AttachmentRules SnapToTargetIncludingScale() noexcept;
};
Result<void> AttachTo(ObjectHandle parent, const AttachmentRules& rules = {}, Name socket = {}) noexcept;
Result<void> Detach(AttachmentRule rule = AttachmentRule::KeepWorld) noexcept;   // UE DetachFromComponent(KeepWorld 기본)
```

- KeepRelative는 상대 Transform을 그대로 둔다. KeepWorld는 붙이기 전 월드 Transform을 유지하도록 상대를 다시 계산한다(새 부모 socket Transform의 역 × 현재 월드). SnapToTarget은 그 성분을 0(위치)·단위(회전)·1(scale)로 둔다.
- KeepWorld는 정확한 역변환을 먼저 쓴다. Rocks 월드 행렬과 짝이 맞아 월드 Transform을 그대로 지키기 때문이다. 상대 값이 TRS로 표현되지 않으면(축에 맞지 않은 비균일 부모 — shear) UE `GetRelativeTransform` 식(회전 Qp⁻¹·Qc, scale Sc÷Sp, 위치 Qp⁻¹(Tc−Tp)÷Sp)으로 계산한다. 이 경우 월드 scale은 UE처럼 근사로 지켜진다. 절대 성분은 현재 월드 값을 상대 값으로 둔다(UE rule switch).
- Static Component는 Static 부모에만 붙는다(UE `SceneComponent.cpp:2381` — `InvalidArgument`). `Detach`의 규칙은 KeepRelative·KeepWorld뿐이다(UE `EDetachmentRule`).
- 성분마다 따로 적용한다. scale만 KeepWorld이고 회전은 아니면(예: SnapToTargetNotIncludingScale), scale은 성분별로 월드 scale ÷ 부모 socket scale이다(UE `FTransform::GetRelativeTransform`). 정확한 역변환에서 분해한 상대 scale은 그 분해의 회전과 짝일 때만 월드 scale을 지키기 때문이다. KeepWorld 계산은 TRS 합성으로 한다(부모가 비균일 scale이고 회전이 있으면 shear를 버린다 — UE `FTransform` 곱과 같다). 표현할 수 없으면(scale 0) 부착이 `InvalidArgument`로 실패하고 아무것도 바꾸지 않는다.
- `AttachTo(parent)`의 기본 규칙은 지금처럼 KeepRelative다. 옛 호출의 의미는 그대로다.

**socket.**
- `virtual bool HasSocket(Name) const`(기본 거짓)와 `virtual Result<Transform> GetSocketTransform(Name socket, TransformSpace space) const`가 있다. `space`는 `World`·`Component`(socket의 부모 Component 기준)다. 이름이 비면 Component 자신이다. 기본 구현은 비거나 없는 이름에 자신의 Transform을 돌려준다(UE `GetSocketTransform`도 없는 socket이면 Component Transform이다).
- 메시 계열이 재정의한다(CM1a 정적 메시 socket, CM10 skeletal bone).
- 부착 socket 이름은 `AttachSocketName()`이다. 월드 행렬 = 상대 × 부모의 socket Transform(Component 공간) × 부모 월드다. socket 이름이 비어도 부모의 `GetSocketTransform(빈 이름)`을 쓴다(UE와 같다 — 기본 구현은 단위라 결과가 같고, SpringArm처럼 이름 없는 자식을 옮기는 부모가 재정의한다, [26장](26-MovementComponents.ko.md) 26.2절).

**절대 Transform(UE `bAbsoluteLocation`·`Rotation`·`Scale`).** `SetAbsolute(location, rotation, scale)`. 켜진 성분은 부모를 무시하고 상대 값을 월드 값으로 쓴다. 월드 행렬은 TRS로 합성한다: 위치는 절대면 상대 위치, 아니면 부모로 변환한 위치. 회전·scale도 같다. 셋 다 꺼져 있으면 지금처럼 행렬 곱 그대로다(shear 보존). 곱이 TRS로 분해되지 않으면(shear) 부모 TRS와 성분별로 합성한 뒤(UE `ComposeTransforms`) 켜진 성분을 덮어쓴다. `SetAbsolute`도 Mobility 검사를 거친다.

**Mobility(UE `EComponentMobility`).** `Mobility::Static`·`Stationary`·`Movable`(기본 Movable — UE `USceneComponent` 기본도 Movable). Static이나 Stationary Component에 다음 일을 하면 거부한다.
- 대상 조건: 등록되어 있고 Game·Tryout World이며 소유 Actor가 Initialized 이후다.
- 막는 일: 상대 Transform 변경, AttachTo, Detach.
- 결과: `InvalidArgument`와 Warning 진단 `StaticComponentMoved`(UE `CheckStaticMobilityAndWarn` — `E/Private/Components/SceneComponent.cpp:3383`).
- Spawn Transform 적용과 OnAwake 안의 설정은 막지 않는다. Actor가 아직 초기화되기 전이다.

**가시성(UE `bVisible`·`bHiddenInGame`).**
- `SetVisibility(bool visible, bool propagateToChildren = false)`·`SetHiddenInGame(bool hidden, bool propagateToChildren = false)`(UE와 같은 서명).
- `IsVisible()` = visible이고 World가 Game·Tryout이면 hiddenInGame도 아님. 편집 World는 hiddenInGame을 보지 않는다(UE `ShouldRender`와 같다).
- 바뀌면 `OnVisibilityChanged()` hook을 부르고 Render 시스템 상태가 있으면 다시 보내기를 표시한다.

**속도.** `ComponentVelocity()`·`SetComponentVelocity(Vec3d)`(cm/s, UE `ComponentVelocity`). 이동 컴포넌트가 쓴다. 엔진은 Transform에서 추정하지 않는다(UE도 이동 컴포넌트가 채운다).

**bounds.**
- `ComponentBounds{Vec3d origin; Vec3d boxExtent; double sphereRadius;}`(UE `FBoxSphereBounds`).
- `virtual ComponentBounds CalcBounds(const Transform& world) const`. 기본은 월드 위치의 점이다(extent 0, 반지름 0 — UE `USceneComponent::CalcBounds`와 같다).
- `Bounds()`는 캐시다. 월드 Transform이 바뀌면 다음 `Bounds()` 호출에서 다시 계산한다(dirty).

**Transform 변경 통지.**
- `virtual void OnTransformUpdated()`(UE `OnUpdateTransform`). 자신이나 조상의 Transform·부착이 바뀌면, 그 변경 호출이 끝날 때 바뀐 Component마다 한 번 부른다(자신 → 자손 순).
- Render 시스템 상태가 있으면 다시 보내기를 표시한다. 구독형 통지(이벤트)는 CR7이 생기면 이 자리에 붙인다.

**부착 부모 영속(CR3).**
- `ROCKS_PROPERTY(ReadOnly, Name = "attachParent") WeakObjectRef<SceneComponent>`와 `ROCKS_PROPERTY(ReadOnly, Name = "attachSocket") Name`을 더한다.
- 런타임 `parent_` handle과 같은 값을 둔다. 쓰는 것은 AttachTo·Detach뿐이다.
- 저장된 그래프는 이 필드로 계층을 다시 만든다. World 편입(4.6절 — 레벨 패키지 단계)이 읽어 부착을 복원한다. 편입이 아직 없으므로 CM0은 저장·로드 왕복에서 값이 보존되는지만 확인한다.
- 절대 플래그·Mobility·visible·hiddenInGame은 `ROCKS_PROPERTY(ReadOnly)`로 영속한다(UE도 UPROPERTY). 변경은 setter로만 한다(CR8 전).

### 23.4 CM0c — CameraComponent

`CameraComponent : SceneComponent`(World 모듈, UE `UCameraComponent` — `E/Classes/Camera/CameraComponent.h`).

| 필드(영속) | 기본 | UE |
|---|---|---|
| `fieldOfView` | 90(도, 수평) | `FieldOfView` |
| `projection` | `Perspective`·`Orthographic` | `ProjectionMode` |
| `orthoWidth` | 512(cm) | `OrthoWidth` |
| `aspectRatio`·`constrainAspectRatio` | 16/9·거짓 | `AspectRatio`·`bConstrainAspectRatio` |
| `usePawnControlRotation` | 거짓 | `bUsePawnControlRotation` |

- `GetCameraView(double dt, CameraViewInfo& out)`는 위치·회전을 Component 월드 Transform으로, 시야를 필드로 채운다(UE `GetCameraView` — `E/Private/Camera/CameraComponent.cpp:418`). `CameraViewInfo`는 위치·회전·fov·투영·orthoWidth·aspect·constrain을 담는 World 쪽 값이고, CameraDirector의 `CameraView`가 이를 받는다.
- `usePawnControlRotation`이면 회전을 소유 Pawn의 controlRotation으로 바꾼다. World 모듈은 Pawn을 모른다. 그래서 이 판정은 CameraDirector(Gameplay)가 한다. `GetCameraView`는 표시만 넘긴다.
- **CameraDirector 연결.** view target Actor에 켜진(enabled) CameraComponent가 있으면 첫 번째 것의 `GetCameraView`를 쓴다. 첫 번째는 Actor Component 순서다(UE `CalcCamera`의 `bFindCameraComponentWhenViewTarget` — `E/Private/Actor.cpp:3688–3700`). 없으면 지금 규칙(root 위치·controlRotation)이다. `CameraView`에 투영·orthoWidth·aspect를 더한다.
- `ViewDesc` 생성은 RS0(`WorldIntegration`)이다(23.1절).

### 23.5 결정

| ID | 항목 | 결정 | 근거 |
|---|---|---|---|
| ERD25 | 시스템 상태 종류 | Render·Physics·Audio 고정 enum, 등록 직후 생성·해제 직전 삭제, 다시 보내기는 PreRender 끝 | UE 등록 안 생성과 프레임 끝 갱신 |
| ERD26 | 시스템 상태 실패 | 등록 실패로 되돌림(역순 삭제 → OnUnregister) | CR1 부분 등록 해제 |
| ERD27 | 타입 규칙 | 거부(자동 추가 없음), 파괴는 막지 않음 | 명시적 순서, Unity와 다른 점 문서화 |
| ERD28 | EditorOnly | Game·Tryout World에서 AddComponent가 Unsupported | UE 게임 World에 EditorOnly 없음 |
| ERD29 | 부착 규칙 | UE 세 규칙·성분별, KeepWorld는 TRS 합성 | UE `FAttachmentTransformRules` |
| ERD30 | Mobility | Static·Stationary 이동 거부 + Warning(게임 World, 초기화 뒤) | UE `CheckStaticMobilityAndWarn` |
| ERD31 | 절대 Transform | 켜진 성분만 TRS 합성, 모두 꺼지면 행렬 곱(shear 보존) | UE 성분별 의미 + Rocks 기존 정확도 |
| ERD32 | Transform 통지 | virtual hook, 구독형은 CR7 뒤 | CR7 없음 |
| ERD33 | 카메라 | CameraComponent는 World, Pawn 회전 판정은 CameraDirector | 3절 의존 방향 |

### 23.6 시험(완료 조건 — 컴포넌트 도입 5절)

1. 수명 순서 헤드리스 시험이다. 등록 → Awake → Enable → Start → Disable → Destroy → 해제의 순서를 단언한다. 시스템 상태 생성·삭제가 종류마다 한 번씩인지, 등록 실패 시 부분 해제되는지, 다시 보내기의 중복·파괴 대기·실패 처리를 본다.
2. 타입 규칙 셋(unique·requires·excludes)을 파생 포함으로 시험한다. 실패 시 hook이 없는지와, EditorOnly의 World 종류별 동작을 본다.
3. 부착 규칙 셋 × 절대 플래그 셋 × 부모 비균일 scale 조합을 돌린다. socket 부착, Mobility 위반(Static 이동) 진단, 가시성 전파, bounds 캐시, 통지 횟수, 영속 왕복을 본다.
4. CameraComponent 시야 값과 CameraDirector가 컴포넌트를 우선하는지, `usePawnControlRotation`, 꺼진 카메라를 건너뛰는지를 본다.
5. UE 대조표(컴포넌트 도입 7절)의 `UActorComponent`·`USceneComponent`·`UCameraComponent` 속성 중 들이지 않은 것은 사유와 함께 23.7절에 적는다.

### 23.7 들이지 않은 UE 속성(사유)

| UE | 사유 |
|---|---|
| `bAutoActivate`·`Activate`/`Deactivate` | Rocks는 `enabled`(OnEnable·OnDisable)로 대신한다(7절) |
| `bWantsInitializeComponent`·`InitializeComponent` | Rocks `OnAwake`가 같은 자리다 |
| `PrimaryComponentTick` 세부(`TickGroup` 등) | Tick 설정·단계는 8·15절 |
| `bReplicates`·`GetLifetimeReplicatedProps` | 네트워크 단계(CR24) |
| `AssetUserData` | 에셋 사용자 데이터 체계가 생길 때 |
| `bVisualizeComponent`·`bIsEditorOnly` 편집 표시 | 디버그 표시(CR23 — RS13) |
| `PhysicsVolume`·`bShouldUpdatePhysicsVolume` | 물리 모듈(CR16) |
| `DetailMode`·`bComputeFastLocalBounds`·`bComputedBoundsOnceForGame` | 렌더 품질·최적화 — RS 단계 측정 뒤 |
| `bUseAttachParentBound` | bounds 사용자(렌더·culling)가 생길 때(RS0) |
| CameraComponent `PostProcessSettings`·`PostProcessBlendWeight` | 후처리(RS6·CM15) |
| CameraComponent `bLockToHmd`·XR | XR 범위 밖 |
| CameraComponent `OrthoNearClipPlane`·`OrthoFarClipPlane`·`bAutoCalculateOrthoPlanes` | 직교 투영 렌더 경로가 생길 때(RS0 이후) |
