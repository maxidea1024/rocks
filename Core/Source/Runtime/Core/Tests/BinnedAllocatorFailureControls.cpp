#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/HostFailure.h>
#include <stdlib.h>

namespace rocks::test {
namespace {

// 자식 프로세스에서만 사용한다. 고의로 잘못된 주소를 돌려 계약 위반을 확인한다.
class InvalidBinnedBacking final : public Allocator {
public:
    explicit InvalidBinnedBacking(uint32 operation) noexcept : operation_(operation) {
    }

    // operation 4가 돌려줄 주소다(allocator가 예약한 범위 안의 주소).
    void* insideRange = nullptr;

    Result<void*> Allocate(usize, usize, uint32) noexcept override {
        if (operation_ == 0) {
            return Result<void*>::Success(nullptr);
        }

        if (operation_ == 1) {
            return Result<void*>::Success(buffer_ + 1);
        }

        if (operation_ == 2) {
            constexpr uptr NearAddressEnd = MaxSize - 4095;
            return Result<void*>::Success(reinterpret_cast<void*>(NearAddressEnd));
        }

        if (operation_ == 4) {
            return Result<void*>::Success(insideRange);
        }

        return Result<void*>::Success(buffer_);
    }

    Result<void> Deallocate(void*) noexcept override {
        return Result<void>::Success();
    }

private:
    alignas(4096) uint8 buffer_[32768]{};
    uint32 operation_;
};

// 큰 경로로 가는 요청 크기다(MaxSmallBytes 초과).
constexpr usize LargeRequest = 20000;

} // namespace

// 상위 allocator가 돌려준 잘못된 저장소와 남은 할당의 파괴가 Fatal인지 본다.
// 0: null, 1: 잘못된 정렬, 2: 주소 넘침, 3: 살아 있는 큰 블록과 같은 주소, 4: 예약 범위 안의 주소,
// 5: 작은 블록을 남긴 파괴, 6: 큰 블록을 남긴 파괴.
int RunBinnedAllocatorNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    constexpr uint32 LiveSmall = 5;
    constexpr uint32 LiveLarge = 6;
    if (operation == LiveSmall || operation == LiveLarge) {
        SystemAllocator system;
        {
            BinnedAllocator allocator(system);
            (void)allocator.Allocate(operation == LiveSmall ? 48 : LargeRequest, 16);
        }

        // 파괴가 Fatal이 아니면 여기에 온다.
        _Exit(0);
    }

    InvalidBinnedBacking backing(operation);
    BinnedAllocator allocator(backing);
    if (operation == 4) {
        auto small = allocator.Allocate(48, 16);
        backing.insideRange = small ? small.Value() : nullptr;
    }

    (void)allocator.Allocate(LargeRequest, 16);
    if (operation == 3) {
        (void)allocator.Allocate(LargeRequest, 16);
    }

    // 누수나 destructor 진단을 이용한 거짓 양성을 방지한다.
    _Exit(0);
}

} // namespace rocks::test
