#pragma once

#include <AssetRuntime/AssetManager.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Vector3.h>
#include <Gameplay/Controller.h>
#include <World/Camera/CameraComponent.h>
#include <Input/InputState.h>
#include <Input/TouchGestureRecognizer.h>
#include <Input/MotorValues.h>
#include <InputActions/Haptics/HapticPlayer.h>
#include <InputActions/InputActionBindings.h>
#include <InputActions/PlayerActionInput.h>

namespace rocks {

// 카메라 결과다(UE FMinimalViewInfo의 최소 부분, Docs/Design/EngineRuntime 20.6절). 위치는 cm,
// 회전은 도, fov는 수평 도(UE DefaultFOV 90).
struct CameraView {
    static constexpr double DefaultFov = 90.0;

    Vec3d location;
    Rotator<double> rotation;
    double fov = DefaultFov;
    // view target의 CameraComponent가 정한 투영·직교 폭·종횡비다(없으면 기본 — 23.4절).
    CameraProjection projection = CameraProjection::Perspective;
    double orthoWidth = CameraComponent::DefaultOrthoWidth;
    double aspectRatio = CameraComponent::DefaultAspectRatio;
    bool constrainAspectRatio = false;
};

// 플레이어 카메라다(UE APlayerCameraManager 최소 범위, 20.6절). PlayerController가 만들고(Owner =
// PlayerController) 함께 파괴한다. LateUpdate 시작 엔진 단계 CameraUpdate에서 UpdateCamera가 view
// target으로 결과를 만든다(pause 중에는 엔진 단계가 돌지 않는다). blend·modifier·shake·카메라
// 컴포넌트는 CM0·CR29가 더한다.
//
// 사용 예(파생 — 결과 고치기):
//   void OnUpdateView(CameraView& view) noexcept override { view.fov = 75.0; }
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(WorldOnly)
class ROCKS_GAMEPLAY_API CameraDirector : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit CameraDirector(Allocator& allocator) noexcept;
    ~CameraDirector() noexcept override;

    // 보는 대상이다(기본은 소유 PlayerController, Possess 뒤 Pawn).
    ObjectHandle ViewTarget() const noexcept {
        return viewTarget_;
    }

    void SetViewTarget(ObjectHandle target) noexcept {
        viewTarget_ = target;
    }

    // 마지막 UpdateCamera의 결과다.
    const CameraView& View() const noexcept {
        return view_;
    }

    // view target으로 결과를 만든다. 대상에 켜진 CameraComponent가 있으면 Component 순서로 첫 것의
    // GetCameraView다(UE CalcCamera — usePawnControlRotation이고 대상이 Pawn이면 회전은
    // GetControlRotation). 없으면 위치는 root SceneComponent의 월드 위치(root가 없으면 직전 값),
    // 회전은 대상이 Controller면 그 controlRotation, Pawn이면 Pawn의 GetControlRotation, 그 밖은
    // root 회전이다 → OnUpdateView. 대상이 없거나 파괴됐으면 소유자로 되돌린다.
    void UpdateCamera(double deltaSeconds) noexcept;

protected:
    // 사용자 hook: 결과를 고친다.
    virtual void OnUpdateView(CameraView&) noexcept {
    }

private:
    ROCKS_NATIVE() CameraView view_;
    ROCKS_NATIVE() ObjectHandle viewTarget_;
};

// 로컬 플레이어의 Controller다(UE APlayerController, 20.5절). GameDirector Login이 만들고
// LocalPlayer와 양방향으로 잇는다(SetPlayer). PlayerState·CameraDirector를 만들고 Spectating으로
// 시작하며 Possess하면 Playing이 된다. OnTick은 엔진이 닫고 입력 스냅숏 → 제스처 → 입력 액션
// 평가·전달(ActionInput·ActionBindings — Docs/Design/InputActions 5.1절) → OnPlayerTick(pause 중
// 생략) → OnControllerTick 순으로 부른다(UE PlayerTick·Tick). pause 중에도 Tick하고 입력 액션도
// 전달한다.
//
// 사용 예(파생):
//   void OnPlayerTick(const TickContext&) noexcept override {
//       if (PlayerInput().WasPressed(input::Key::Space)) { ... }
//   }
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(WorldOnly)
class ROCKS_GAMEPLAY_API PlayerController : public Controller {
    ROCKS_GENERATED_BODY()

public:
    explicit PlayerController(Allocator& allocator) noexcept;
    ~PlayerController() noexcept override;

