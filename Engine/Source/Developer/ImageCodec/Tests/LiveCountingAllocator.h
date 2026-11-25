#pragma once

#include <Core/Memory/Allocator.h>

namespace rocks::image_codec_test {

// 살아 있는 블록 수를 세는 Allocator다(디코딩·부호화가 실패 경로에서도 받은 블록을 전부 돌려주는지
// 본다).
class LiveCountingAllocator final : public Allocator {
public:
    explicit LiveCountingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        auto allocated = inner_.Allocate(bytes, alignment, tag);
        if (allocated) {
            ++live_;
        }

        return allocated;
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address != nullptr) {
            --live_;
        }

        return inner_.Deallocate(address);
    }

    void TrimMemory() noexcept override {
        inner_.TrimMemory();
    }

    void MarkThreadIdle() noexcept override {
        inner_.MarkThreadIdle();
    }

    void MarkThreadActive() noexcept override {
        inner_.MarkThreadActive();
    }

    int64 Live() const noexcept {
        return live_;
    }

private:
    Allocator& inner_;
    int64 live_ = 0;
};

} // namespace rocks::image_codec_test
