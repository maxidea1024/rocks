#include <Base/Types.h>
#include <Platform/Atomics.h>
#include <new>
#include <stdlib.h>

// 이 시험 바이너리의 전역 operator new·delete 대체다. 프로세스 전체의 operator new 호출 수를 원자
// 카운터로 세어 무할당 시험(로그 캡처·렌더·LogSystem)이 C++ 힙 할당이 없었음을 확인한다. 전역이라
// 출력 스레드(Async) 같은 다른 스레드의 할당도 보인다. 정렬·nothrow 판도 모두 대체해 세지 않고
// 지나가는 할당이 없게 한다. 할당은 malloc·free로 넘긴다(정렬 판은 앞에 원래 주소를 적어 둔다).
// 시험 바이너리 전용이며 엔진 코드에는 두지 않는다.
namespace rocks::test {
namespace {

constinit uint64 operatorNewCalls = 0;

void* CountedAllocate(decltype(sizeof(0)) bytes) noexcept {
    (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&operatorNewCalls, uint64{1});
    return malloc(bytes == 0 ? 1 : bytes);
}

// alignment 경계로 맞춘 주소 바로 앞 칸에 malloc이 준 주소를 적는다.
void* CountedAllocateAligned(decltype(sizeof(0)) bytes, std::align_val_t alignment) noexcept {
    (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&operatorNewCalls, uint64{1});
    const auto align = static_cast<decltype(sizeof(0))>(alignment);
    const decltype(sizeof(0)) header = sizeof(void*);
    void* raw = malloc((bytes == 0 ? 1 : bytes) + align + header);
    if (raw == nullptr) {
        return nullptr;
    }

    const auto start = reinterpret_cast<decltype(sizeof(0))>(raw) + header;
    const auto aligned = (start + align - 1) / align * align;
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void FreeAligned(void* memory) noexcept {
    if (memory != nullptr) {
        free(static_cast<void**>(memory)[-1]);
    }
}

void* OrAbort(void* memory) noexcept {
    if (memory == nullptr) {
        abort();
    }

    return memory;
}

} // namespace

uint64 OperatorNewCalls() noexcept {
    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&operatorNewCalls);
}

} // namespace rocks::test

void* operator new(decltype(sizeof(0)) bytes) {
    return rocks::test::OrAbort(rocks::test::CountedAllocate(bytes));
}

void* operator new[](decltype(sizeof(0)) bytes) {
    return rocks::test::OrAbort(rocks::test::CountedAllocate(bytes));
}

void* operator new(decltype(sizeof(0)) bytes, const std::nothrow_t&) noexcept {
    return rocks::test::CountedAllocate(bytes);
}

void* operator new[](decltype(sizeof(0)) bytes, const std::nothrow_t&) noexcept {
    return rocks::test::CountedAllocate(bytes);
}

void* operator new(decltype(sizeof(0)) bytes, std::align_val_t alignment) {
    return rocks::test::OrAbort(rocks::test::CountedAllocateAligned(bytes, alignment));
}

void* operator new[](decltype(sizeof(0)) bytes, std::align_val_t alignment) {
    return rocks::test::OrAbort(rocks::test::CountedAllocateAligned(bytes, alignment));
}

void* operator new(decltype(sizeof(0)) bytes, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    return rocks::test::CountedAllocateAligned(bytes, alignment);
}

void* operator new[](decltype(sizeof(0)) bytes, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    return rocks::test::CountedAllocateAligned(bytes, alignment);
}

void operator delete(void* memory) noexcept {
    free(memory);
}

void operator delete[](void* memory) noexcept {
    free(memory);
}

void operator delete(void* memory, decltype(sizeof(0))) noexcept {
    free(memory);
}

void operator delete[](void* memory, decltype(sizeof(0))) noexcept {
    free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    rocks::test::FreeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    rocks::test::FreeAligned(memory);
}

void operator delete(void* memory, decltype(sizeof(0)), std::align_val_t) noexcept {
    rocks::test::FreeAligned(memory);
}

void operator delete[](void* memory, decltype(sizeof(0)), std::align_val_t) noexcept {
    rocks::test::FreeAligned(memory);
}

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    rocks::test::FreeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    rocks::test::FreeAligned(memory);
}
