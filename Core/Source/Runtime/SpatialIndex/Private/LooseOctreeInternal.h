#pragma once

#include <Core/Diagnostics/LogCategory.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <SpatialIndex/LooseOctree.h>

namespace rocks::spatial {

inline constexpr LogCategory LogSpatialIndex{"SpatialIndex"};

// 변경 함수 하나의 범위다(6.9절 위반 검출). writer 표시를 세운 뒤 reader 수를 읽고,
// SpatialReadScope는 reader 수를 올린 뒤 writer 표시를 읽는다. 두 쪽 모두 쓰기 뒤 읽기를 seq_cst로
// 하므로 겹치면 적어도 한쪽이 상대를 본다.
class LooseOctree::WriterGuard {
public:
    explicit WriterGuard(LooseOctree& tree) noexcept : tree_(tree) {
        if (platform::AtomicExchange(&tree.writer_, 1u) != 0) {
            platform::Fatal("LooseOctree mutation overlaps another mutation");
        }
        if (platform::AtomicLoad(&tree.readers_) != 0) {
            platform::Fatal("LooseOctree mutation while a read scope is open");
        }
    }

    ~WriterGuard() noexcept {
        platform::AtomicStore(&tree_.writer_, 0u);
    }

    WriterGuard(const WriterGuard&) = delete;
    WriterGuard& operator=(const WriterGuard&) = delete;

private:
    LooseOctree& tree_;
};

// 삽입 위치 계획이다. 트리를 바꾸지 않고 계산한다(6.5절 수용 판정).
struct LooseOctree::Placement {
    // 루트 확장 한도 안에 들어가지 않는다. 범위 밖 목록에 넣는다.
    bool outOfRange = false;
    // 빈 트리라 루트를 이 프록시 중심의 initialRootLevel 격자 셀로 다시 잡는다.
    bool reset = false;
    int64 resetOrigin[3] = {0, 0, 0};
    // 루트 확장 단계 수다.
    uint32 expansions = 0;
    // 확장에 새 블록이 필요한가(루트에 내용이 있다).
    bool expansionNeedsBlocks = false;
};

} // namespace rocks::spatial
