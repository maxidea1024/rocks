#include <stdlib.h>

namespace rocks::test {

int RunAsanNegativeControl() noexcept {
    auto* memory = static_cast<unsigned char*>(malloc(32));
    if (memory == nullptr) {
        return 2;
    }
    memory[0] = 17;
    volatile unsigned char* dangling = memory;
    free(memory);
    // CMake가 ASan 구성에서만 선택한다. 일반 시험에서 실행하지 않는 자식 프로세스 반례다.
    const volatile unsigned char read = *dangling;
    (void)read;
    return 0;
}

} // namespace rocks::test
