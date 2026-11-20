#include <TestSupport/SystemError.h>

#include <Windows.h>

namespace rocks::test {

void SetSystemError(uint32 value) noexcept {
    SetLastError(value);
}

uint32 SystemError() noexcept {
    return GetLastError();
}

} // namespace rocks::test
