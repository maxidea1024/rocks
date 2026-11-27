#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/Name.h>
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
//       using Component::Component;   // allocator 생성자(Docs/Design/EngineRuntime 31.3절)
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
// SetEnabled)와 tickSettings(Tick 설정 — 생성자 값은 클래스 기본값, 시작 때 한 번 등록하므로 시작
// 뒤 변경은 반영하지 않는다)이고, World 포인터·소유 Actor·Tick 등록 handle·수명 기록은
// ROCKS_NATIVE(저장하지 않는 런타임 상태)다. 영속 상태를 더하는 파생은 ROCKS_CLASS(WorldOnly
// 상속)로 리플렉션 헤더에, 영속 상태가 없는 런타임 전용 파생은 위처럼 ROCKS_NATIVE_TYPE으로 둔다(그
// 생성 헤더 <World/Component.generated.h>를 RegisterNativeClass 전에 include한다).
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
ROCKS_CLASS(WorldOnly, ScriptType)
class ROCKS_WORLD_API Component : public Object {
    ROCKS_GENERATED_BODY()

public:
    // allocator는 태그 목록용이다(파생은 받은 allocator를 넘긴다 — 31.3절).
    explicit Component(Allocator& allocator) noexcept : componentTags_(allocator) {
    }

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

    // World에 등록되어 있는지다(OnRegister 성공 뒤 ~ 해제 전).
    bool IsRegistered() const noexcept {
        return registered_;
    }

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

    // ---- 태그(Docs/Design/EngineRuntime 31.4절 — UE UActorComponent::ComponentTags) ----

    // tag가 들어 있는지다. None은 늘 거짓이다(UE ComponentHasTag).
    bool HasTag(Name tag) const noexcept;

