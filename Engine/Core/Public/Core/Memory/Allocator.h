#pragma once

#include <Base/Types.h>
#include <Core/Result.h>

namespace rocks {

// 성공적으로 게시된 사용자 할당의 통계다. bytes는 요청 크기이며 alignment padding이나
// allocator 자체 ledger·OS 메모리 사용량까지 포함하는 값은 아니다.
struct AllocationStats {
    uint64 liveAllocations = 0;
    uint64 liveBytes = 0;
    uint64 totalAllocations = 0;
};

// byte storage의 할당·해제를 분리한다. 객체 생성/소멸은 수행하지 않는다.
// allocator는 반환 storage와 진행 중 호출보다 오래 살아야 한다. 동시 호출 허용 여부는 구현별
// 계약이다.
class Allocator {
public:
    virtual ~Allocator() = default;

    // 유효한 alignment는 0이 아닌 2의 거듭제곱이며 bytes 단위로 요청한다.
    // 0-byte는 성공한 nullptr, 실패는 caller에게 소유 allocation을 넘기지 않는다.
    virtual Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept = 0;
    // 같은 allocator의 살아 있는 allocation을 모든 사용자 종료 후 반환한다. nullptr는 무효
    // 연산이다. raw 주소만으로는 해제된 주소가 이후 다른 allocation에 재사용된 경우를 구별할 수
    // 없다.
    virtual Result<void> Deallocate(void* address) noexcept = 0;
};

} // namespace rocks
