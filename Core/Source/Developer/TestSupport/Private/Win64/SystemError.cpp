#include <TestSupport/SystemError.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::test {

void SetSystemError(uint32 value) noexcept {
    SetLastError(value);
}

uint32 SystemError() noexcept {
    return GetLastError();
}

} // namespace rocks::test
