// object 층 bit와 시뮬레이션 필터(Docs/Design/PhysicsModule 6.1–6.2절, 7.1.1·7.1.6절).
#include "JoltLayers.h"

#include "JoltResponseSets.h"

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

    if (ObjectLayerMotion(first) == LayerMotion::Static &&
        ObjectLayerMotion(second) == LayerMotion::Static) {
        return false;
    }

    // 서로의 object type에 대한 응답의 최소가 Block일 때만 시뮬레이션 접촉이다(6.2절 2–3단계 — UE
    // SceneComponent.cpp:3089-3092). Overlap은 질의 기반 overlap이 다룬다.
    const auto firstChannel = static_cast<CollisionChannel>(ObjectLayerChannel(first));
    const auto secondChannel = static_cast<CollisionChannel>(ObjectLayerChannel(second));
    const CollisionResponse firstSide =
        responseSets_.Get(ObjectLayerResponseSet(first)).Get(secondChannel);
    const CollisionResponse secondSide =
        responseSets_.Get(ObjectLayerResponseSet(second)).Get(firstChannel);
    return firstSide == CollisionResponse::Block && secondSide == CollisionResponse::Block;
}

} // namespace rocks::physics
