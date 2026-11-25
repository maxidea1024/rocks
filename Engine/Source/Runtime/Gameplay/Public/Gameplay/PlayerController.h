#pragma once

#include <Core/Math/Vector3.h>
#include <Gameplay/Controller.h>
#include <World/Camera/CameraComponent.h>
#include <Input/InputState.h>
#include <Input/TouchGestureRecognizer.h>
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

    // 이 플레이어의 액션 바인딩이다(UE UEnhancedInputComponent — 5.1절). OnTick이 평가 뒤 전달한다.
    input::InputActionBindings& ActionBindings() noexcept {
        return actionBindings_;
    }

    // 입력 액션 평가에 쓰는 패드 번호다.
    uint32 InputGamepad() const noexcept {
        return gamepad_;
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
    ROCKS_NATIVE() uint32 gamepad_ = 0;
    // 입력 액션 평가가 실패하고 있다(경고를 실패가 이어지는 동안 한 번만 남긴다).
    ROCKS_NATIVE() bool inputEvaluationFailing_ = false;
    ROCKS_NATIVE() ObjectHandle localPlayer_;
    ROCKS_NATIVE() ObjectHandle cameraDirector_;
};

} // namespace rocks
