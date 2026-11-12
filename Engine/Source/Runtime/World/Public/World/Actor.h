#pragma once

#include <Object/NativeClass.h>
#include <Object/ObjectRefArray.h>
#include <World/WorldTypes.h>

namespace rocks {

// World에 배치되는 게임플레이 객체의 기반이다. World가 생성하고 Level이 소유하며, Actor는 자기
// Component를 소유한다.
//
// 사용 예:
//   class Spinner final : public Actor {
//       ROCKS_NATIVE_CLASS(Spinner, Actor, 0x...ull, 0x...ull)
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
// 소유: Level이 Actor를, Actor가 자기 Component를 강한 GC 간선 목록(ObjectRefArray)으로 소유한다.
// 파생 클래스가 다른 객체로의 강한 간선(ObjectRef·ObjectRefArray)을 더하면
// `void Trace(TraceVisitor&) const noexcept`를 선언해 자기 필드만 방문한다. 기반 클래스의 Trace는
// RegisterNativeClass가 상속 사슬을 따라 자동으로 호출한다.
//
// 수명(ActorState 참고):
// - 생성자에서는 World에 연결되기 전이다. GetWorld()·Handle() 기반 API를 쓰지 않는다. 기본값과 Tick
//   설정만 채운다. 실패할 수 있는 초기화는 OnAwake에서 한다.
// - hook 순서: OnAwake → OnEnable(활성일 때) → OnStart(시작 단계, 활성일 때) → OnTick… →
//   OnDisable(활성이었을 때) → OnDestroy. OnDestroy는 OnAwake가 성공한 Actor에만 호출한다.
//   Component와 섞인 전체 순서는 World 설명을 참고한다.
// - 소멸자는 저장소 회수(GC) 시점에 불린다. World·다른 객체·에셋 관리자를 호출하지 않는다. 외부
// 자원은
//   OnDestroy에서 반환한다.
// - 객체 포인터는 World가 준 Borrow 범위(hook 호출 중, World::With 콜백 안)에서만 쓴다. 다른 객체는
//   ObjectHandle로 보관하고 필요할 때 World::With로 접근한다.
//
// 스레드: World의 게임 스레드에서만 접근한다.
class Actor : public Object {
    ROCKS_NATIVE_CLASS(Actor, Object, 0x7745f127432e4d2aull, 0xa86d84d477d93eb6ull)

public:
    // allocator는 Registry allocator다(Object 생성 시 주입). 파생 클래스 생성자도 받아서 넘긴다.
    explicit Actor(Allocator& allocator) noexcept : components_(allocator) {
    }

    virtual ~Actor() noexcept = default;

    // 자기 필드의 GC 강한 간선(소유 Component)을 방문한다. 파생 클래스의 Trace는 이것을 부르지
    // 않는다(RegisterNativeClass가 사슬을 따라 호출한다).
    void Trace(TraceVisitor& visitor) const noexcept {
        components_.Trace(visitor);
    }

    // 소유한 Component의 handle 목록이다(추가 순서). 이 Actor의 Borrow 범위에서만 쓰며 Component
    // 추가·제거 후에는 다시 얻는다.
    Span<const ObjectHandle> Components() const noexcept {
        return components_.View();
    }

    // 소속 World다. Spawn이 연결한 뒤에만 유효하다(생성자 안에서는 Fatal).
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
    // 저장소 파괴를 직접 요청하면 World가 OnDestroy·Component 정리를 할 수 없기 때문이다.
    Result<void> Destroy() noexcept;

    // 대표 SceneComponent다. 없으면 null handle이다.
    ObjectHandle RootComponent() const noexcept {
        return rootComponent_;
    }

    // 이 Actor가 소유한 SceneComponent를 대표로 지정한다. 다른 Actor의 Component나 SceneComponent가
    // 아니면 InvalidArgument다. null handle은 대표를 비운다.
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

    World* world_ = nullptr;
    ObjectHandle level_;
    ObjectRefArray<Component> components_;
    ObjectHandle rootComponent_;
    TickSettings tickSettings_;
    bool enabled_ = true;
    bool tickEnabled_ = true;
};

} // namespace rocks
