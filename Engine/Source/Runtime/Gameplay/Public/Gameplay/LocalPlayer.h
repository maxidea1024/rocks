#pragma once

#include <Gameplay/GameplayTypes.h>
#include <Meta/Annotations.h>
#include <Object/NativeClass.h>
#include <World/Subsystem.h>

namespace rocks {

class GameInstance;
class LocalPlayer;

namespace input {
class InputState;
} // namespace input

// LocalPlayer와 수명을 같이하는 Subsystem의 기반이다(UE ULocalPlayerSubsystem, Docs/Design/
// EngineRuntime 16.3절 ERD11). LocalPlayer를 추가할 때 초기화하고(UE PlayerAdded) 제거할 때
// 역순으로 정리한다. Initialize·Deinitialize가 받는 LocalPlayer는 그 호출 동안만 빌린
// 것이다(포인터를 남기지 않는다 — 필요하면 handle을 남긴다).
//
// 사용 예:
//   class PlayerSettings final : public LocalPlayerSubsystem {
//   public:
//       Result<void> Initialize(LocalPlayer& player) noexcept override {
//           controllerId = player.ControllerId();
//           return Result<void>::Success();
//       }
//       int32 controllerId = 0;
//   };
//   const LocalPlayerSubsystemType types[] = {
//       MakeSubsystemType<LocalPlayerSubsystem, PlayerSettings>("PlayerSettings")};
//   // GameInstanceConfig::localPlayerSubsystems = types;
class LocalPlayerSubsystem {
public:
    virtual ~LocalPlayerSubsystem() = default;

    virtual Result<void> Initialize(LocalPlayer&) noexcept {
        return Result<void>::Success();
    }

    virtual void Deinitialize(LocalPlayer&) noexcept {
    }
};

using LocalPlayerSubsystemType = SubsystemType<LocalPlayerSubsystem>;
using LocalPlayerSubsystems = SubsystemCollection<LocalPlayerSubsystem, LocalPlayer>;

// 이 기기에서 게임을 하는 사람 한 명이다(UE ULocalPlayer, 18.4절). GameInstance가 만들고 소유하며
// World를 넘어 산다. 로컬 번호(Index — GameInstance 목록 위치), 입력 장치 번호(ControllerId), 입력
// 원천(호스트가 넘기는 프레임 입력 상태의 읽기 창), LocalPlayer Subsystem 묶음을 가진다. 화면 분할·
// viewport는 렌더 단계가 붙인다.
//
// 엔진 Registry 객체라 Borrow 범위에서만 쓴다(GameInstance::WithLocalPlayer, Subsystem hook).
// 게임은 파생 클래스를 GameInstanceConfig::localPlayerType으로 지정할 수 있다.
//
// 사용 예:
//   auto id = gameInstance.WithLocalPlayer(player, [](LocalPlayer& local) noexcept {
//       return local.ControllerId();
//   });
//
// 스레드: 엔진 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_GAMEPLAY_API LocalPlayer : public Object {
    ROCKS_GENERATED_BODY()

public:
    // allocator는 Registry allocator다(Object 생성 시 주입). 파생 클래스 생성자도 받아서 넘긴다.
    explicit LocalPlayer(Allocator& allocator) noexcept;
    virtual ~LocalPlayer() noexcept;

    // GameInstance 목록의 위치다(0부터). 앞의 LocalPlayer가 제거되면 앞당겨진다(UE LocalPlayers
    // index).
    uint32 Index() const noexcept {
        return index_;
    }

    // 입력 장치 번호다(UE ControllerId·PlatformUserId). GameInstance 안에서 겹치지 않는다.
    int32 ControllerId() const noexcept {
        return controllerId_;
    }

    // 호스트가 넘긴 프레임 입력 상태다(없으면 null — 헤드리스·서버). 포인터는 호스트가 바꾸거나
    // 지울 때까지 유효하며 게임 스레드에서 읽는다. 바꾸는 것은
    // GameInstance::SetLocalPlayerInput이다 (현재 PlayerController의 입력 원천도 함께 바꾼다).
    const input::InputState* Input() const noexcept {
        return input_;
    }

    // 현재 World의 PlayerController다(없으면 null — Login 전, Client·편집 World). 게임 코드가 그
    // PlayerController를 직접 파괴하면 낡은 handle일 수 있다(World::HasActor로 확인한다).
    ObjectHandle PlayerControllerHandle() const noexcept {
        return playerController_;
    }

    // 소유 GameInstance의 handle이다(엔진 Registry).
    ObjectHandle OwnerHandle() const noexcept {
        return owner_;
    }

    // T 종류의 LocalPlayer Subsystem이다. 없으면 nullptr이고 포인터는 이 LocalPlayer를 제거할
    // 때까지 유효하다.
    template <class T>
    T* GetSubsystem() const noexcept {
        return subsystems_.template Get<T>();
    }

    // 이 LocalPlayer를 만들 때 생략한 Subsystem이다(진단용).
    Span<const SkippedSubsystem> SkippedSubsystems() const noexcept {
        return subsystems_.Skipped();
    }

private:
    friend class GameInstance;
    friend class GameInstanceClient;

    ROCKS_NATIVE() ObjectHandle owner_;
    ROCKS_NATIVE() ObjectHandle playerController_;
    ROCKS_NATIVE() const input::InputState* input_ = nullptr;
    ROCKS_NATIVE() LocalPlayerSubsystems subsystems_;
    ROCKS_NATIVE() int32 controllerId_ = 0;
    ROCKS_NATIVE() uint32 index_ = 0;
};

// Gameplay 리플렉션 타입(GameInstance·LocalPlayer)을 등록한다(생성 집계 함수
// generated::Gameplay::RegisterModuleTypes — 의존 모듈 타입도 함께). 엔진은 모듈 로더가 등록하므로
// 도구·시험만 직접 부른다. 오류: 등록 오류(할당 실패 등).
ROCKS_GAMEPLAY_API Result<void> RegisterGameplayTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
