#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/Vector3.h>
#include <Core/Math/VolumeShape.h>
#include <Meta/Annotations.h>
#include <World/Primitives/PrimitiveTypes.h>
#include <World/Primitives/PrimitiveComponent.h>

namespace rocks {

// 단순 기하 충돌 모양의 기반이다(UE UShapeComponent — Docs/Design/EngineRuntime 24.3.6절). 기본으로
// 게임에서 숨고(hiddenInGame) 동적 그림자를 드리우지 않으며 profile은 "OverlapAllDynamic"이다.
// 데이터만 있다: 충돌·overlap(CR16)과 디버그 표시(RS 단계) 연결 전에는 충돌하거나 그리지 않는다.
// bounds는 지금 끝까지 동작한다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API ShapeComponent : public PrimitiveComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 모양 색이다(UE ShapeComponent.cpp:55 — Box·Capsule 기본이기도 하다).
    static constexpr Color DefaultShapeColor = Color(223, 149, 157, 255);
    // 클래스 기본 충돌 profile(UE ShapeComponent.cpp:48)이다.
    static constexpr const char* ShapeCollisionProfileName = "OverlapAllDynamic";

    explicit ShapeComponent(Allocator& allocator) noexcept : PrimitiveComponent(allocator) {
        castDynamicShadow_ = false;
        InitializeHiddenInGame(true);
    }

    StringView DefaultCollisionProfileName() const noexcept override {
        return StringView(ShapeCollisionProfileName);
    }

    // 지역 bounds를 월드 Transform 행렬로 옮긴다(UE CalcBounds(LocalToWorld) — Bounds()는 월드
    // 행렬을 바로 쓴다).
    ComponentBounds CalcBounds(const Transform& world) const noexcept final;

    // 편집기·디버그 표시 색이다.
    Color ShapeColor() const noexcept {
        return shapeColor_;
    }

    void SetShapeColor(Color color) noexcept {
        Change(shapeColor_, color);
    }

    float LineThickness() const noexcept {
        return lineThickness_;
    }

    // 오류: 음수·비유한.
    Result<void> SetLineThickness(float thickness) noexcept;

    // 내비게이션 동적 장애물인지다(UE bDynamicObstacle — 영역 클래스는 내비게이션 체계 때).
    bool IsDynamicObstacle() const noexcept {
        return dynamicObstacle_;
    }

    void SetDynamicObstacle(bool obstacle) noexcept {
        Change(dynamicObstacle_, obstacle);
    }

    // 월드에 놓인 영역 형상이다(Docs/Design/EngineRuntime 28.3절): 중심 WorldLocation, 회전
    // WorldQuaternion, 크기는 scale을 적용한 값의 절댓값(24.3.6절 규칙). 오류: 월드 변환이
    // 비유한(InvalidArgument).
    virtual Result<VolumeShape> TryWorldVolumeShape() const noexcept = 0;

    // 점에서 형상 표면까지의 거리(cm)다. 안이면 0이다(UE GetSquaredDistanceToCollision의 물리 없는
    // 대응). 오류: 비유한 점·잘못된 월드 변환(InvalidArgument).
    //
    // 사용 예:
    //   auto distance = box.TryDistanceToShape(listenerLocation);
    //   if (distance && distance.Value() == 0.0) { ... }   // 영역 안
    Result<double> TryDistanceToShape(const Vec3d& point) const noexcept;

protected:
    // 지역 bounds다(파생이 채운다 — Shape는 boundsScale을 곱하지 않는다, UE와 같다).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override = 0;

    ROCKS_PROPERTY(ReadOnly, Name = "shapeColor") Color shapeColor_ = DefaultShapeColor;
    ROCKS_PROPERTY(ReadOnly, Name = "lineThickness") float lineThickness_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "dynamicObstacle") bool dynamicObstacle_ = false;
};

} // namespace rocks
