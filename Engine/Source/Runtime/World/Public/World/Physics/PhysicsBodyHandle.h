#pragma once

#include <Base/Types.h>

namespace rocks {

// 물리 장면 안 body 하나의 handle이다(Docs/Design/PhysicsModule 1.3절 — UE FBodyInstance의 실행
// 상태 자리). 장면의 body 칸 번호와 그 칸의 세대다. 세대 0은 "없음"이다(기본값). body를 지우면 칸
// 세대가 올라가 옛 handle은 StaleHandle이 된다. 저장하지 않는다(실행 중 값).
struct PhysicsBodyHandle {
    uint32 index = 0;
    uint32 generation = 0;

    bool IsNull() const noexcept {
        return generation == 0;
    }

    bool operator==(const PhysicsBodyHandle&) const noexcept = default;
};

} // namespace rocks
