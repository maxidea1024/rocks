#pragma once

#include <Collision/CollisionResponses.h>
#include <Core/Containers/Array.h>
#include <Core/Result.h>

namespace rocks::physics {

// 장면의 응답 묶음 표다(Docs/Design/PhysicsModule 6.1절 bit 11–31, 7.1.6절). 같은 64칸 응답을
// 쓰는 body가 칸 하나를 공유하고(참조 수), 색인을 object 층에 넣어 쌍 필터·질의 필터가 body의
// 응답을 찾는다.
//
// 칸 0은 "모든 채널 Block"으로 늘 있고 참조를 세지 않는다(Jolt를 직접 쓰는 시험의 층 색인 0이
// PH2까지와 같은 뜻이 된다. 컴포넌트 기본 응답은 Gizmo 채널이 Ignore라 일반 칸이다). 칸 1부터는
// Acquire가 만들고 참조 수가 0이 되면 빈 칸으로 돌린다.
//
// 스레드: 바꾸기(Acquire·Release)는 게임 thread에서 step 밖에만 한다. 읽기(Get)는 step 안의 job
// thread에서도 한다(바꾸기와 겹치지 않으므로 잠금이 없다 — 3.2절).
class ResponseSetTable {
public:
    explicit ResponseSetTable(Allocator& allocator) noexcept;

    // responses의 칸 색인을 돌려주고 참조 수를 하나 늘린다(없으면 만든다). 오류: 색인 bit를 넘는 칸
    // 수(CapacityExceeded), 할당 실패.
    Result<uint32> Acquire(const CollisionResponses& responses) noexcept;

    // Acquire 하나를 놓는다. 칸 0·범위 밖 색인은 무시한다.
    void Release(uint32 index) noexcept;

    // 색인의 응답이다. 범위 밖·빈 칸은 칸 0(모두 Block)이다(장면이 만든 층만 오므로 생기지 않는다).
    const CollisionResponses& Get(uint32 index) const noexcept;

    // 쓰는 칸 수다(칸 0 포함 — 진단).
    uint32 Count() const noexcept {
        return 1 + used_;
    }

private:
    struct Entry {
        CollisionResponses responses;
        uint32 references = 0;
    };

    // entries_[i]가 색인 i + 1이다.
    Array<Entry> entries_;
    Array<uint32> free_;
    uint32 used_ = 0;
};

} // namespace rocks::physics