    // 연결된 LocalPlayer다(엔진 Registry handle, 없으면 null — 접속 끊김).
    ObjectHandle LocalPlayerHandle() const noexcept {
        return localPlayer_;
    }

    // 이번 프레임 입력 스냅숏이다(TickPlayerInput이 입력 원천에서 복사한다 — 원천이 없으면 빈
    // 상태).
    const input::InputState& PlayerInput() const noexcept {
        return playerInput_;
    }

    ObjectHandle CameraDirectorHandle() const noexcept {
        return cameraDirector_;
    }

    // LocalPlayer와 잇는다(UE SetPlayer — GameInstance Login이 부른다). source는 그 LocalPlayer의
    // 입력 원천이다(null이면 입력 없음). gamepad는 그 LocalPlayer의 패드 번호(ControllerId)다.
    // userSettings는 그 LocalPlayer의 키 설정이다(입력 액션 평가기에 붙는다 — null이면 없음,
    // ClearPlayer 전까지 살아 있어야 한다). 스냅숏을 비운다.
    void SetPlayer(ObjectHandle localPlayer, const input::InputState* source, uint32 gamepad,
                   input::InputUserSettings* userSettings = nullptr) noexcept;

    // 이 플레이어의 입력 액션 평가기다(컨텍스트 추가·주입 — Docs/Design/InputActions 4절). OnTick이
    // 평가한다.
    //
    // 사용 예:
    //   (void)controller.ActionInput().AddContext(Move(bundle), 0);
    input::PlayerActionInput& ActionInput() noexcept {
        return actionInput_;
    }

    const input::PlayerActionInput& ActionInput() const noexcept {
        return actionInput_;
    }

    // ---- 에셋 컨텍스트(Docs/Design/InputActions 5.4절) ----

    // lease의 InputContext 에셋을 모아 평가기에 넣고, 성공하면 언제나 lease를 가져간다(UE
    // AddMappingContext — 호출자의 lease는 빈다). 같은 에셋을 다시 넣으면 AddContext의 같은 id
    // 규칙(우선순위 바꿈·등록 횟수)을 따르고, 쥔 lease는 받은 새 lease로 바꾼다(옛 lease를 놓는다).
    // 실패하면 상태와 lease가 그대로다. 에셋이 쥔 컨텍스트 id에 ActionInput()을 직접 쓰는 것
    // (UpdateContext, 같은 id AddContext)은 계약 위반이다(5.4절).
    //
    // 사용 예:
    //   auto lease = assets.LoadBlocking(request, "Game.DefaultInput");
    //   if (lease) {
    //       (void)controller.AddInputContext(assets, Move(lease.Value()), 0);
    //   }
    //
    // 오류: 끝난 PlayerController(OnFrameworkEndPlay 뒤)·다른 원천(다른 에셋이나 ActionInput에 직접
    // 넣은 bundle)이 이미 넣은 컨텍스트 id·같은 에셋인데 쥘 때와 다른 id·검증 실패는
    // InvalidArgument, 회수·무효 lease는 StaleHandle, 대표 에셋이 InputContext가 아니면
    // TypeMismatch, 할당 실패는 OutOfMemory다. 게임 스레드(AssetManager 소유 스레드) 전용이다.
    Result<void> AddInputContext(AssetManager& assets, AssetLease&& context, int32 priority,
                                 const input::ContextOptions& options = {}) noexcept;

