// 글꼴 라이브러리의 프로세스 전역 상태다(Docs/Design/Font 6.8·8.2절): ICU 할당 hook과 데이터 등록,
// HarfBuzz 할당 hook(hb_*_impl = RocksHarfBuzz*). 둘 다 프로세스 수명 Rocks 할당기를 쓴다 —
// HarfBuzz·ICU의 지연 정적 객체가 프로세스 종료 때 풀리므로 할당기는 소멸하지 않는다. u_cleanup은
// 부르지 않는다.
#include "TextLibraries.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/Sync.h>
#include <Base/NumericLimits.h>
#include <Base/PlacementNew.h>
#include <stddef.h>
#include <string.h>
#include <unicode/uclean.h>
#include <unicode/udata.h>
#include <unicode/umachine.h>

namespace rocks::text::detail {

namespace {

// 라이브러리 블록 정렬(malloc과 같은 보장)이다.
constexpr usize LibraryBlockAlignment = alignof(max_align_t);

// ICU·HarfBuzz realloc hook은 이전 크기를 주지 않으므로 블록 앞에 크기를 적는다.
constexpr usize LibraryBlockHeader = alignof(max_align_t);

// 프로세스 수명 할당기다(소멸하지 않는 정적 저장소).
SystemAllocator& LibraryAllocator() noexcept {
    alignas(SystemAllocator) static unsigned char storage[sizeof(SystemAllocator)];
    static SystemAllocator* allocator = ::new (static_cast<void*>(storage)) SystemAllocator();
    return *allocator;
}

void* AllocateBlock(size_t size) noexcept {
    if (size > NumericLimits<size_t>::Max() - LibraryBlockHeader) {
        return nullptr;
    }

    Result<void*> block =
        LibraryAllocator().Allocate(LibraryBlockHeader + size, LibraryBlockAlignment);
    if (!block) {
        return nullptr;
    }

    auto* bytes = static_cast<uint8*>(block.Value());
    memcpy(bytes, &size, sizeof(size));
    return bytes + LibraryBlockHeader;
}

void FreeBlock(void* memory) noexcept {
    if (memory != nullptr) {
        (void)LibraryAllocator().Deallocate(static_cast<uint8*>(memory) - LibraryBlockHeader);
    }
}

void* ReallocateBlock(void* memory, size_t size) noexcept {
    void* resized = AllocateBlock(size);
    if (resized == nullptr || memory == nullptr) {
        return resized;
    }

    size_t previous = 0;
    memcpy(&previous, static_cast<uint8*>(memory) - LibraryBlockHeader, sizeof(previous));
    memcpy(resized, memory, previous < size ? previous : size);
    FreeBlock(memory);
    return resized;
}

void* U_CALLCONV IcuAlloc(const void*, size_t size) {
    return AllocateBlock(size);
}

void* U_CALLCONV IcuRealloc(const void*, void* memory, size_t size) {
    return ReallocateBlock(memory, size);
}

void U_CALLCONV IcuFree(const void*, void* memory) {
    FreeBlock(memory);
}

struct LibraryState {
    platform::Mutex mutex;
    const uint8* icuData = nullptr;
    usize icuDataSize = 0;
    bool ready = false;
};

LibraryState& State() noexcept {
    alignas(LibraryState) static unsigned char storage[sizeof(LibraryState)];
    static LibraryState* state = ::new (static_cast<void*>(storage)) LibraryState();
    return *state;
}

} // namespace

Result<void> Initialize(Span<const uint8> icuData) noexcept {
    if (icuData.Size() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    LibraryState& state = State();
    platform::LockGuard lock(state.mutex);
    if (state.ready) {
        const bool same = state.icuData == icuData.Data() && state.icuDataSize == icuData.Size();
        return same ? Result<void>::Success() : Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    UErrorCode status = U_ZERO_ERROR;
    u_setMemoryFunctions(nullptr, IcuAlloc, IcuRealloc, IcuFree, &status);
    if (U_FAILURE(status)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    udata_setCommonData(icuData.Data(), &status);
    if (U_FAILURE(status)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    state.icuData = icuData.Data();
    state.icuDataSize = icuData.Size();
    state.ready = true;
    return Result<void>::Success();
}

bool IsInitialized() noexcept {
    LibraryState& state = State();
    platform::LockGuard lock(state.mutex);
    return state.ready;
}

} // namespace rocks::text::detail

// HarfBuzz 할당 hook이다(ThirdParty.json defines의 hb_*_impl — HarfBuzz를 링크하는 모듈이
// 정의한다).
extern "C" void* RocksHarfBuzzMalloc(size_t size) {
    return rocks::text::detail::AllocateBlock(size);
}

extern "C" void* RocksHarfBuzzCalloc(size_t count, size_t size) {
    if (size != 0 && count > rocks::NumericLimits<size_t>::Max() / size) {
        return nullptr;
    }

    void* block = rocks::text::detail::AllocateBlock(count * size);
    if (block != nullptr) {
        memset(block, 0, count * size);
    }

    return block;
}

extern "C" void* RocksHarfBuzzRealloc(void* memory, size_t size) {
    return rocks::text::detail::ReallocateBlock(memory, size);
}

extern "C" void RocksHarfBuzzFree(void* memory) {
    rocks::text::detail::FreeBlock(memory);
}
