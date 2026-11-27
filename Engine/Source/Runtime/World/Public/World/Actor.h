#pragma once

#include <Core/Containers/Span.h>
#include <Core/Math/Rotator.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>
#include <Object/NativeClass.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <World/Component.h>
#include <World/Physics/HitResult.h>
#include <World/WorldTypes.h>

namespace rocks {

namespace detail {
struct LevelGraphTestAccess;
}

class SceneComponent;

// World에 배치되는 게임플레이 객체의 기반이다. World가 생성하고 Level이 소유하며, Actor는 자기
// Component를 소유한다.
//
// 사용 예(리플렉션 base(Actor)·SceneComponent를 쓰는 TU는 그 타입의 생성 헤더를 먼저 include한다):
//   #include <World/Actor.generated.h>
//
//   class Spinner final : public Actor {
//       ROCKS_NATIVE_TYPE(Spinner, Actor, 0x...ull, 0x...ull)
//   public:
//       explicit Spinner(Allocator& allocator) noexcept : Actor(allocator) {
//           PrimaryTick().tick = true;            // OnTick을 받는다
//       }
//       Result<void> OnAwake() noexcept override {
//           auto root = GetWorld().AddComponent<SceneComponent>(Handle());
//           if (!root) {
//               return Result<void>::Failure(root.Error());
//           }
//           return SetRootComponent(root.Value());
//       }
//       void OnTick(const TickContext& context) noexcept override {
//           angle += context.deltaSeconds;
//       }
//       double angle = 0.0;
//   };
//
//   RegisterNativeClass<Spinner>(types);                 // World 기본 타입 등록 뒤
//   auto spawned = world.SpawnActor<Spinner>();          // OnAwake·OnEnable까지 실행
//   if (spawned) {
//       auto destroyed = world.DestroyActor(spawned.Value());   // 프레임 끝에 OnDestroy·회수
//   }
//
// 리플렉션(Docs/Design/ReflectionFoundations.ko.md 9.1절): WorldOnly 리플렉션 클래스다. 영속
// 필드는 components(소유 Component 목록 — 강한 GC 간선), rootComponent(대표 SceneComponent —
// 늘 components 안에 있는 강한 참조), enabled이고 모두 ReadOnly다(경로·리플렉션 쓰기가 World 장부 —
// ActorRecord·hook·Tick 등록 — 를 우회하지 않게)와 tickSettings(Tick 설정 — 생성자 값은 클래스
// 기본값, 시작 때 한 번 등록하므로 시작 뒤 변경은 반영하지 않는다)다. World 포인터·소속 Level·
// Tick 등록 handle은 ROCKS_NATIVE(저장하지 않는 런타임 상태)다. 영속 상태를 더하는 파생은
// ROCKS_CLASS(WorldOnly 상속)로 리플렉션 헤더에 선언하고, 영속 상태가 없는 런타임 전용 파생은
// 위처럼 ROCKS_NATIVE_TYPE으로 둔다(그 객체가 든 그래프는 저장할 수 없다).
//
// 소유: Level이 Actor를, Actor가 자기 Component를 강한 GC 간선 목록(ObjectRefArray)으로 소유한다.
// native 파생 클래스가 다른 객체로의 강한 간선(ObjectRef·ObjectRefArray)을 더하면
// `void Trace(TraceVisitor&) const noexcept`를 선언해 자기 필드만 방문한다. 리플렉션 기반(Actor)의
// 간선은 RegisterNativeClass가 생성 Trace로 방문한다(Super::Trace를 부르지 않는다 — Actor에는 손
// Trace가 없다).
//
// 수명(ActorState 참고):
// - 생성자에서는 World에 연결되기 전이다. GetWorld()·Handle() 기반 API를 쓰지 않는다. 기본값과 Tick
//   설정만 채운다. 실패할 수 있는 초기화는 OnAwake에서 한다.
// - hook 순서: OnAwake -> OnEnable(활성일 때) -> OnStart(시작 단계, 활성일 때) -> OnTick… ->
//   OnDisable(활성이었을 때) -> OnDestroy. OnDestroy는 OnAwake가 성공한 Actor에만 호출한다.
//   Component와 섞인 전체 순서는 World 설명을 참고한다.
// - 소멸자는 저장소 회수(GC) 시점에 불린다. World·다른 객체·에셋 관리자를 호출하지 않는다. 외부
//   자원은 OnDestroy에서 반환한다.
// - 객체 포인터는 World가 준 Borrow 범위(hook 호출 중, World::With 콜백 안)에서만 쓴다. 다른 객체는
//   ObjectHandle로 보관하고 필요할 때 World::With로 접근한다.
// - World 장부 밖 객체(그래프 로드 등 World::SpawnActor 밖에서 만든 Actor — HasWorld()가 거짓)는
//   레벨 편입(EngineRuntime 문서 4.6절) 전까지 World API를 쓰지 못한다: SetRootComponent·Destroy는
//   InvalidArgument다.
//
// 스레드: World의 게임 스레드에서만 접근한다.
ROCKS_CLASS(WorldOnly, ScriptExtensible)
class ROCKS_WORLD_API Actor : public Object {
    ROCKS_GENERATED_BODY()

public:
    // allocator는 Registry allocator다(Object 생성 시 주입). 파생 클래스 생성자도 받아서 넘긴다.
    explicit Actor(Allocator& allocator) noexcept : components_(allocator), tags_(allocator) {
    }

