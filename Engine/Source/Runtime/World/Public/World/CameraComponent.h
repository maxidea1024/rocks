#pragma once

#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/SceneComponent.h>

namespace rocks {

// 카메라 투영 방식이다(UE ECameraProjectionMode).
ROCKS_ENUM()
enum class CameraProjection : uint8 {
    Perspective,
    Orthographic
};

// 카메라 Component가 내는 view 값이다(UE FMinimalViewInfo의 이 단계 부분 —
// Docs/Design/EngineRuntime 23.4절). 위치는 cm, 회전은 단위 quaternion, 시야는 수평 도다. 렌더 쪽
// ViewDesc는 RS0의 WorldIntegration이 이 값에서 만든다.
struct CameraViewInfo {
    Vec3d location;
    Quatd rotation = Quatd::Identity();
    double fieldOfView = 90.0;
    CameraProjection projection = CameraProjection::Perspective;
    double orthoWidth = 512.0;
    double aspectRatio = 16.0 / 9.0;
    bool constrainAspectRatio = false;
    // 소유 Pawn의 controlRotation으로 회전을 바꿀지다(판정은 CameraDirector가 한다 — World는 Pawn을
    // 모른다).
    bool usePawnControlRotation = false;
};

// 카메라 시점 Component다(UE UCameraComponent, 23.4절). Actor에 붙이면 그 Actor가 view target일 때
// CameraDirector가 켜진 첫 CameraComponent의 GetCameraView를 쓴다(UE CalcCamera). 위치·회전은
// Component 월드 Transform이다.
//
// 사용 예(Pawn 생성자·OnAwake):
//   auto camera = GetWorld().AddComponent<CameraComponent>(Handle());
//   GetWorld().With<CameraComponent>(camera.Value(), [](CameraComponent& value) noexcept {
//       value.SetUsePawnControlRotation(true);   // 1인칭: 조준 방향으로
//       return value.SetFieldOfView(100.0);
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API CameraComponent : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 수평 시야(도, UE FieldOfView 90)·직교 폭(cm, UE OrthoWidth 512)·종횡비(UE 16:9)다.
    static constexpr double DefaultFieldOfView = 90.0;
    static constexpr double DefaultOrthoWidth = 512.0;
    static constexpr double DefaultAspectRatio = 16.0 / 9.0;
    // 시야 허용 범위다(UE FieldOfView ClampMin 5·ClampMax 170 — 그 밖은 거부한다).
    static constexpr double MinFieldOfView = 5.0;
    static constexpr double MaxFieldOfView = 170.0;

    explicit CameraComponent(Allocator& allocator) noexcept : SceneComponent(allocator) {
    }

    double FieldOfView() const noexcept {
        return fieldOfView_;
    }

    // 오류: [MinFieldOfView, MaxFieldOfView] 밖·비유한 값은 InvalidArgument(값 유지).
    Result<void> SetFieldOfView(double degrees) noexcept;

    CameraProjection Projection() const noexcept {
        return projection_;
    }

    void SetProjection(CameraProjection projection) noexcept {
        projection_ = projection;
    }

    double OrthoWidth() const noexcept {
        return orthoWidth_;
    }

    // 오류: 0 이하·비유한 값은 InvalidArgument.
    Result<void> SetOrthoWidth(double width) noexcept;

    double AspectRatio() const noexcept {
        return aspectRatio_;
    }

    // 오류: 0 이하·비유한 값은 InvalidArgument.
    Result<void> SetAspectRatio(double ratio) noexcept;

    bool ConstrainsAspectRatio() const noexcept {
        return constrainAspectRatio_;
    }

    void SetConstrainAspectRatio(bool constrain) noexcept {
        constrainAspectRatio_ = constrain;
    }

    bool UsesPawnControlRotation() const noexcept {
        return usePawnControlRotation_;
    }

    void SetUsePawnControlRotation(bool use) noexcept {
        usePawnControlRotation_ = use;
    }

    // view 값을 채운다(UE GetCameraView — CameraComponent.cpp:418). 파생은 재정의해 흔들림 등을
    // 더할 수 있다(기본: 월드 Transform·필드 값).
    virtual void GetCameraView(double deltaSeconds, CameraViewInfo& out) const noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "fieldOfView") double fieldOfView_ = DefaultFieldOfView;
    ROCKS_PROPERTY(ReadOnly, Name = "projection")
    CameraProjection projection_ = CameraProjection::Perspective;
    ROCKS_PROPERTY(ReadOnly, Name = "orthoWidth") double orthoWidth_ = DefaultOrthoWidth;
    ROCKS_PROPERTY(ReadOnly, Name = "aspectRatio") double aspectRatio_ = DefaultAspectRatio;
    ROCKS_PROPERTY(ReadOnly, Name = "constrainAspectRatio") bool constrainAspectRatio_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "usePawnControlRotation") bool usePawnControlRotation_ = false;
};

} // namespace rocks