    // 쥔 에셋 컨텍스트를 뺀다(UE RemoveMappingContext). 평가기에서 빠지면(등록 횟수를 세는
    // 컨텍스트면 횟수가 0이 될 때) lease를 놓는다. 쥐지 않은 에셋(끝난 칸 포함)은 NotFound다. 게임
    // 스레드 전용이다.
    //
    // 사용 예:
    //   (void)controller.RemoveInputContext(menuContextAsset);
    Result<void> RemoveInputContext(AssetId asset,
                                    const input::ContextOptions& options = {}) noexcept;

    // 쥔 lease로 다시 모아 UpdateContext한다(에셋 데이터를 바꾼 뒤 — 우선순위·등록 횟수 그대로).
    // 오류: 쥐지 않은 에셋 NotFound, 모은 id가 쥘 때와 다르면 InvalidArgument, 회수된 lease
    // StaleHandle, 그 밖은 AddInputContext의 모으기·검증 오류다. 실패하면 평가기 사본이 그대로다.
    // 게임 스레드 전용이다. 모든 플레이어에 퍼뜨리려면 GameplayWorldSubsystem::RefreshInputContext.
    //
    // 사용 예:
    //   (void)controller.RefreshInputContext(assets, gameplayContextAsset, true);
    Result<void> RefreshInputContext(AssetManager& assets, AssetId asset,
                                     bool forceImmediately = false) noexcept;

    // 에셋 컨텍스트를 쥐고 있고 그 칸이 끝나지 않았으면 참이다. ActionInput에서 직접 뺐거나 뺀 뒤
    // 같은 id를 다시 넣은 칸(평가기 칸 세대가 다르다)은 거짓이다(lease는 다음 Add·Remove·Refresh나
    // OnTick 평가 전에 놓는다). 게임 스레드 전용이다.
    bool HasInputContext(AssetId asset) const noexcept;

    // ---- 입력 스택(Docs/Design/InputActions 5.5절) ----

    // component(InputComponent)를 입력 스택에 넣는다(UE PushInputComponent). 이미 있으면 빼고 다시
    // 넣는다. 뒤에서부터 priority가 같거나 작은 첫 항목 바로 뒤 자리다. 스택은 매 틱 priority로
    // 다시 정렬해 전달한다.
    //
    // 오류: InputComponent가 아니면 TypeMismatch, 없는 handle StaleHandle, 파괴 대기 Destroyed,
    // 할당 실패 OutOfMemory(스택 그대로). 게임 스레드 전용이다.
    Result<void> PushInputComponent(ObjectHandle component) noexcept;

    // component를 스택에서 빼고 그 값 바인딩을 0으로 한다(UE PopInputComponent — 값 0은 Rocks가
    // 더한 것, 5.5절). 빠진 층은 같은 틱 남은 전달도 받지 않는다. 없었으면 거짓이다.
    bool PopInputComponent(ObjectHandle component) noexcept;

    bool IsInputComponentInStack(ObjectHandle component) const noexcept;

    // actor의 입력을 켠다(UE AActor::EnableInput — World는 Gameplay 아래라 여기 둔다). actor의 첫
    // 살아 있는 InputComponent를 쓰고 없으면 만들어 Push하고 그 handle을 돌려준다. 이
    // PlayerController가 조종하는 Pawn이면 Push하지 않고 그 Pawn의 입력을 켠다(UE
    // APawn::EnableInput — 돌려주는 handle은 Pawn의 플레이어 입력 컴포넌트, 없으면 null). actor가
    // 이 PlayerController 자신이면 PlayerController 층을 켠다(UE APlayerController::EnableInput —
    // null handle).
    //
    // 사용 예:
    //   auto component = controller.EnableInput(door);
    //
    // 오류: 이 World의 Actor가 아니면 StaleHandle, 파괴 대기 Destroyed, 다른 Controller가
    // 조종하거나 Controller가 없는 Pawn은 InvalidArgument, 컴포넌트 추가·Push 오류는 그대로다.
    Result<ObjectHandle> EnableInput(ObjectHandle actor) noexcept;