    virtual ~Actor() noexcept = default;

    // 소유한 Component의 handle 목록이다(추가 순서). 이 Actor의 Borrow 범위에서만 쓰며 Component
    // 추가·제거 후에는 다시 얻는다. World가 만든 목록에는 null이 없지만 로드된 목록에는 있을 수
    // 있다(Required는 로드에서 진단만 한다).
    Span<const ObjectHandle> Components() const noexcept {
        return components_.View();
    }

    // 소속 World다. Spawn이 연결한 뒤에만 유효하다(생성자 안이나 World 장부 밖 객체에서는 Fatal —
    // 확인이 필요하면 HasWorld).
    World& GetWorld() const noexcept;

    bool HasWorld() const noexcept {
        return world_ != nullptr;
    }

    // 소속 Level이다(Spawn 시 정해지며 바뀌지 않는다). World 연결 전이면 null handle이다.
    ObjectHandle GetLevel() const noexcept {
        return level_;
    }

    // World에 기록된 현재 수명 단계다. World 연결 전이면 Constructed다.
    ActorState GetState() const noexcept;

    // 활성 여부다. 기본은 활성이다. 비활성 Actor는 Tick하지 않는다. Initialized 이후의 변경은
    // OnEnable/OnDisable을 부르고 Component에도 전파한다.
    bool IsEnabled() const noexcept {
        return enabled_;
    }

    Result<void> SetEnabled(bool enabled) noexcept;

    // 이 Actor를 파괴 대기 상태로 만든다(World::DestroyActor와 같다). Object::Destroy를 가린다:
    // 저장소 파괴를 직접 요청하면 World가 OnDestroy·Component 정리를 할 수 없기 때문이다. World
    // 장부 밖 객체는 InvalidArgument다.
    Result<void> Destroy() noexcept;

    // 소유 Actor다(UE AActor::Owner — 게임 논리 참조이며 수명 소유가 아니다. 17.3절 ERD15). 없거나
    // 파괴됐으면 null·stale handle이다.
    ObjectHandle Owner() const noexcept {
        return owner_;
    }

    // 소유 Actor를 바꾼다. null은 소유자 없음이다. 이 World의 살아 있는 Actor가 아니면 StaleHandle,
    // 소유 고리(자기 자신 포함)는 InvalidArgument(Error 로그)다. World 장부 밖 객체는
    // InvalidArgument다.
    Result<void> SetOwner(ObjectHandle owner) noexcept;

    // 이 Actor를 만든 원인(보통 Pawn — UE Instigator)이다. 게임 논리 참조다.
    ObjectHandle Instigator() const noexcept {
        return instigator_;
    }

    Result<void> SetInstigator(ObjectHandle instigator) noexcept;

    // seconds 게임 시간 뒤 파괴한다(UE SetLifeSpan — 17.5절 ERD16, 사유 Destroyed). 0 이하는 해제,
    // 다시 부르면 새 값으로 바꾼다. 만료는 LateUpdate 시작 엔진 단계 Timers에서 판정한다(pause
    // 중에는 게임 시계가 멈춰 흐르지 않는다). 비유한 값은 InvalidArgument, World 장부 밖은
    // InvalidArgument다.
    Result<void> SetLifeSpan(double seconds) noexcept;

    // ---- 태그(Docs/Design/EngineRuntime 31.4절 — UE AActor::Tags) ----

    // tag가 들어 있는지다. None은 늘 거짓이다(UE ActorHasTag).
    bool HasTag(Name tag) const noexcept;

    // 태그 목록이다(순서·중복 그대로). 바꾸면 무효다.
    Span<const Name> Tags() const noexcept {
        return tags_.View();
    }

