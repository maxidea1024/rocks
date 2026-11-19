#pragma once

#include <SpatialIndex/FlatBoundsSet.h>

#include "SpatialIndexInternal.h"

namespace rocks::spatial {

// 변경 함수 하나의 범위다(8절 위반 검출). Core AccessOverlapChecker의 변경 범위를 열어 읽기
// 범위·다른 변경과의 겹침을 Fatal로 잡고, 결과 세대를 새로 받아 이전 읽기 단계의 비트 집합을 낡게
// 만든다.
class FlatBoundsSet::WriterGuard {
public:
    explicit WriterGuard(FlatBoundsSet& set) noexcept : access_(set.access_) {
        set.epoch_ = NextMutationEpoch();
    }

    WriterGuard(const WriterGuard&) = delete;
    WriterGuard& operator=(const WriterGuard&) = delete;

private:
    WriteAccessScope access_;
};

} // namespace rocks::spatial