    // actor의 입력을 끈다(UE AActor::DisableInput). 조종하는 Pawn이면 그 Pawn의 입력을 끄고, 이
    // PlayerController 자신이면 PlayerController 층을 끈다. 아니면 그 Actor 소유의 InputComponent를
    // 모두 이 스택에서 뺀다. 오류는 EnableInput과 같다(컴포넌트 오류 없음).
    Result<void> DisableInput(ObjectHandle actor) noexcept;

    // PlayerController 층(ActionBindings)이 켜져 있는지다(UE bInputEnabled — 기본 참).
    bool IsInputEnabled() const noexcept {
        return inputEnabled_;
    }

    // 입력 스택에서 PlayerController 층의 priority다(UE PC_InputComponent0의 기본 Priority).
    static constexpr int32 ControllerLayerPriority = 0;

    // 이 플레이어의 액션 바인딩이다(UE UEnhancedInputComponent — 5.1절). OnTick이 평가 뒤 전달한다.
    // 입력 스택의 PlayerController 층이다(ControllerLayerPriority, 막지 않음 — 5.5절).
    input::InputActionBindings& ActionBindings() noexcept {
        return actionBindings_;
    }

    // 입력 액션 평가에 쓰는 패드 번호다.
    uint32 InputGamepad() const noexcept {
        return gamepad_;
    }

    // 이 플레이어의 게임패드 진동이다(UE ClientPlayForceFeedback·PlayDynamicForceFeedback —
    // Docs/Design/InputActions 6.2절). OnTick이 입력 액션 전달 뒤 합성한다.
    //
    // 사용 예:
    //   input::HapticParams params;
    //   params.tag = "Hit";
    //   (void)controller.Haptics().PlayHapticEffect(hitEffect, params);
    input::HapticPlayer& Haptics() noexcept {
        return haptics_;
    }

    // 이번 틱에 합성한 모터 값이다(6.3절 — LocalPlayer가 없으면 0). 호스트가 프레임 끝에 이
    // 플레이어의 패드 번호(InputGamepad)로 GamepadBackend::SetMotors에 넘긴다.
    const input::MotorValues& MotorOutput() const noexcept {
        return motorOutput_;
    }

    // 진동 설정 input.haptic_scale·input.enable_haptics를 넣는다(GameInstance가 프레임마다
    // 부른다 — 설정 문맥이 없으면 기본 1·켬).
    void ApplyHapticSettings(float scale, bool enabled) noexcept {
        hapticScale_ = scale;
        hapticsEnabled_ = enabled;
    }

    // 입력 원천만 바꾼다(GameInstance::SetLocalPlayerInput).
    void SetInputSource(const input::InputState* source) noexcept {
        inputSource_ = source;
        gestures_.Reset();
    }

    // 터치 제스처 인식(Pinch·Rotate·Flick — Docs/Design/TouchInput.ko.md 5.1절)을 켜고 끈다. 명시로
    // 정하면 설정 input.enable_gesture_recognizer(기본 꺼짐 — UE bEnableGestureRecognizer)보다
    // 우선한다. 켜면 TickPlayerInput 단계에서 입력 스냅숏 다음에 돈다. 끄면 진행 중인 제스처를
    // 지운다.
    //
    // 사용 예:
    //   controller.SetGestureRecognizerEnabled(true);
    //   if (controller.Gesture(input::TouchGesture::Pinch).active) { ... }
    void SetGestureRecognizerEnabled(bool enabled) noexcept {
        gestureRecognizerExplicit_ = true;
        SetGestureRecognition(enabled);
    }

    // 명시 설정을 지우고 설정 값을 따르게 한다.
    void UseGestureRecognizerSetting() noexcept {
        gestureRecognizerExplicit_ = false;
    }

    // 설정 값을 넣는다(GameInstance가 프레임마다 부른다). 명시로 정했으면 무시한다.
    void ApplyGestureRecognizerSetting(bool enabled) noexcept {
        if (!gestureRecognizerExplicit_) {
            SetGestureRecognition(enabled);
        }
    }

