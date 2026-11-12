#include <Core/Containers/BitArray.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/HostFailure.h>

namespace rocks::test {

// 범위 오류가 조용한 메모리 접근으로 이어지지 않는지 자식 프로세스에서 확인한다.
int RunBitArrayInvalidIndexControl() noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    BitArray bits(allocator);
    return static_cast<bool>(bits[0]) ? 2 : 3;
}

} // namespace rocks::test
