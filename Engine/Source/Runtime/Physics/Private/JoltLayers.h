#pragma once

#include <Base/Types.h>

// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
// clang-format on

namespace rocks::physics {

// object 층 32 bit 배치다(Docs/Design/PhysicsModule 6.1절 PMD12):
//   bit 0–5   object type(충돌 채널 0–63)
//   bit 6–7   운동 묶음(LayerMotion)
//   bit 8–10  참여 표지(LayerParticipation*)
//   bit 11–31 응답 묶음 색인(장면의 응답 묶음 표 — PH3)
inline constexpr uint32 LayerChannelBits = 6;
inline constexpr uint32 LayerChannelMask = (1u << LayerChannelBits) - 1u;
inline constexpr uint32 LayerMotionShift = 6;
inline constexpr uint32 LayerMotionMask = 0x3u;
inline constexpr uint32 LayerParticipationShift = 8;
inline constexpr uint32 LayerParticipationMask = 0x7u;
inline constexpr uint32 LayerResponseSetShift = 11;
inline constexpr uint32 LayerResponseSetBits = 21;
inline constexpr uint32 LayerResponseSetMask = (1u << LayerResponseSetBits) - 1u;

// 운동 묶음이다. broadphase 층 번호와 같다.
enum class LayerMotion : uint8 {
    Static = 0,
    // Kinematic·Dynamic.
    Moving = 1,
    // 시뮬레이션에 참여하지 않는 body(UE QueryOnly — 질의만).
    QueryOnly = 2
};

// broadphase 층 수다(Static·Moving·QueryOnly).
inline constexpr JPH::uint BroadPhaseLayerCount = 3;

// 참여 표지다(실효 충돌 켬에서 — CollisionData 2.2절).
inline constexpr uint32 LayerParticipationPhysics = 1u << 0;
inline constexpr uint32 LayerParticipationQuery = 1u << 1;
inline constexpr uint32 LayerParticipationProbe = 1u << 2;

// 칸 값으로 object 층을 만든다. 범위를 넘는 칸은 잘린다(호출자가 범위를 지킨다).
constexpr JPH::ObjectLayer MakeObjectLayer(uint32 channel, LayerMotion motion, uint32 participation,
                                           uint32 responseSet) noexcept {
    return JPH::ObjectLayer((channel & LayerChannelMask) |
                            ((static_cast<uint32>(motion) & LayerMotionMask) << LayerMotionShift) |
                            ((participation & LayerParticipationMask) << LayerParticipationShift) |
                            ((responseSet & LayerResponseSetMask) << LayerResponseSetShift));
}

constexpr LayerMotion ObjectLayerMotion(JPH::ObjectLayer layer) noexcept {
    return static_cast<LayerMotion>((static_cast<uint32>(layer) >> LayerMotionShift) &
                                    LayerMotionMask);
}

constexpr uint32 ObjectLayerParticipation(JPH::ObjectLayer layer) noexcept {
    return (static_cast<uint32>(layer) >> LayerParticipationShift) & LayerParticipationMask;
}

// object 층 → broadphase 층(운동 묶음 bit — 6.1절).
class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override;
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override;
    // GetBroadPhaseLayerName은 JPH_EXTERNAL_PROFILE·JPH_PROFILE_ENABLED에서만 있다(반입은 둘 다
    // 끔).
};

// 시뮬레이션 쌍의 object 층 대 broadphase 층 판정이다. 6.2절 1단계와 같은 뜻으로 거른다.
class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override;
};

// 시뮬레이션 쌍 필터다. PH1은 6.2절 1단계(둘 다 Static, QueryOnly, physics·probe 표지 없음 →
// 거짓)까지이고, 응답 묶음 판정은 PH3에서 더한다(7.1.1절).
class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override;
};

} // namespace rocks::physics
