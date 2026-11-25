#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 충돌 채널·응답·충돌 켬 상태다(Docs/Design/CollisionData.ko.md 2.1·2.2절). 값과 순서는 UE 5.8과
// 같다. 물리 모듈(CR16)과 Jolt 어댑터는 이 값을 읽기만 한다.
namespace rocks {

// 충돌 채널이다(UE ECollisionChannel — E/Classes/Engine/EngineTypes.h:1098). 0–7은 엔진 기본
// 채널(object 또는 trace), 8–13은 엔진 예약, 14–63은 프로젝트가 이름을 주어 쓰는 채널이다. 이름이
// 없는 8–63 채널은 쓰이지 않는다(profile 표 — CollisionProfileTable). 응답 묶음
// (CollisionResponses)의 색인이기도 하다.
ROCKS_ENUM()
enum class CollisionChannel : uint8 {
    WorldStatic,
    WorldDynamic,
    Pawn,
    Visibility,
    Camera,
    PhysicsBody,
    Vehicle,
    Destructible,
    EngineTraceChannel1,
    EngineTraceChannel2,
    EngineTraceChannel3,
    EngineTraceChannel4,
    EngineTraceChannel5,
    EngineTraceChannel6,
    GameTraceChannel1,
    GameTraceChannel2,
    GameTraceChannel3,
    GameTraceChannel4,
    GameTraceChannel5,
    GameTraceChannel6,
    GameTraceChannel7,
    GameTraceChannel8,
    GameTraceChannel9,
    GameTraceChannel10,
    GameTraceChannel11,
    GameTraceChannel12,
    GameTraceChannel13,
    GameTraceChannel14,
    GameTraceChannel15,
    GameTraceChannel16,
    GameTraceChannel17,
    GameTraceChannel18,
    GameTraceChannel19,
    GameTraceChannel20,
    GameTraceChannel21,
    GameTraceChannel22,
    GameTraceChannel23,
    GameTraceChannel24,
    GameTraceChannel25,
    GameTraceChannel26,
    GameTraceChannel27,
    GameTraceChannel28,
    GameTraceChannel29,
    GameTraceChannel30,
    GameTraceChannel31,
    GameTraceChannel32,
    GameTraceChannel33,
    GameTraceChannel34,
    GameTraceChannel35,
    GameTraceChannel36,
    GameTraceChannel37,
    GameTraceChannel38,
    GameTraceChannel39,
    GameTraceChannel40,
    GameTraceChannel41,
    GameTraceChannel42,
    GameTraceChannel43,
    GameTraceChannel44,
    GameTraceChannel45,
    GameTraceChannel46,
    GameTraceChannel47,
    GameTraceChannel48,
    GameTraceChannel49,
    GameTraceChannel50
};

// 채널 수(응답 묶음의 칸 수)다.
inline constexpr uint32 CollisionChannelCount = 64;

// 편집기 gizmo 충돌용 엔진 예약 채널이다(UE COLLISION_GIZMO — EngineTypes.h:1183). 기본 응답이
// Ignore다.
inline constexpr CollisionChannel GizmoCollisionChannel = CollisionChannel::EngineTraceChannel1;

// 프로젝트가 이름·종류·기본 응답을 정할 수 있는 첫 채널이다(UE IS_VALID_COLLISIONCHANNEL —
// E/Private/Collision/CollisionProfile.cpp:14). 이 앞(0–7)은 엔진 고정이다.
inline constexpr CollisionChannel FirstConfigurableCollisionChannel =
    CollisionChannel::EngineTraceChannel1;

static_assert(static_cast<uint32>(CollisionChannel::GameTraceChannel50) + 1 ==
                  CollisionChannelCount,
              "CollisionChannel must have CollisionChannelCount values");

// 채널 번호다(응답 묶음 색인).
constexpr uint32 CollisionChannelIndex(CollisionChannel channel) noexcept {
    return static_cast<uint32>(channel);
}

// 한 채널에 대한 응답이다(UE ECollisionResponse — EngineTypes.h:1346). 순서가 의미를 갖는다: 두
// 쪽 응답 중 작은 것이 실효 응답이다(Ignore < Overlap < Block — CollisionResponses::Min).
ROCKS_ENUM()
enum class CollisionResponse : uint8 {
    Ignore,
    Overlap,
    Block
};

// 응답 종류 수다.
inline constexpr uint32 CollisionResponseCount = 3;

// body가 질의·시뮬레이션·탐침 중 무엇에 참여하는지다(UE ECollisionEnabled::Type —
// EngineTypes.h:1805).
// - NoCollision: 물리 표현을 만들지 않는다.
// - QueryOnly: 질의(ray·sweep·overlap)에만 쓴다.
// - PhysicsOnly: 시뮬레이션에만 쓴다.
// - QueryAndPhysics: 둘 다.
// - ProbeOnly: 접촉 데이터만 만들고 반작용하지 않는다(질의 없음).
// - QueryAndProbe: 질의 + 탐침.
ROCKS_ENUM()
enum class CollisionEnabled : uint8 {
    NoCollision,
    QueryOnly,
    PhysicsOnly,
    QueryAndPhysics,
    ProbeOnly,
    QueryAndProbe
};

// 충돌 켬 상태 종류 수다.
inline constexpr uint32 CollisionEnabledCount = 6;

// 질의에 참여하는지다(UE CollisionEnabledHasQuery — EngineTypes.h:1855).
constexpr bool CollisionEnabledHasQuery(CollisionEnabled value) noexcept {
    return value == CollisionEnabled::QueryOnly || value == CollisionEnabled::QueryAndPhysics ||
           value == CollisionEnabled::QueryAndProbe;
}

// 시뮬레이션에 참여하는지다(UE CollisionEnabledHasPhysics — EngineTypes.h:1849).
constexpr bool CollisionEnabledHasPhysics(CollisionEnabled value) noexcept {
    return value == CollisionEnabled::PhysicsOnly || value == CollisionEnabled::QueryAndPhysics;
}

// 탐침(접촉 데이터만)인지다(UE CollisionEnabledHasProbe — EngineTypes.h:1862).
constexpr bool CollisionEnabledHasProbe(CollisionEnabled value) noexcept {
    return value == CollisionEnabled::ProbeOnly || value == CollisionEnabled::QueryAndProbe;
}

// 두 충돌 켬 상태의 교집합이다(UE CollisionEnabledIntersection — EngineTypes.h:1868). body의 값과
// 형상의 값을 합칠 때 쓴다. UE의 5×5 행렬을 그대로 쓴다: 한쪽이 NoCollision이면 NoCollision이고,
// 대략 query는 둘 다 query일 때, physics는 둘 다 physics이고 probe가 끼지 않을 때 남으며, physics와
// probe가 만나면 probe가 된다(QueryOnly와 ProbeOnly처럼 겹치는 참여가 없으면 NoCollision).
ROCKS_COLLISION_API CollisionEnabled CollisionEnabledIntersection(CollisionEnabled a,
                                                                  CollisionEnabled b) noexcept;

} // namespace rocks