    bool IsGestureRecognizerEnabled() const noexcept {
        return gestureRecognizerEnabled_;
    }

    // 이번 프레임 제스처 상태다(인식이 꺼져 있으면 늘 비어 있다).
    const input::TouchGestureValue& Gesture(input::TouchGesture gesture) const noexcept {
        return gestures_.Gesture(gesture);
    }

    // LocalPlayer 연결을 끊는다(LocalPlayer 제거). 끊긴 PlayerController는 파괴될 때 Pawn도
    // 지운다(UE PawnLeavingGame).
    void ClearPlayer() noexcept {
        localPlayer_ = {};
        inputSource_ = nullptr;
        gamepad_ = 0;
        actionInput_.SetUserSettings(nullptr);
        // 플레이어가 없으면 진동을 내지 않는다(효과는 남는다 — UE Player == nullptr).
        motorOutput_ = {};
    }

protected:
    // 사용자 hook: 입력 해석(UE PlayerTick). pause 중에는 불리지 않는다.
    virtual void OnPlayerTick(const TickContext&) noexcept {
    }

    // 사용자 hook: Tick 본문(UE Tick). pause 중에도 불린다(ctx.paused로 pause 해제 입력 처리).
    virtual void OnControllerTick(const TickContext&) noexcept {
    }

    Result<void> OnPossess(ObjectHandle pawn) noexcept override;
    void OnUnPossess() noexcept override;

private:
    // 쥔 에셋 컨텍스트 하나다(5.4절 — UE AppliedInputContextData가 컨텍스트를 쥐는 역할).
    struct AppliedInputAsset {
        AssetId asset;
        Guid context;
        // 넣을 때 평가기 칸의 세대다(PlayerActionInput::ContextSerial — 다르면 끝난 칸).
        uint64 serial = 0;
        AssetLease lease;
    };

    // asset을 쥔 칸의 index다(없으면 inputAssets_.Size()).
    usize FindInputAsset(AssetId asset) const noexcept;
    // 그 칸이 아직 평가기의 같은 칸인지다(세대 비교).
    bool IsInputAssetLive(const AppliedInputAsset& entry) const noexcept;
    // 끝난 칸(평가기에서 빠졌거나 다른 칸이 된 컨텍스트)의 lease를 놓는다.
    void ReleaseRemovedInputAssets() noexcept;
    // 쥔 컨텍스트를 모두 평가기에서 빼고 lease를 놓는다(OnFrameworkEndPlay).
    void ReleaseInputAssets() noexcept;

    // 입력 스택 층이 어디서 왔는지다(같은 틱 재검사 기준 — 5.5절).
    enum class InputLayerSource : uint8 {
        // 조종하는 Pawn의 InputComponent.
        Pawn,
        // PlayerController 층(ActionBindings — component는 null).
        Controller,
        // PushInputComponent로 넣은 것.
        Stack
    };

    // 입력 스택 층 하나다.
    struct InputLayer {
        ObjectHandle component;
        int32 priority = 0;
        InputLayerSource source = InputLayerSource::Controller;
    };

