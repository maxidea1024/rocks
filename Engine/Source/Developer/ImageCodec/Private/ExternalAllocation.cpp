#include "ExternalAllocation.h"

#include <Base/NumericLimits.h>
#include <Platform/Memory.h>

// 외부 라이브러리 할당 묶음이다(Docs/Design/AssetSystem 12.10절 공통 규칙 1). 스레드 지역 상태라
// 병렬 임포트가 서로의 Allocator를 쓰지 않는다.
namespace rocks::image::detail {
namespace {

// 블록 머리다 — 크기가 정렬과 같아 뒤따르는 사용자 영역도 16 byte 정렬이다.
struct ExternalBlockHeader {
    Allocator* allocator;
    usize bytes;
};

constexpr usize ExternalAlignment = 16;
constexpr usize ExternalHeaderBytes = ExternalAlignment;
static_assert(sizeof(ExternalBlockHeader) <= ExternalHeaderBytes,
              "External block header must fit in the alignment unit");

thread_local constinit Allocator* externalAllocator = nullptr;
thread_local constinit bool externalFailed = false;

ExternalBlockHeader* ExternalHeaderOf(void* address) noexcept {
    return reinterpret_cast<ExternalBlockHeader*>(static_cast<uint8*>(address) -
                                                  ExternalHeaderBytes);
}

} // namespace

ExternalAllocationScope::ExternalAllocationScope(Allocator& allocator) noexcept
    : previousAllocator_(externalAllocator), previousFailed_(externalFailed) {
    externalAllocator = &allocator;
    externalFailed = false;
}

ExternalAllocationScope::~ExternalAllocationScope() noexcept {
    externalAllocator = previousAllocator_;
    externalFailed = previousFailed_;
}

bool ExternalAllocationScope::AllocationFailed() const noexcept {
    return externalFailed;
}

void* ExternalAllocate(usize bytes) noexcept {
    Allocator* allocator = externalAllocator;
    if (allocator == nullptr || bytes > NumericLimits<usize>::Max() - ExternalHeaderBytes) {
        externalFailed = true;
        return nullptr;
    }

    auto block = allocator->Allocate(bytes + ExternalHeaderBytes, ExternalAlignment);
    if (!block) {
        externalFailed = true;
        return nullptr;
    }

    auto* header = static_cast<ExternalBlockHeader*>(block.Value());
    header->allocator = allocator;
    header->bytes = bytes;
    return static_cast<uint8*>(block.Value()) + ExternalHeaderBytes;
}

void* ExternalAllocateZeroed(usize count, usize size) noexcept {
    if (size != 0 && count > NumericLimits<usize>::Max() / size) {
        externalFailed = true;
        return nullptr;
    }

    void* address = ExternalAllocate(count * size);
    if (address != nullptr) {
        platform::MemoryZero(address, count * size);
    }

    return address;
}

void* ExternalReallocate(void* address, usize bytes) noexcept {
    if (address == nullptr) {
        return ExternalAllocate(bytes);
    }

    void* resized = ExternalAllocate(bytes);
    if (resized == nullptr) {
        return nullptr;
    }

    const usize kept = ExternalHeaderOf(address)->bytes;
    platform::MemoryCopy(resized, address, kept < bytes ? kept : bytes);
    ExternalFree(address);
    return resized;
}

void ExternalFree(void* address) noexcept {
    if (address == nullptr) {
        return;
    }

    ExternalBlockHeader* header = ExternalHeaderOf(address);
    (void)header->allocator->Deallocate(header);
}

} // namespace rocks::image::detail
