#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/PrimitiveComponent.h>

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

protected:
    // 지역 bounds다(파생이 채운다 — Shape는 boundsScale을 곱하지 않는다, UE와 같다).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override = 0;

    ROCKS_PROPERTY(ReadOnly, Name = "shapeColor") Color shapeColor_ = DefaultShapeColor;
    ROCKS_PROPERTY(ReadOnly, Name = "lineThickness") float lineThickness_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "dynamicObstacle") bool dynamicObstacle_ = false;
};

// 상자 모양이다(UE UBoxComponent). extent는 반 크기(cm)다.
ROCKS_CLASS()
class ROCKS_WORLD_API BoxComponent : public ShapeComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 반 크기(cm, UE BoxComponent.cpp:22)다.
    static constexpr double DefaultBoxHalfExtent = 32.0;

    explicit BoxComponent(Allocator& allocator) noexcept : ShapeComponent(allocator) {
    }

    const Vec3d& UnscaledBoxExtent() const noexcept {
        return boxExtent_;
    }

    // extent × 월드 scale(성분별, 부호 유지 — UE GetScaledBoxExtent)이다.
    Vec3d ScaledBoxExtent() const noexcept;

    // 오류: 비유한·음수 성분(UE는 검증하지 않는다 — 음수는 상자가 뒤집힌다).
    Result<void> SetBoxExtent(const Vec3d& extent) noexcept;

protected:
    // 지역 (0, E, |E|)다(UE BoxComponent.cpp:122).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "boxExtent")
    Vec3d boxExtent_ = Vec3d(DefaultBoxHalfExtent, DefaultBoxHalfExtent, DefaultBoxHalfExtent);
};

// 구 모양이다(UE USphereComponent).
ROCKS_CLASS()
class ROCKS_WORLD_API SphereComponent : public ShapeComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 반지름(cm, UE SphereComponent.cpp:19)과 색(:20)이다.
    static constexpr float DefaultSphereRadius = 32.0f;
    static constexpr Color DefaultSphereColor = Color(255, 0, 0, 255);

    explicit SphereComponent(Allocator& allocator) noexcept : ShapeComponent(allocator) {
        shapeColor_ = DefaultSphereColor;
    }

    float UnscaledSphereRadius() const noexcept {
        return sphereRadius_;
    }

    // 반지름 × 최소 축 scale(절댓값 — UE GetShapeScale·GetMinimumAxisScale)이다.
    float ScaledSphereRadius() const noexcept;

    // 오류: 음수·비유한.
    Result<void> SetSphereRadius(float radius) noexcept;

protected:
    // 지역 (0, (R, R, R), R)다(UE SphereComponent.cpp:25).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "sphereRadius") float sphereRadius_ = DefaultSphereRadius;
};

// 캡슐 모양이다(UE UCapsuleComponent). 축은 지역 Z이고 반 높이는 반구를 포함한다(늘 반지름 이상).
ROCKS_CLASS()
class ROCKS_WORLD_API CapsuleComponent : public ShapeComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 반지름·반 높이(cm, UE CapsuleComponent.cpp:23–24)다.
    static constexpr float DefaultCapsuleRadius = 22.0f;
    static constexpr float DefaultCapsuleHalfHeight = 44.0f;

    explicit CapsuleComponent(Allocator& allocator) noexcept : ShapeComponent(allocator) {
    }

    float UnscaledCapsuleRadius() const noexcept {
        return capsuleRadius_;
    }

    float UnscaledCapsuleHalfHeight() const noexcept {
        return capsuleHalfHeight_;
    }

    // 반구를 뺀 반 높이다(UE GetUnscaledCapsuleHalfHeight_WithoutHemisphere).
    float UnscaledCapsuleHalfHeightWithoutHemisphere() const noexcept {
        return capsuleHalfHeight_ - capsuleRadius_;
    }

    // 반지름 × min(scale.x, scale.y)·반 높이 × scale.z다(UE GetScaledCapsuleRadius·HalfHeight).
    float ScaledCapsuleRadius() const noexcept;
    float ScaledCapsuleHalfHeight() const noexcept;

    float ScaledCapsuleHalfHeightWithoutHemisphere() const noexcept {
        return ScaledCapsuleHalfHeight() - ScaledCapsuleRadius();
    }

    // 반 높이는 max(halfHeight, radius)로 올린다(UE SetCapsuleSize — CapsuleComponent.cpp:171).
    // 오류: 음수·비유한(값 유지).
    Result<void> SetCapsuleSize(float radius, float halfHeight) noexcept;

    Result<void> SetCapsuleRadius(float radius) noexcept {
        return SetCapsuleSize(radius, capsuleHalfHeight_);
    }

    Result<void> SetCapsuleHalfHeight(float halfHeight) noexcept {
        return SetCapsuleSize(capsuleRadius_, halfHeight);
    }

protected:
    // 지역 (0, (r, r, h), h)다(UE CapsuleComponent.cpp:104).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "capsuleRadius") float capsuleRadius_ = DefaultCapsuleRadius;
    ROCKS_PROPERTY(ReadOnly, Name = "capsuleHalfHeight")
    float capsuleHalfHeight_ = DefaultCapsuleHalfHeight;
};

} // namespace rocks
