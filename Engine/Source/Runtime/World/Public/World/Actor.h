#pragma once

#include <Meta/Annotations.h>
#include <Object/NativeClass.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <World/Component.h>
#include <World/WorldTypes.h>

namespace rocks {

class SceneComponent;

// World에 배치되는 게임플레이 객체의 기반이다. World가 생성하고 Level이 소유하며, Actor는 자기
// Component를 소유한다.
//
// 사용 예(리플렉션 base(Actor)·SceneComponent를 쓰는 TU는 생성 헤더를 먼저 include한다):
//   #include <World/World.module.generated.h>
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
// ActorRecord·hook·Tick 등록 — 를 우회하지 않게). World 포인터·소속 Level·Tick 설정은
// ROCKS_NATIVE(저장하지 않는 런타임 상태)다. 영속 상태를 더하는 파생은 ROCKS_CLASS(WorldOnly
// 상속)로 Metadata 헤더에 선언하고, 영속 상태가 없는 런타임 전용 파생은 위처럼
// ROCKS_NATIVE_TYPE으로 둔다(그 객체가 든 그래프는 저장할 수 없다).
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
ROCKS_CLASS(WorldOnly)
class Actor : public Object {
    ROCKS_GENERATED_BODY()

public:
    // allocator는 Registry allocator다(Object 생성 시 주입). 파생 클래스 생성자도 받아서 넘긴다.
    explicit Actor(Allocator& allocator) noexcept : components_(allocator) {
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

    // 정리 hook이다. 다른 Actor 파괴·Spawn 요청은 허용한다(같은 프레임 정리 단계에서 처리된다).
    virtual void OnDestroy() noexcept {
    }

protected:
    // 시작 전 Tick 설정을 바꾼다. 생성자나 OnAwake에서 쓴다.
    TickSettings& PrimaryTick() noexcept {
        return tickSettings_;
    }

private:
    friend class World;

    ROCKS_NATIVE() World* world_ = nullptr;
    // actors 포함 관계에서 유도되는 값이다(편입이 다시 채운다).
    ROCKS_NATIVE() ObjectHandle level_;
    ROCKS_PROPERTY(ReadOnly, Required, Name = "components")
    ObjectRefArray<Component> components_;
    // UE AActor::RootComponent처럼 영속 강한 참조다. 늘 components_ 안의 객체라 새 생존 간선을
    // 만들지 않는다. 쓰기는 Registry SetReference로만 한다(Actor·World 구현).
    ROCKS_PROPERTY(ReadOnly, Name = "rootComponent")
    ObjectRef<SceneComponent> rootComponent_;
    // Tick 설정은 클래스 기본값(생성자)이 정한다 — CDO가 없어 인스턴스 저장값이 기본값 변경을
    // 가리므로 저장하지 않는다(9.1절).
    ROCKS_NATIVE() TickSettings tickSettings_;
    ROCKS_PROPERTY(ReadOnly, Name = "enabled") bool enabled_ = true;
    ROCKS_NATIVE() bool tickEnabled_ = true;
};

} // namespace rocks
