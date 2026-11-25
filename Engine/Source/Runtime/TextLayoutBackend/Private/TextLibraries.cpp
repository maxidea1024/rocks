// 글꼴 라이브러리의 프로세스 전역 상태다(Docs/Design/Font 6.8·8.2절): ICU 할당 hook과 데이터 등록,
// HarfBuzz 할당 hook(hb_*_impl = RocksHarfBuzz*). 둘 다 Core LibraryHeap(프로세스 수명 Rocks
// 할당기, 크기 머리를 둔 malloc 호환 블록)을 쓴다. u_cleanup은 부르지 않는다.
#include "TextLibraries.h"

#include <Core/Memory/LibraryHeap.h>
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

void* U_CALLCONV IcuAlloc(const void*, size_t size) {
    return LibraryHeapAllocate(size);
}

void* U_CALLCONV IcuRealloc(const void*, void* memory, size_t size) {
    return LibraryHeapReallocate(memory, size);
}

void U_CALLCONV IcuFree(const void*, void* memory) {
    LibraryHeapFree(memory);
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
    return rocks::LibraryHeapAllocate(size);
}

extern "C" void* RocksHarfBuzzCalloc(size_t count, size_t size) {
    return rocks::LibraryHeapAllocateZeroed(count, size);
}

extern "C" void* RocksHarfBuzzRealloc(void* memory, size_t size) {
    return rocks::LibraryHeapReallocate(memory, size);
}

extern "C" void RocksHarfBuzzFree(void* memory) {
    rocks::LibraryHeapFree(memory);
}
