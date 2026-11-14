#pragma once

#include <Meta/Annotations.h>
#include <Object/NativeClass.h>
#include <World/WorldTypes.h>

namespace rocks {

// Actor에 속하는 기능 단위의 기반이다. 소유 Actor가 강한 GC 간선으로 소유하고 World가 수명을
// 진행한다.
//
// 사용 예:
//   class Health final : public Component {
//       ROCKS_NATIVE_TYPE(Health, Component, 0x...ull, 0x...ull)
//   public:
//       int32 value = 100;
//   };
//   RegisterNativeClass<Health>(types);                // World 기본 타입 등록 뒤
//   auto health = world.AddComponent<Health>(actor);   // Actor가 이미 초기화됐으면 즉시 OnAwake
//   if (health) {
//       auto hit = world.With<Health>(health.Value(), [](Health& h) noexcept { h.value -= 10; });
//   }
//
// 리플렉션(Docs/Design/ReflectionFoundations.ko.md 9.1절): WorldOnly 리플렉션 클래스다. 영속
// 필드는 enabled(ReadOnly — 경로·리플렉션 쓰기가 World의 활성 전환 hook을 우회하지 않게, 바꿀 때는
// SetEnabled)뿐이고, World 포인터·소유 Actor·Tick 설정·수명 기록은 ROCKS_NATIVE(저장하지 않는
// 런타임 상태)다. 영속 상태를 더하는 파생은 ROCKS_CLASS(WorldOnly 상속)로 Metadata 헤더에, 영속
// 상태가 없는 런타임 전용 파생은 위처럼 ROCKS_NATIVE_TYPE으로 둔다(그 생성 헤더
// <World/Component.generated.h> 또는 집계 헤더 <World/World.module.generated.h>를
// RegisterNativeClass 전에 include한다).
//
// 수명:
// - hook 순서: OnRegister -> OnAwake -> OnEnable -> OnStart(시작 단계, 활성일 때) -> OnTick… ->
//   OnDisable -> OnDestroy -> OnUnregister. OnRegister/OnUnregister는 엔진 내부 등록(렌더·물리
//   backend 연결)용이며 게임플레이 hook보다 먼저/나중에 불린다.
// - 소유 Actor가 Constructed이면 FinishSpawning 때(그 hook 안에서 추가해도) 함께 진행한다. 이미
//   초기화된 Actor에 추가하면 즉시 OnRegister·OnAwake·OnEnable을 부르고 OnStart는 Actor 시작 때
//   또는 다음 프레임 시작에 부른다.
// - 소멸자에서 World·다른 객체·외부 관리자를 호출하지 않는다(OnDestroy/OnUnregister에서 반환).
// - World 장부 밖 객체(그래프 로드 등 World::AddComponent 밖에서 만든 Component — HasWorld()가
//   거짓)는 World에 편입되기 전까지 World API를 쓰지 못한다(Destroy는 InvalidArgument).
//
// 스레드: World의 게임 스레드에서만 접근한다.
ROCKS_CLASS(WorldOnly)
class Component : public Object {
    ROCKS_GENERATED_BODY()

public:
    Component() noexcept = default;
    virtual ~Component() noexcept = default;

    // 소속 World다. AddComponent가 연결한 뒤에만 유효하다(생성자 안이나 World 장부 밖 객체에서는
    // Fatal — 확인이 필요하면 HasWorld).
    World& GetWorld() const noexcept;

    // AddComponent가 World에 연결했는지다. 생성자 안과 World 장부 밖 객체(그래프 로드 등)는
    // 거짓이다.
    bool HasWorld() const noexcept {
        return world_ != nullptr;
    }

    // 소유 Actor의 handle이다.
    ObjectHandle GetOwner() const noexcept {
        return owner_;
    }

    ComponentState GetState() const noexcept;

    bool IsEnabled() const noexcept {
        return enabled_;
    }

    Result<void> SetEnabled(bool enabled) noexcept;

    // 이 Component를 파괴 대기 상태로 만든다(World::DestroyComponent와 같다). Object::Destroy를
    // 가린다. World 장부 밖 객체는 InvalidArgument다.
    Result<void> Destroy() noexcept;

    const TickSettings& GetTickSettings() const noexcept {
        return tickSettings_;
    }

    Result<void> SetTickEnabled(bool enabled) noexcept;

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

    virtual void OnDestroy() noexcept {
    }

protected:
    // 엔진 내부 등록 hook이다. 실패하면 Component를 붙이지 않는다(OnUnregister 없이 정리).
    virtual Result<void> OnRegister() noexcept {
        return Result<void>::Success();
    }

    // OnRegister가 성공한 Component의 정리 hook이다. 다른 객체 접근은 World::With로 한다.
    virtual void OnUnregister() noexcept {
    }

    TickSettings& PrimaryTick() noexcept {
        return tickSettings_;
    }

private:
    friend class World;

    ROCKS_NATIVE() World* world_ = nullptr;
    ROCKS_NATIVE() ObjectHandle owner_;
    // Tick 설정은 클래스 기본값(생성자)이 정한다 — CDO가 없어 인스턴스 저장값이 기본값 변경을
    // 가리므로 저장하지 않는다(9.1절).
    ROCKS_NATIVE() TickSettings tickSettings_;
    ROCKS_PROPERTY(ReadOnly, Name = "enabled") bool enabled_ = true;
    ROCKS_NATIVE() bool tickEnabled_ = true;
    // World가 관리하는 수명 기록이다.
    ROCKS_NATIVE() ComponentState state_ = ComponentState::Constructed;
    ROCKS_NATIVE() bool registered_ = false;
    ROCKS_NATIVE() bool awake_ = false;
    ROCKS_NATIVE() bool enabledApplied_ = false;
    ROCKS_NATIVE() bool startQueued_ = false;
    ROCKS_NATIVE() TickHandle ticks_[detail::TickHookCount];
};

} // namespace rocks