    // tag를 더한다(이미 있으면 아무것도 하지 않고 성공). tag는 이 객체 Registry의 이름 pool에서
    // 발급한 토큰이어야 한다(엔진 World는 Engine::Names()).
    //
    // 사용 예:
    //   auto enemy = engine.Names().TryIntern("Enemy");
    //   if (enemy) {
    //       (void)value.AddTag(enemy.Value());
    //   }
    //
    // 오류(값 그대로): World 장부 밖 객체 InvalidArgument, 파괴 대기 Destroyed, None·다른 pool·옛
    // 토큰 InvalidArgument, Registry에 이름 pool이 없으면 Unsupported, 할당 실패 OutOfMemory다.
    // 스레드: World의 게임 스레드다.
    Result<void> AddTag(Name tag) noexcept;

    // 같은 tag를 모두 뺀다(없으면 아무것도 하지 않고 성공). 오류: 장부 밖 InvalidArgument, 파괴
    // 대기 Destroyed, None InvalidArgument다.
    Result<void> RemoveTag(Name tag) noexcept;

    // 태그 목록을 통째로 바꾼다(중복은 그대로 둔다). 원소마다 AddTag와 같은 검사를 하고, 하나라도
    // 거부하면 값 그대로다. 오류는 AddTag와 같다.
    Result<void> SetTags(Span<const Name> tags) noexcept;

    // 대표 SceneComponent다. 없으면 null handle이다.
    ObjectHandle RootComponent() const noexcept {
        return rootComponent_.Handle();
    }

    // 이 Actor가 소유한 SceneComponent를 대표로 지정한다(Registry SetReference — Marking 중이면
    // 삽입 장벽을 거친다). 다른 Actor의 Component나 SceneComponent가 아니면 InvalidArgument다.
    // null handle은 대표를 비운다. World 장부 밖 객체는 InvalidArgument다(대표를 바꿀 Registry
    // 문맥이 없다).
    Result<void> SetRootComponent(ObjectHandle component) noexcept;

    // Tick 설정이다. OnStart 시점에 한 번 등록한다(이후 변경은 SetTickEnabled만 반영).
    const TickSettings& GetTickSettings() const noexcept {
        return tickSettings_;
    }

    // Tick 실행을 켜거나 끈다(활성 상태와 별개다. 비활성 Actor는 이 값과 관계없이 Tick하지 않는다).
    // 시작 전에 끄면 시작 시 꺼진 상태로 등록된다. 등록할 hook 종류는 바꾸지 않는다.
    Result<void> SetTickEnabled(bool enabled) noexcept;

    // 게임플레이 시작에 필요한 의존(비동기 에셋 등)이 준비됐는지다. World는 Initialized Actor에
    // 대해 프레임마다 이 값을 확인하고, 참이 된 프레임에 OnStart를 부른다. 게임 스레드를 막지
    // 않도록 기다리는 대신 거짓을 반환한다. 기본은 참이다.
    virtual bool IsReadyToStart() const noexcept {
        return true;
    }

    // 바라보는 회전(도)을 줄 수 있으면 out에 채우고 참이다(UE APawn::GetViewRotation의 자리 —
    // Docs/Design/EngineRuntime 26.2절). 기본은 거짓이다. Gameplay Pawn이 control rotation으로
    // 재정의하고, SpringArm의 usePawnControlRotation이 쓴다(World는 Pawn을 모른다).
    virtual bool TryGetViewRotation(Rotator<double>& out) noexcept {
        (void)out;
        return false;
    }

    // 초기화 hook이다. 실패를 반환하면 Spawn이 실패하고 Actor는 OnDestroy 없이 정리된다.
    virtual Result<void> OnAwake() noexcept {
        return Result<void>::Success();
    }

    virtual void OnEnable() noexcept {
    }

    virtual void OnStart() noexcept {
    }

    virtual void OnTick(const TickContext&) noexcept {
    }

    virtual void OnFixedTick(const TickContext&) noexcept {
    }

    virtual void OnLateTick(const TickContext&) noexcept {
    }

    virtual void OnDisable() noexcept {
    }

