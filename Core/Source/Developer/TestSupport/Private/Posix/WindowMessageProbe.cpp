#include <TestSupport/WindowMessageProbe.h>

namespace rocks::test {

// 스레드에 묶인 창 메시지 큐가 없다(Android는 입력 스레드가 넘긴다 — 시험은 건너뛴다).
bool SupportsWindowMessages() noexcept {
    return false;
}

bool PostWindowDoubleClick(void*, int32, int32) noexcept {
    return false;
}

bool HasThreadWakeMessage() noexcept {
    return false;
}

bool SendWindowDpiChanged(void*, uint32, int32, int32) noexcept {
    return false;
}

} // namespace rocks::test