    // 태그 목록이다(순서·중복 그대로). 바꾸면 무효다.
    Span<const Name> ComponentTags() const noexcept {
        return componentTags_.View();
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

    // 게임플레이 시작에 필요한 의존이 준비됐는지다(Actor::IsReadyToStart와 같은 뜻 — 17.4절 ERD18,
    // CR4·SE-W2). 소유 Actor는 자신과 켜진 모든 Component가 준비된 프레임에 시작한다. 이미
    // Playing인 Actor에 나중에 추가한 Component는 자기 준비만 기다린다. 기본은 참이다.
    virtual bool IsReadyToStart() const noexcept {
        return true;
    }

    // reason은 정리 이유다(17.2절 — 소유 Actor와 함께 정리되면 Actor의 이유).
    virtual void OnDestroy(EndPlayReason) noexcept {
    }

    // --- 시스템 상태(Docs/Design/EngineRuntime 23.2절 CR1) ---

    // 그 종류의 시스템 상태가 지금 있는지다(등록 직후 만들고 해제 직전 지운다).
    bool HasSystemState(SystemStateKind kind) const noexcept {
        const uint32 index = static_cast<uint32>(kind);
        return index < SystemStateKindCount && (systemStates_ & (1u << index)) != 0;
    }

    // 그 종류의 시스템 상태를 다음 갱신 지점(World Tick의 PreRender 단계 끝, 파괴 정리 전)에
    // 지우고 다시 만들게 표시한다(UE MarkRenderStateDirty). 등록 전·파괴 대기·그 종류가 없으면
    // 아무것도 하지 않고 성공한다. 같은 프레임의 중복 표시는 한 번이다. 오류: 범위 밖 종류
    // InvalidArgument, 할당 실패.
    Result<void> MarkSystemStateDirty(SystemStateKind kind) noexcept;

    // 그 종류의 시스템 상태를 그 자리에서 다시 만든다(UE RecreatePhysicsState·
    // EnsurePhysicsStateCreated — 23.2절). 있으면 지우고, WantsSystemState가 참이면 만든다(없던
    // 것도 만든다). 그 종류의 대기 표시는 지운다. 같은 프레임의 다음 질의가 바뀐 상태를 봐야 하는
    // setter가 부른다.
    //
    // 사용 예:
    //   collisionEnabled_ = enabled;
    //   (void)RecreateSystemStateNow(SystemStateKind::Physics);   // 실패해도 값은 바뀌었다
    //
    // 등록 전·해제 뒤·파괴 대기면 아무것도 하지 않고 성공한다. 오류: 범위 밖 종류 InvalidArgument,
    // 같은 종류의 Create·Destroy 안에서 다시 부르면 Busy, Create 실패는 그 오류(그 종류 없이 두고
    // Warning SystemStateRecreateFailed).
    Result<void> RecreateSystemStateNow(SystemStateKind kind) noexcept;

    // --- 타입 규칙·EditorOnly(23.2절 CR5·CR6) ---

    // 이 타입의 규칙이다(인스턴스 virtual — World가 만든 직후 등록 전에 묻는다). 기본은 규칙 없음.
    virtual ComponentTypeRules TypeRules() const noexcept {
        return {};
    }

    // 편집 전용 Component인지다(UE bIsEditorOnly). 참이면 Game·Tryout World의 AddComponent가
    // Unsupported로 거부한다(편집·미리보기 World는 만든다).
    virtual bool IsEditorOnly() const noexcept {
        return false;
    }

protected:
    // 엔진 내부 등록 hook이다. 실패하면 Component를 붙이지 않는다(OnUnregister 없이 정리).
    virtual Result<void> OnRegister() noexcept {
        return Result<void>::Success();
    }

    // OnRegister가 성공한 Component의 정리 hook이다. 다른 객체 접근은 World::With로 한다.
    virtual void OnUnregister() noexcept {
    }

    // 시스템 상태 hook이다(23.2절). WantsSystemState가 참인 종류만 등록 직후(OnRegister 뒤)
    // Render → Physics → Audio 순으로 CreateSystemState를 부른다. 하나라도 실패하면 만든 것을
    // 역순으로 지우고 OnUnregister를 부른 뒤 등록이 실패한다. 해제(OnUnregister 전)에는 있는 것을
    // 역순으로 DestroySystemState한다. 꺼짐(OnDisable)은 지우지 않는다.
    virtual bool WantsSystemState(SystemStateKind) const noexcept {
        return false;
    }

    virtual Result<void> CreateSystemState(SystemStateKind) noexcept {
        return Result<void>::Success();
    }

    virtual void DestroySystemState(SystemStateKind) noexcept {
    }

    // 시스템 상태 kind를 만들었거나(created) 지웠다는 통지다. World가 상태 bit·등록 표지를 확정한
    // 뒤 부르므로(등록은 모든 종류를 만든 뒤, 즉시·갱신 지점 다시 만들기는 진행 표지를 푼 뒤
    // Destroyed → Created 순) 여기서 다시 만들기·값 변경을 해도 된다(UE OnComponentPhysicsState-
    // Changed가 bPhysicsStateCreated 확정 뒤 나는 것과 같다 — PhysicsModule 7.1.19절). 기본은
    // 아무것도 하지 않는다.
    virtual void OnSystemStateChanged(SystemStateKind, bool) noexcept {
    }

    TickSettings& PrimaryTick() noexcept {
        return tickSettings_;
    }

private:
    friend class World;

    ROCKS_NATIVE() World* world_ = nullptr;
    ROCKS_NATIVE() ObjectHandle owner_;
    // Tick 설정이다(A5c — 생성자 값은 클래스 기본값 레코드에 들어가고, 차이 저장이 기본값과 같은
    // 값을 생략하므로 기본값 변경이 인스턴스에 반영된다. 9.1절).
    ROCKS_PROPERTY(Name = "tickSettings") TickSettings tickSettings_;
    ROCKS_PROPERTY(ReadOnly, Name = "enabled", ScriptReadOnly) bool enabled_ = true;
    // 태그다(31.4절 — 바꾸기는 AddTag·RemoveTag·SetTags).
    ROCKS_PROPERTY(ReadOnly, Name = "componentTags") Array<Name> componentTags_;
    ROCKS_NATIVE() bool tickEnabled_ = true;
    // World가 관리하는 수명 기록이다.
    ROCKS_NATIVE() ComponentState state_ = ComponentState::Constructed;
    ROCKS_NATIVE() bool registered_ = false;
    ROCKS_NATIVE() bool awake_ = false;
    ROCKS_NATIVE() bool enabledApplied_ = false;
    ROCKS_NATIVE() bool startQueued_ = false;
    // 있는 시스템 상태와 다시 만들 표시다(종류마다 비트).
    ROCKS_NATIVE() uint8 systemStates_ = 0;
    ROCKS_NATIVE() uint8 dirtySystemStates_ = 0;
    // 지금 다시 만드는 중인 종류다(즉시·갱신 지점 모두 — 같은 종류 재진입은 Busy).
    ROCKS_NATIVE() uint8 recreatingSystemStates_ = 0;
    // 갱신 지점이 이 Component에서 아직 처리하지 않은 종류다(즉시 다시 만들기가 지운다).
    ROCKS_NATIVE() uint8 flushingSystemStates_ = 0;
    // World의 갱신 목록에 들어 있다(표시 비트와 따로 — 즉시 다시 만들기가 비트를 지워도 한 번만
    // 들어간다).
    ROCKS_NATIVE() bool systemStateQueued_ = false;
    // 단독 파괴(DestroyComponent)의 정리 이유다(먼저 정한 것을 유지한다).
    ROCKS_NATIVE() EndPlayReason endReason_ = EndPlayReason::Destroyed;
    ROCKS_NATIVE() TickHandle ticks_[detail::TickHookCount];
};

} // namespace rocks
