#pragma once

#include <Core/Memory/Allocator.h>

namespace rocks::test {

class NFailAllocator final : public Allocator {
public:
    explicit NFailAllocator(Allocator& allocator) noexcept : allocator_(allocator) {
    }

    NFailAllocator(const NFailAllocator&) = delete;
    NFailAllocator& operator=(const NFailAllocator&) = delete;

    void FailOnAttempt(uint64 oneBasedAttempt) noexcept {
        attempts_ = 0;
        failureAttempt_ = oneBasedAttempt;
    }

    void DisableFailure() noexcept {
        attempts_ = 0;
        failureAttempt_ = 0;
    }

    uint64 Attempts() const noexcept {
        return attempts_;
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++attempts_;

        if (failureAttempt_ != 0 && attempts_ == failureAttempt_) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return allocator_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return allocator_.Deallocate(address);
    }

    // 상위 allocator에 그대로 넘긴다(Allocator.h).
    void TrimMemory() noexcept override {
        allocator_.TrimMemory();
    }

    void MarkThreadIdle() noexcept override {
        allocator_.MarkThreadIdle();
    }

    void MarkThreadActive() noexcept override {
        allocator_.MarkThreadActive();
    }

private:
    Allocator& allocator_;
    uint64 attempts_ = 0;
    uint64 failureAttempt_ = 0;
};

} // namespace rocks::test
