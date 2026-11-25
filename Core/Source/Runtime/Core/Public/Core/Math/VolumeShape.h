#pragma once

#include <Base/Types.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <Core/Result.h>

namespace rocks {

// 영역 형상의 종류다(Docs/Design/EngineRuntime 28.2절 — UE brush volume 대체, ComponentCatalog
// CD1).
enum class VolumeShapeKind : uint8 {
    Box,
    Sphere,
    Capsule
};

// 월드에 놓인 영역 형상이다(후처리 볼륨·오디오 리버브 존·물리 볼륨이 같이 쓴다). 크기는 scale을
// 이미 적용한 월드 값(cm)이다. 물리 엔진 없이 점까지의 거리를 정확히 잰다(TryDistanceToVolume).
//
// 사용 예:
//   auto zone = VolumeShape::TryBox(Vec3d(0, 0, 100), Quatd::Identity(), Vec3d(500, 500, 100));
//   auto distance = TryDistanceToVolume(zone.Value(), listener);   // 안이면 0
//
// 스레드: 값 타입이다. 여러 스레드가 읽기만 하면 안전하다.
struct VolumeShape {
    VolumeShapeKind kind = VolumeShapeKind::Box;
    Vec3d center;
    // 단위 quaternion이다(Try* 가 정규화한다).
    Quatd rotation = Quatd::Identity();
    // 상자 반 크기다.
    Vec3d boxHalfExtent;
    // 구·캡슐 반지름이다.
    double radius = 0.0;
    // 캡슐의 반구를 포함한 반 높이다(UE 캡슐과 같다 — 축은 회전된 +Z). 항상 radius 이상이다.
    double halfHeight = 0.0;

    // 오류(InvalidArgument): 비유한 값, 음수 크기, 정규화할 수 없는 회전.
    ROCKS_CORE_API static Result<VolumeShape> TryBox(const Vec3d& center, const Quatd& rotation,
                                                     const Vec3d& halfExtent) noexcept;
    ROCKS_CORE_API static Result<VolumeShape> TrySphere(const Vec3d& center,
                                                        double radius) noexcept;
    // halfHeight < radius면 radius로 올린다(UE SetCapsuleSize).
    ROCKS_CORE_API static Result<VolumeShape> TryCapsule(const Vec3d& center, const Quatd& rotation,
                                                         double radius, double halfHeight) noexcept;
};

// 점에서 형상 표면까지의 거리(cm)다. 안(경계 포함)이면 0이다(UE GetSquaredDistanceToCollision의
// 물리 없는 대응). 오류(InvalidArgument): 비유한 점, 잘못된 형상(Try* 밖에서 고친 값).
ROCKS_CORE_API Result<double> TryDistanceToVolume(const VolumeShape& shape,
                                                  const Vec3d& point) noexcept;

} // namespace rocks
