#pragma once

#include <Collision/CollisionProfileTable.h>
#include <Collision/CollisionResponses.h>
#include <Core/Text/StringView.h>

namespace rocks {

// 장면 질의가 맞힐 body를 고르는 규칙이다(Docs/Design/PhysicsModule 4.2·4.4절). UE는 필터 종류마다
// 함수가 따로(ByChannel·ByObjectType·ByProfile)지만 Rocks는 이 값 하나로 받는다(PMD8 — 의미는
// 같다).
//
// - Channel: body 응답 묶음의 channel 응답과 질의 응답 묶음(responses)의 body object type 응답 중
//   작은 것이 hit 종류다(Ignore면 제외, Overlap은 겹침, Block은 막음 — UE
//   FCollisionResponseParams).
// - ObjectTypes: body object type의 bit(CollisionObjectTypeBit)가 objectTypes에 있으면 막음이다(UE
//   FCollisionObjectQueryParams). 빈 집합(0)은 UE처럼 WorldStatic 채널·모두 Block 질의가 된다.
// - Profile: profile 표에서 그 profile의 object type을 채널로, 응답 묶음을 응답으로 써서 Channel
//   질의를 한다. 표에 없으면 Warning 뒤 WorldStatic·모두 Block이다. profile은 질의 호출 동안
//   읽는다.
//
// 사용 예:
//   QueryFilter visibility = QueryFilter::ByChannel(CollisionChannel::Visibility);
//   QueryFilter pawns = QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::Pawn));
struct QueryFilter {
    enum class Kind : uint8 {
        Channel,
        ObjectTypes,
        Profile
    };

    Kind kind = Kind::Channel;
    // Channel 질의의 채널이다.
    CollisionChannel channel = CollisionChannel::Visibility;
    // Channel 질의의 응답 묶음이다(UE FCollisionResponseParams 기본 — 모두 Block).
    CollisionResponses responses{CollisionResponse::Block};
    // ObjectTypes 질의의 채널 번호 bit 집합이다(64칸 — UE ObjectTypesToQuery).
    uint64 objectTypes = 0;
    // Profile 질의의 profile 이름이다.
    StringView profile;

    static QueryFilter
    ByChannel(CollisionChannel channel,
              const CollisionResponses& responses = CollisionResponses{}) noexcept {
        QueryFilter filter;
        filter.kind = Kind::Channel;
        filter.channel = channel;
        filter.responses = responses;
        return filter;
    }

    static QueryFilter ByObjectTypes(uint64 objectTypes) noexcept {
        QueryFilter filter;
        filter.kind = Kind::ObjectTypes;
        filter.objectTypes = objectTypes;
        return filter;
    }

    static QueryFilter ByProfile(StringView profile) noexcept {
        QueryFilter filter;
        filter.kind = Kind::Profile;
        filter.profile = profile;
        return filter;
    }
};

// object type 하나의 bit다(QueryFilter::objectTypes).
constexpr uint64 CollisionObjectTypeBit(CollisionChannel channel) noexcept {
    return uint64{1} << CollisionChannelIndex(channel);
}

// trace 채널(Visibility·Camera·trace 사용자 채널)을 뺀 모든 채널이다. 이름 없는 사용자 채널도 든다
// (UE FCollisionObjectQueryParams::AllObjects — CollisionProfile.cpp
// RemoveFromAllObjectsQueryFlag).
ROCKS_WORLD_API uint64 AllObjectTypes(const CollisionProfileTable& table) noexcept;

// 정적 object 채널(WorldStatic과 staticObject 사용자 채널)이다(UE AllStaticObjects).
ROCKS_WORLD_API uint64 AllStaticObjectTypes(const CollisionProfileTable& table) noexcept;

// AllObjectTypes에서 정적 채널을 뺀 것이다(UE AllDynamicObjects).
ROCKS_WORLD_API uint64 AllDynamicObjectTypes(const CollisionProfileTable& table) noexcept;

} // namespace rocks
