#include "SandboxGame.h"

#include <Core/Diagnostics/Log.h>
#include <Gameplay/Controller.generated.h>
#include <Gameplay/GameDirector.generated.h>
#include <Gameplay/GameDirector.h>
#include <Gameplay/Pawn.generated.h>
#include <Gameplay/Pawn.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <World/Actor.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/SceneComponent.h>

namespace sandbox {

namespace {

using namespace rocks;

inline constexpr LogCategory LogSandboxGame{"Sandbox.Game"};

// 로그로 확인하는 키다.
constexpr input::Key WatchedKeys[] = {input::Key::W, input::Key::A, input::Key::S, input::Key::D,
                                      input::Key::Space};
constexpr input::MouseButton WatchedButtons[] = {input::MouseButton::Left,
                                                 input::MouseButton::Right};
constexpr input::GamepadButton WatchedGamepadButtons[] = {input::GamepadButton::South,
                                                          input::GamepadButton::East};

// 로그로 확인하는 게임패드 번호다(첫 패드).
constexpr uint32 WatchedGamepad = 0;

// Controller의 입력 스냅숏에서 이번 프레임 새로 눌린 것을 로그로 남기는 Pawn이다.
class SandboxPawn final : public Pawn {
    ROCKS_NATIVE_TYPE(SandboxPawn, Pawn, 0x6c1f8e3a5d2b4097ull, 0xa3d70e9b1c64f582ull)

public:
    explicit SandboxPawn(Allocator& allocator) noexcept : Pawn(allocator) {
        autoPossessAi = AutoPossessAi::Disabled;
        PrimaryTick().tick = true;
    }

    // PlayerStart Transform이 붙을 root다(Spawn Transform은 root에 쓴다 — 17.3절).
    Result<void> OnAwake() noexcept override {
        auto root = GetWorld().AddComponent<SceneComponent>(Handle());
        if (!root) {
            return Result<void>::Failure(root.Error());
        }

        return SetRootComponent(root.Value());
    }

    void OnTick(const TickContext&) noexcept override {
        if (ControllerHandle().IsNull()) {
            return;
        }

        auto snapshot = GetWorld().With<PlayerController>(
            ControllerHandle(),
            [](PlayerController& controller) noexcept { return controller.PlayerInput(); });
        if (!snapshot) {
            return;
        }

        const input::InputState& state = snapshot.Value();
        for (input::Key key : WatchedKeys) {
            if (state.WasPressed(key)) {
                Log::Info(LogSandboxGame, "Pawn received key {Key}", input::KeyName(key));
            }
        }

        for (input::MouseButton button : WatchedButtons) {
            if (state.WasPressed(button)) {
                Log::Info(LogSandboxGame, "Pawn received mouse button {Button} at ({X}, {Y})",
                          button == input::MouseButton::Left ? "Left" : "Right", state.MouseX(),
                          state.MouseY());
            }
        }

        if (state.MouseWheel() != 0.0f) {
            Log::Info(LogSandboxGame, "Pawn received mouse wheel {Wheel}", state.MouseWheel());
        }

        for (input::GamepadButton button : WatchedGamepadButtons) {
            if (state.WasPressed(WatchedGamepad, button)) {
                Log::Info(LogSandboxGame, "Pawn received gamepad button {Button}",
                          button == input::GamepadButton::South ? "South" : "East");
            }
        }
    }

protected:
    void OnPossessed(ObjectHandle) noexcept override {
        Log::Info(LogSandboxGame, "Pawn possessed by the local player");
    }
};

// 기본 Pawn을 SandboxPawn으로 두는 GameDirector다.
class SandboxDirector final : public GameDirector {
    ROCKS_NATIVE_TYPE(SandboxDirector, GameDirector, 0x19e4b7d2a06c3f85ull, 0x5f0a2c8e7b3d9164ull)

public:
    explicit SandboxDirector(Allocator& allocator) noexcept : GameDirector(allocator) {
        defaultPawnType = ObjectTypeIdOf<SandboxPawn>();
    }
};

// 기본 맵: 원점의 PlayerStart 하나.
Result<void> PopulateMain(World& world, void*) noexcept {
    auto start = world.SpawnActor<PlayerStart>(SpawnParams::At(Transform::Identity()));
    return start ? Result<void>::Success() : Result<void>::Failure(start.Error());
}

} // namespace

SandboxGame::SandboxGame() noexcept {
    config.defaultDirectorType = ObjectTypeIdOf<SandboxDirector>();
    config.defaultMap.name = "Main";
    config.defaultMap.populate = &PopulateMain;
}

Result<void> RegisterSandboxTypes(TypeRegistry& types) noexcept {
    // 등록 토큰은 버린다(Engine은 종료 때 Unregister하지 않는다 — EngineConfig::registerTypes).
    auto pawn = RegisterNativeClass<SandboxPawn>(types);
    if (!pawn) {
        return Result<void>::Failure(pawn.Error());
    }

    auto director = RegisterNativeClass<SandboxDirector>(types);
    return director ? Result<void>::Success() : Result<void>::Failure(director.Error());
}

} // namespace sandbox
