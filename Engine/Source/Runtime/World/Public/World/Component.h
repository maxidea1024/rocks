#pragma once

#include <Object/NativeClass.h>
#include <World/WorldTypes.h>

namespace rocks {

// Actor에 속하는 기능 단위의 기반이다. 소유 Actor가 강한 GC 간선으로 소유하고 World가 수명을
// 진행한다.
//
// 사용 예:
//   class Health final : public Component {
//       ROCKS_NATIVE_CLASS(Health, Component, 0x...ull, 0x...ull)
//   public:
//       int32 value = 100;
//   };
//   RegisterNativeClass<Health>(types);                // World 기본 타입 등록 뒤
//   auto health = world.AddComponent<Health>(actor);   // Actor가 이미 초기화됐으면 즉시 OnAwake
//   if (health) {
//       auto hit = world.With<Health>(health.Value(), [](Health& h) noexcept { h.value -= 10; });
//   }
//
// 수명:
// - hook 순서: OnRegister → OnAwake → OnEnable → OnStart(시작 단계, 활성일 때) → OnTick… →
//   OnDisable → OnDestroy → OnUnregister. OnRegister/OnUnregister는 엔진 내부 등록(렌더·물리
//   backend 연결)용이며 게임플레이 hook보다 먼저/나중에 불린다.
// - 소유 Actor가 Constructed이면 FinishSpawning 때(그 hook 안에서 추가해도) 함께 진행한다. 이미
//   초기화된 Actor에 추가하면 즉시 OnRegister·OnAwake·OnEnable을 부르고 OnStart는 Actor 시작 때
//   또는 다음 프레임 시작에 부른다.
// - 소멸자에서 World·다른 객체·외부 관리자를 호출하지 않는다(OnDestroy/OnUnregister에서 반환).
//
// 스레드: World의 게임 스레드에서만 접근한다.
class Component : public Object {
    ROCKS_NATIVE_CLASS(Component, Object, 0xb4cc30b3df9f47f0ull, 0xbc573c886ac3d468ull)

public:
    Component() noexcept = default;
    virtual ~Component() noexcept = default;

    // 소속 World다. AddComponent가 연결한 뒤에만 유효하다(생성자 안에서는 Fatal).
    World& GetWorld() const noexcept;

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
    // 가린다.
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

    World* world_ = nullptr;
    ObjectHandle owner_;
    TickSettings tickSettings_;
    bool enabled_ = true;
    bool tickEnabled_ = true;
    // World가 관리하는 수명 기록이다.
    ComponentState state_ = ComponentState::Constructed;
    bool registered_ = false;
    bool awake_ = false;
    bool enabledApplied_ = false;
    bool startQueued_ = false;
    TickHandle ticks_[detail::TickHookCount];
};

} // namespace rocks
