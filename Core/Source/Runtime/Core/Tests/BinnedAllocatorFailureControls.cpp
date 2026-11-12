#include <Core/Memory/BinnedAllocator.h>
#include <TestSupport/HostFailure.h>
#include <cstdlib>

namespace rocks::test {
namespace {

// 자식 프로세스에서만 사용한다. 고의로 잘못된 주소를 돌려 계약 위반을 확인한다.
class InvalidBinnedBacking final : public Allocator {
public:
    explicit InvalidBinnedBacking(uint32 operation) noexcept : operation_(operation) {
    }

    Result<void*> Allocate(usize, usize, uint32) noexcept override {
        ++calls_;
        if (operation_ == 0) {
            return Result<void*>::Success(nullptr);
        }
        if (operation_ == 1) {
            return Result<void*>::Success(buffer_ + 1);
        }
        if (operation_ == 2) {
            return Result<void*>::Success(reinterpret_cast<void*>(MaxSize - 7));
        }
        if (operation_ == 4 && calls_ > 1) {
            return Result<void*>::Success(buffer_ + 64);
        }
        return Result<void*>::Success(buffer_);
    }

    Result<void> Deallocate(void*) noexcept override {
        return Result<void>::Success();
    }

private:
    alignas(64) uint8 buffer_[32768]{};
    uint32 operation_;
    uint32 calls_ = 0;
};
} // namespace

// 0: null, 1: 잘못된 정렬, 2: 범위 오버플로, 3: 대형 동일 주소,
// 4: 대형 부분 겹침, 5: 두 pool chunk 겹침, 6: pool과 대형 블록 겹침.
int RunBinnedAllocatorNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    InvalidBinnedBacking backing(operation);
    BinnedAllocator allocator(backing);
    if (operation <= 2) {
        (void)allocator.Allocate(5000, 8);
    } else if (operation <= 4) {
        (void)allocator.Allocate(5000, 8);
        (void)allocator.Allocate(5000, 8);
    } else if (operation == 5) {
        (void)allocator.Allocate(8, 8);
        (void)allocator.Allocate(24, 8);
    } else if (operation == 6) {
        (void)allocator.Allocate(8, 8);
        (void)allocator.Allocate(5000, 8);
    }
    // 누수나 destructor 진단을 이용한 거짓 양성을 방지한다.
    std::_Exit(0);
}

} // namespace rocks::test
