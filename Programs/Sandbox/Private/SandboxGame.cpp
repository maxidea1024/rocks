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

// 카메라 조작 값이다(Docs/Design/RenderBase 18.5절 4항, UE ADefaultPawn·UFloatingPawnMovement·
// APlayerCameraManager 기본값).
// 최대 속도(cm/s, UE FloatingPawnMovement MaxSpeed)다.
constexpr double PawnMaxSpeed = 1200.0;
// 회전율(도/s, UE DefaultPawn BaseTurnRate·BaseLookUpRate)이다.
constexpr double PawnTurnRate = 45.0;
// 마우스 1 pixel당 회전(도, UE 기본 축 감도 0.07 × 컨트롤러 입력 배율 2.5)이다.
constexpr double PawnMouseDegreesPerPixel = 0.175;
// pitch 한계(도, UE ViewPitchMin·ViewPitchMax)다.
constexpr double PawnPitchLimit = 89.9;

// 키 둘의 축 값(+1·−1)이다.
double KeyAxis(const input::InputState& state, input::Key positive, input::Key negative) noexcept {
    return (state.IsDown(positive) ? 1.0 : 0.0) - (state.IsDown(negative) ? 1.0 : 0.0);
}

// UE DefaultPawn 바인딩으로 이번 프레임 이동 입력(앞·오른쪽·위)과 회전(yaw·pitch, 도)을 모은다.
struct PawnInput {
    double forward = 0.0;
    double right = 0.0;
    double up = 0.0;
    double yaw = 0.0;
    double pitch = 0.0;
};

PawnInput ReadPawnInput(const input::InputState& state, double deltaSeconds) noexcept {
    PawnInput out;
    out.forward = KeyAxis(state, input::Key::W, input::Key::S) +
                  KeyAxis(state, input::Key::Up, input::Key::Down) +
                  state.GamepadAxisValue(WatchedGamepad, input::GamepadAxis::LeftY);
    out.right = KeyAxis(state, input::Key::D, input::Key::A) +
                state.GamepadAxisValue(WatchedGamepad, input::GamepadAxis::LeftX);
    out.up = KeyAxis(state, input::Key::E, input::Key::Q) +
             KeyAxis(state, input::Key::Space, input::Key::C) -
             (state.IsDown(input::Key::LeftControl) ? 1.0 : 0.0) +
             (state.IsDown(WatchedGamepad, input::GamepadButton::South) ? 1.0 : 0.0) +
             (state.IsDown(WatchedGamepad, input::GamepadButton::LeftThumb) ? 1.0 : 0.0) -
             (state.IsDown(WatchedGamepad, input::GamepadButton::RightThumb) ? 1.0 : 0.0);
    const double turn = KeyAxis(state, input::Key::Right, input::Key::Left) +
                        state.GamepadAxisValue(WatchedGamepad, input::GamepadAxis::RightX);
    const double look = state.GamepadAxisValue(WatchedGamepad, input::GamepadAxis::RightY);
    out.yaw = turn * PawnTurnRate * deltaSeconds;
    out.pitch = look * PawnTurnRate * deltaSeconds;
    // 창이 마우스를 가두지 않으므로 오른쪽 버튼을 누른 동안만 돌린다(UE 편집 viewport 방식). 화면
    // y는 아래가 +라 위로 올리면 위를 본다.
    if (state.IsDown(input::MouseButton::Right)) {
        out.yaw += state.MouseDeltaX() * PawnMouseDegreesPerPixel;
        out.pitch -= state.MouseDeltaY() * PawnMouseDegreesPerPixel;
    }

    return out;
}

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

    void OnTick(const TickContext& context) noexcept override {
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
        ApplyMovement(ReadPawnInput(state, context.deltaSeconds), context.deltaSeconds);
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
    // controlRotation을 돌리고(pitch 한계) 그 방향 기준으로 root를 옮긴다(UE DefaultPawn
    // MoveForward· MoveRight는 control 회전 축, MoveUp은 월드 Z). 입력 벡터는 길이 1로 자른다(UE
    // ConsumeInputVector·GetClampedToMaxSize). 가속·감속은 이동 컴포넌트 단계에서 넣는다.
    void ApplyMovement(const PawnInput& input, double deltaSeconds) noexcept {
        Rotator<double> rotation = GetControlRotation();
        if (input.yaw != 0.0 || input.pitch != 0.0) {
            rotation.yaw = Rotator<double>::NormalizeAxis(rotation.yaw + input.yaw);
            rotation.pitch = Rotator<double>::NormalizeAxis(rotation.pitch + input.pitch);
            rotation.pitch = rotation.pitch < -PawnPitchLimit  ? -PawnPitchLimit
                             : rotation.pitch > PawnPitchLimit ? PawnPitchLimit
                                                               : rotation.pitch;
            (void)GetWorld().With<PlayerController>(
                ControllerHandle(), [&rotation](PlayerController& controller) noexcept {
                    controller.SetControlRotation(rotation);
                });
        }

        Vec3d direction = rotation.RotateVector(Vec3d(1.0, 0.0, 0.0)) * input.forward +
                          rotation.RotateVector(Vec3d(0.0, 1.0, 0.0)) * input.right +
                          Vec3d(0.0, 0.0, 1.0) * input.up;
        const double length = direction.Length();
        if (length == 0.0 || RootComponent().IsNull()) {
            return;
        }

        if (length > 1.0) {
            direction = direction * (1.0 / length);
        }

        const Vec3d step = direction * (PawnMaxSpeed * deltaSeconds);
        (void)GetWorld().With<SceneComponent>(
            RootComponent(), [&step](SceneComponent& root) noexcept {
                return root.SetRelativeLocation(root.WorldLocation() + step);
            });
    }

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
