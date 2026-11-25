#pragma once

#include <Collision/CollisionTypes.h>

namespace rocks {

// 채널마다 응답 하나를 담은 64칸 묶음이다(UE FCollisionResponseContainer —
// E/Classes/Engine/EngineTypes.h:1445, Docs/Design/CollisionData.ko.md 2.3절). 값 타입이고
// 리플렉션 타입이 아니다. 영속은 채널 이름과 응답의 차이 목록(ChannelResponse 배열)으로 한다.
//
// 사용 예:
//   CollisionResponses responses(CollisionResponse::Block);
//   responses.Set(CollisionChannel::Visibility, CollisionResponse::Ignore);
//   if (responses.Get(CollisionChannel::Pawn) == CollisionResponse::Block) { ... }
class CollisionResponses {
public:
    // 모든 칸이 Block이다.
    constexpr CollisionResponses() noexcept : CollisionResponses(CollisionResponse::Block) {
    }

    // 모든 칸을 response로 채운다.
    explicit constexpr CollisionResponses(CollisionResponse response) noexcept {
        for (CollisionResponse& slot : responses_) {
            slot = response;
        }
    }

    constexpr CollisionResponse Get(CollisionChannel channel) const noexcept {
        return responses_[CollisionChannelIndex(channel)];
    }

    // 한 칸을 바꾼다. 값이 바뀌었으면 참이다(UE SetResponse).
    constexpr bool Set(CollisionChannel channel, CollisionResponse response) noexcept {
        CollisionResponse& slot = responses_[CollisionChannelIndex(channel)];
        const bool changed = slot != response;
        slot = response;
        return changed;
    }

    // 모든 칸을 바꾼다. 한 칸이라도 바뀌었으면 참이다(UE SetAllChannels).
    constexpr bool SetAll(CollisionResponse response) noexcept {
        bool changed = false;
        for (CollisionResponse& slot : responses_) {
            changed = changed || slot != response;
            slot = response;
        }

        return changed;
    }

    // from인 칸만 to로 바꾼다. from인 칸이 하나라도 있었으면 참이다(같은 값으로 바꾼 경우도 —
    // UE ReplaceChannels, E/Private/PhysicsEngine/PhysUtils.cpp:234-247).
    constexpr bool Replace(CollisionResponse from, CollisionResponse to) noexcept {
        bool replaced = false;
        for (CollisionResponse& slot : responses_) {
            if (slot == from) {
                slot = to;
                replaced = true;
            }
        }

        return replaced;
    }

    // 칸마다 작은 응답(Ignore < Overlap < Block)이다. 두 body 사이의 실효 응답이다(UE
    // CreateMinContainer).
    static constexpr CollisionResponses Min(const CollisionResponses& a,
                                            const CollisionResponses& b) noexcept {
        CollisionResponses result;
        for (uint32 index = 0; index < CollisionChannelCount; ++index) {
            result.responses_[index] = a.responses_[index] < b.responses_[index]
                                           ? a.responses_[index]
                                           : b.responses_[index];
        }

        return result;
    }

    friend constexpr bool operator==(const CollisionResponses& a,
                                     const CollisionResponses& b) noexcept {
        for (uint32 index = 0; index < CollisionChannelCount; ++index) {
            if (a.responses_[index] != b.responses_[index]) {
                return false;
            }
        }

        return true;
    }

private:
    CollisionResponse responses_[CollisionChannelCount]{};
};

} // namespace rocks
