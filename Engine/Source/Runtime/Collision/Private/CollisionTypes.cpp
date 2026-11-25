#include <Collision/CollisionTypes.h>

// 충돌 켬 교집합이다(Docs/Design/CollisionData.ko.md 2.2절).
namespace rocks {

namespace {

// UE CollisionEnabledIntersection의 행렬이다(E/Classes/Engine/EngineTypes.h:1883-1891). 첫 칸
// NoCollision을 뺀 다섯 값(QueryOnly … QueryAndProbe) 순서다.
constexpr uint32 IntersectionMatrixSize = CollisionEnabledCount - 1;

constexpr CollisionEnabled IntersectionMatrix[IntersectionMatrixSize][IntersectionMatrixSize] = {
    {CollisionEnabled::QueryOnly, CollisionEnabled::NoCollision, CollisionEnabled::QueryOnly,
     CollisionEnabled::NoCollision, CollisionEnabled::QueryOnly},
    {CollisionEnabled::NoCollision, CollisionEnabled::PhysicsOnly, CollisionEnabled::PhysicsOnly,
     CollisionEnabled::ProbeOnly, CollisionEnabled::ProbeOnly},
    {CollisionEnabled::QueryOnly, CollisionEnabled::PhysicsOnly, CollisionEnabled::QueryAndPhysics,
     CollisionEnabled::ProbeOnly, CollisionEnabled::QueryAndProbe},
    {CollisionEnabled::NoCollision, CollisionEnabled::ProbeOnly, CollisionEnabled::ProbeOnly,
     CollisionEnabled::ProbeOnly, CollisionEnabled::ProbeOnly},
    {CollisionEnabled::QueryOnly, CollisionEnabled::ProbeOnly, CollisionEnabled::QueryAndProbe,
     CollisionEnabled::ProbeOnly, CollisionEnabled::QueryAndProbe},
};

} // namespace

CollisionEnabled CollisionEnabledIntersection(CollisionEnabled a, CollisionEnabled b) noexcept {
    if (a == CollisionEnabled::NoCollision || b == CollisionEnabled::NoCollision) {
        return CollisionEnabled::NoCollision;
    }

    return IntersectionMatrix[static_cast<uint32>(a) - 1][static_cast<uint32>(b) - 1];
}

} // namespace rocks
