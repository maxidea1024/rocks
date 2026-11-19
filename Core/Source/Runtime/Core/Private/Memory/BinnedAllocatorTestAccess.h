#pragma once

#include "BinnedAllocatorTestHooks.h"

// BinnedAllocator의 시험 통로다(friend). 제품 코드는 쓰지 않는다 — Tests/ 소스만 include한다(정책
// 검사 test-access-include). 시험은 <Core/Private/Memory/BinnedAllocatorTestAccess.h>로 쓴다.
// 함수는 BinnedAllocatorTestHooks.h의 BinnedAllocator::TestHooks 그대로다(실패 주입, 등급 표, 내부
// 상태 조회).
namespace rocks::detail {

struct BinnedAllocatorTestAccess : BinnedAllocator::TestHooks {};

} // namespace rocks::detail
