// object 층 bit와 PH1 필터(Docs/Design/PhysicsModule 6.1–6.2절, 7.1.1절).
#include "JoltLayers.h"

namespace rocks::physics {

namespace {

// 시뮬레이션에 참여하는 층인지다: QueryOnly가 아니고 physics·probe 표지가 있다.
bool Simulates(JPH::ObjectLayer layer) noexcept {
    if (ObjectLayerMotion(layer) == LayerMotion::QueryOnly) {
        return false;
    }

    return (ObjectLayerParticipation(layer) &
            (LayerParticipationPhysics | LayerParticipationProbe)) != 0;
}

} // namespace

JPH::uint BroadPhaseLayers::GetNumBroadPhaseLayers() const {
    return BroadPhaseLayerCount;
}

JPH::BroadPhaseLayer BroadPhaseLayers::GetBroadPhaseLayer(JPH::ObjectLayer layer) const {
    const LayerMotion motion = ObjectLayerMotion(layer);
    // 예약 값(3)은 QueryOnly로 둔다(시뮬레이션 쌍을 만들지 않는 쪽).
    const uint32 index = motion == LayerMotion::Static || motion == LayerMotion::Moving
                             ? static_cast<uint32>(motion)
                             : static_cast<uint32>(LayerMotion::QueryOnly);
    return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(index));
}

bool ObjectVsBroadPhaseFilter::ShouldCollide(JPH::ObjectLayer layer,
                                             JPH::BroadPhaseLayer broadPhase) const {
    if (!Simulates(layer)) {
        return false;
    }

    const auto other = static_cast<JPH::BroadPhaseLayer::Type>(broadPhase);
    if (other == static_cast<JPH::BroadPhaseLayer::Type>(LayerMotion::QueryOnly)) {
        return false;
    }

    // Static 끼리는 쌍을 만들지 않는다.
    return !(ObjectLayerMotion(layer) == LayerMotion::Static &&
             other == static_cast<JPH::BroadPhaseLayer::Type>(LayerMotion::Static));
}

bool ObjectLayerPairFilter::ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const {
    if (!Simulates(first) || !Simulates(second)) {
        return false;
    }

    return !(ObjectLayerMotion(first) == LayerMotion::Static &&
             ObjectLayerMotion(second) == LayerMotion::Static);
}

} // namespace rocks::physics