    // 막는 hit hook이다(UE NotifyHit — Docs/Design/PhysicsModule 5.4절·7.1.10절). myComponent는 이
    // Actor의 맞은(또는 움직인) 컴포넌트, otherActor·otherComponent는 상대다. selfMoved는 이
    // Actor의 컴포넌트가 움직여 생긴 hit인지다. 상대가 움직인 hit(selfMoved 거짓)는 hit의
    // normal·impactNormal이 뒤집혀 상대가 이 Actor를 미는 방향이다(UE GetReversedHit).
    // normalImpulse는 시뮬레이션 접촉의 solver 충격량(kg·cm/s — PH4c)이고 sweep 이동 hit는 0이다.
    // 파괴 대기 Actor에는 부르지 않는다.
    virtual void OnHit(ObjectHandle myComponent, ObjectHandle otherActor,
                       ObjectHandle otherComponent, bool selfMoved, const Vec3d& normalImpulse,
                       const HitResult& hit) noexcept {
        (void)myComponent;
        (void)otherActor;
        (void)otherComponent;
        (void)selfMoved;
        (void)normalImpulse;
        (void)hit;
    }

    // 정리 hook이다. 다른 Actor 파괴·Spawn 요청은 허용한다(같은 프레임 정리 단계에서 처리된다).
    // reason은 정리 이유다(17.2절). OnStart를 받지 못했어도 OnAwake가 성공했으면 받는다.
    virtual void OnDestroy(EndPlayReason) noexcept {
    }

protected:
    // 시작 전 Tick 설정을 바꾼다. 생성자나 OnAwake에서 쓴다.
    TickSettings& PrimaryTick() noexcept {
        return tickSettings_;
    }

private:
    friend class World;
    // 편입 시험이 장부 밖 그래프를 손상시킨다(Private/LevelGraphTestAccess.h — Tests/ 전용).
    friend struct detail::LevelGraphTestAccess;

protected:
    // --- 프레임워크 내부 hook(Docs/Design/EngineRuntime 17.8절 ERD19) ---
    // Gameplay 모듈의 프레임워크 클래스(GameDirector·GameState·PlayerState·Controller·Pawn·
    // CameraDirector)와 엔진 Actor 클래스(광원 Actor 등 — 24.2.5절)만 재정의하고 final로 닫는다.
    // 게임 코드는 OnAwake·OnDestroy를 쓴다.

    // Component OnRegister 뒤, Actor OnAwake 전(UE PreInitializeComponents). 실패는 OnAwake 실패와
    // 같다(Spawn 실패, OnDestroy 없음).
    virtual Result<void> OnFrameworkPreAwake() noexcept {
        return Result<void>::Success();
    }

    // 모든 Component OnAwake 뒤, OnEnable 전(UE PostInitializeComponents). 실패하면 Spawn이
    // 실패하고 OnAwake가 성공했으므로 정리에서 OnDestroy를 받는다.
    virtual Result<void> OnFrameworkPostAwake() noexcept {
        return Result<void>::Success();
    }

    // 정리의 OnDisable 전(UE Destroyed·EndPlay의 엔진 부분). OnAwake가 성공한 Actor만 받는다.
    virtual void OnFrameworkEndPlay(EndPlayReason) noexcept {
    }

private:
    ROCKS_NATIVE() World* world_ = nullptr;
    // actors 포함 관계에서 유도되는 값이다(편입이 다시 채운다).
    ROCKS_NATIVE() ObjectHandle level_;
    ROCKS_PROPERTY(ReadOnly, Required, Name = "components", ScriptReadOnly)
    ObjectRefArray<Component> components_;
    // UE AActor::RootComponent처럼 영속 강한 참조다. 늘 components_ 안의 객체라 새 생존 간선을
    // 만들지 않는다. 쓰기는 Registry SetReference로만 한다(Actor·World 구현).
    ROCKS_PROPERTY(ReadOnly, Name = "rootComponent", ScriptReadOnly)
    ObjectRef<SceneComponent> rootComponent_;
    // Tick 설정이다(A5c — 생성자 값은 클래스 기본값 레코드에 들어가고, 차이 저장이 기본값과 같은
    // 값을 생략하므로 기본값 변경이 인스턴스에 반영된다. 9.1절).
    ROCKS_PROPERTY(Name = "tickSettings") TickSettings tickSettings_;
    ROCKS_PROPERTY(ReadOnly, Name = "enabled", ScriptReadOnly) bool enabled_ = true;
    // 태그다(31.4절 — 바꾸기는 AddTag·RemoveTag·SetTags).
    ROCKS_PROPERTY(ReadOnly, Name = "tags") Array<Name> tags_;
    ROCKS_NATIVE() bool tickEnabled_ = true;
    // 게임 논리 참조다(17.3절). 저장 여부는 Prefab·저장 단계가 정한다(지금은 실행 중 값).
    ROCKS_NATIVE() ObjectHandle owner_;
    ROCKS_NATIVE() ObjectHandle instigator_;
};

} // namespace rocks