    // 이번 틱의 스택을 쌓아 위층부터 전달한다(5.5절).
    void DispatchInputStack() noexcept;
    // 스택 쌓기: Pawn → PlayerController 층 → Push한 것, priority 안정 정렬. 할당 실패로 층을
    // 빠뜨렸으면 거짓이다(들어간 층은 그대로 쓴다).
    bool BuildInputLayers() noexcept;
    // 조종하는 Pawn의 InputComponent들을 inputLayers_ 뒤에 붙인다(플레이어 입력 컴포넌트 먼저 —
    // UE :2683-2693, Push 스택에 든 것·파괴 대기는 뺀다). 할당 실패면 거짓이다.
    bool AppendPawnInputComponents(ObjectHandle pawn) noexcept;
    // actor의 components를 actorComponents_에 옮긴다(World With 밖에서 돌기 위해). 할당 실패면
    // 거짓이다.
    bool CopyActorComponents(ObjectHandle actor) noexcept;
    // 그 층을 이번 틱 지금 전달해도 되는지다(앞 층의 콜백이 Pop·파괴·UnPossess·입력 끄기를 했을 수
    // 있다 — UE EvaluateInputDelegates의 IsValid 대응).
    bool IsInputLayerLive(const InputLayer& layer) noexcept;
    // actor의 첫 살아 있는 InputComponent다(없으면 null, 할당 실패 OutOfMemory).
    Result<ObjectHandle> FindLiveInputComponent(ObjectHandle actor) noexcept;
    // Possess한 Pawn에 플레이어 입력 컴포넌트를 만들고 hook을 부른다.
    void SetupPawnInput(ObjectHandle pawn) noexcept;
    // actor가 이 PlayerController가 조종하는 Pawn인지(참) 본다. Pawn이 아니면 거짓, 다른
    // Controller가 조종하거나 Controller가 없는 Pawn은 InvalidArgument다.
    Result<bool> IsOwnPawn(ObjectHandle actor) noexcept;

    void SetGestureRecognition(bool enabled) noexcept {
        if (enabled != gestureRecognizerEnabled_) {
            gestureRecognizerEnabled_ = enabled;
            gestures_.Reset();
        }
    }

protected:
    void OnTick(const TickContext& context) noexcept final;
    Result<void> OnFrameworkPostAwake() noexcept final;
    void OnFrameworkEndPlay(EndPlayReason reason) noexcept final;

private:
    ROCKS_NATIVE() input::InputState playerInput_;
    ROCKS_NATIVE() const input::InputState* inputSource_ = nullptr;
    ROCKS_NATIVE() input::TouchGestureRecognizer gestures_;
    ROCKS_NATIVE() bool gestureRecognizerEnabled_ = false;
    ROCKS_NATIVE() bool gestureRecognizerExplicit_ = false;
    ROCKS_NATIVE() input::PlayerActionInput actionInput_;
    ROCKS_NATIVE() input::InputActionBindings actionBindings_;
    ROCKS_NATIVE() input::HapticPlayer haptics_;
    ROCKS_NATIVE() input::MotorValues motorOutput_;
    ROCKS_NATIVE() float hapticScale_ = 1.0f;
    ROCKS_NATIVE() bool hapticsEnabled_ = true;
    ROCKS_NATIVE() uint32 gamepad_ = 0;
    // 입력 액션 평가가 실패하고 있다(경고를 실패가 이어지는 동안 한 번만 남긴다).
    ROCKS_NATIVE() bool inputEvaluationFailing_ = false;
    ROCKS_NATIVE() ObjectHandle localPlayer_;
    ROCKS_NATIVE() ObjectHandle cameraDirector_;
    ROCKS_NATIVE() Array<AppliedInputAsset> inputAssets_;
    // Push한 InputComponent(넣은 자리 순 — UE CurrentInputStack).
    ROCKS_NATIVE() Array<ObjectHandle> inputStack_;
    // 전달 작업 배열이다(이번 틱 층·층 사이 소비·Actor 컴포넌트 목록 — 용량을 다시 쓴다).
    ROCKS_NATIVE() Array<InputLayer> inputLayers_;
    ROCKS_NATIVE() Array<Guid> stackConsumed_;
    ROCKS_NATIVE() Array<ObjectHandle> actorComponents_;
    // PlayerController 층이 켜져 있다(UE bInputEnabled).
    ROCKS_NATIVE() bool inputEnabled_ = true;
    // 스택 쌓기가 할당 실패로 층을 빠뜨리고 있다(오류를 실패가 이어지는 동안 한 번만 남긴다).
    ROCKS_NATIVE() bool inputStackFailing_ = false;
    // OnFrameworkEndPlay가 lease를 놓은 뒤다(새 에셋 컨텍스트를 받지 않는다).
    ROCKS_NATIVE() bool inputAssetsClosed_ = false;
};

} // namespace rocks
