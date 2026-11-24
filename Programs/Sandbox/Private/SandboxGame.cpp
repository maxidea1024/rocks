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
constexpr input::Key WatchedKeys[] = {input::Key::W, input::Key::A,     input::Key::S,
                                      input::Key::D, input::Key::Space, input::Key::Back};
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

        // 입력 스냅숏과 제스처를 함께 읽는다(제스처 인식은 빙의 때 켰다).
        struct Frame {
            input::InputState input;
            input::TouchGestureValue gestures[input::TouchGestureCount];
        };
        auto snapshot = GetWorld().With<PlayerController>(
            ControllerHandle(), [](PlayerController& controller) noexcept {
                Frame frame;
                frame.input = controller.PlayerInput();
                for (uint32 gesture = 0; gesture < input::TouchGestureCount; ++gesture) {
                    frame.gestures[gesture] =
                        controller.Gesture(static_cast<input::TouchGesture>(gesture));
                }

                return frame;
            });
        if (!snapshot) {
            return;
        }

        const input::InputState& state = snapshot.Value().input;
        LogGestures(snapshot.Value().gestures);
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

        for (uint32 touch = 0; touch < input::MaxTouches; ++touch) {
            if (state.WasTouchBegan(touch)) {
                Log::Info(
                    LogSandboxGame,
                    "Pawn received touch {Index} at ({X}, {Y}) force {Force} reported {Reported}",
                    touch, state.TouchX(touch), state.TouchY(touch), state.TouchForce(touch),
                    state.TouchHasForce(touch));
            }

            if (state.WasTouchEnded(touch)) {
                Log::Info(LogSandboxGame, "Pawn touch {Index} ended at ({X}, {Y}){Cancelled}",
                          touch, state.TouchX(touch), state.TouchY(touch),
                          state.WasTouchCancelled(touch) ? " (cancelled)" : "");
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
    void OnPossessed(ObjectHandle controller) noexcept override {
        Log::Info(LogSandboxGame, "Pawn possessed by the local player");
        // 터치 제스처를 로그로 확인하려고 인식을 켠다(기본은 꺼짐 —
        // Docs/Design/TouchInput.ko.md 5.1절).
        (void)GetWorld().With<PlayerController>(controller, [](PlayerController& player) noexcept {
            player.SetGestureRecognizerEnabled(true);
        });
    }

private:
    // 제스처 시작·끝을 로그로 남긴다(Pinch·Rotate는 끝날 때 마지막 값, Flick은 방향 각).
    static void
    LogGestures(const input::TouchGestureValue (&gestures)[input::TouchGestureCount]) noexcept {
        static constexpr const char* Names[input::TouchGestureCount] = {"Pinch", "Rotate", "Flick"};
        for (uint32 gesture = 0; gesture < input::TouchGestureCount; ++gesture) {
            if (gestures[gesture].started && !gestures[gesture].ended) {
                Log::Info(LogSandboxGame, "Pawn gesture {Gesture} started", Names[gesture]);
            }

            if (gestures[gesture].ended) {
                Log::Info(LogSandboxGame, "Pawn gesture {Gesture} ended with {Value}",
                          Names[gesture], gestures[gesture].value);
            }
        }
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
