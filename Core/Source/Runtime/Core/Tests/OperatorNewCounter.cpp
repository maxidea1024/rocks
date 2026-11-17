#include <Base/Types.h>
#include <stdlib.h>

// 이 시험 바이너리의 전역 operator new·delete 대체다. 호출 스레드별로 operator new 호출 수를 세어
// 무할당 시험(로그 캡처·렌더·LogSystem)이 C++ 힙 할당이 없었음을 확인한다. 할당은 그대로
// malloc·free로 넘긴다. 시험 바이너리 전용이며 엔진 코드에는 두지 않는다.
namespace rocks::test {
namespace {

thread_local uint64 operatorNewCalls = 0;

void* CountedAllocate(decltype(sizeof(0)) bytes) noexcept {
    ++operatorNewCalls;
    void* memory = malloc(bytes == 0 ? 1 : bytes);
    if (memory == nullptr) {
        abort();
    }

    return memory;
}

} // namespace

uint64 OperatorNewCallsOnThisThread() noexcept {
    return operatorNewCalls;
}

} // namespace rocks::test

void* operator new(decltype(sizeof(0)) bytes) {
    return rocks::test::CountedAllocate(bytes);
}

void* operator new[](decltype(sizeof(0)) bytes) {
    return rocks::test::CountedAllocate(bytes);
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
