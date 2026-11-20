#include <TestSupport/SystemError.h>

#include <errno.h>

namespace rocks::test {

void SetSystemError(uint32 value) noexcept {
    errno = static_cast<int>(value);
}

uint32 SystemError() noexcept {
    return static_cast<uint32>(errno);
}

} // namespace rocks::test
