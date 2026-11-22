#include <TestSupport/Environment.h>

#include <Platform/Win64/WideString.h>
#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

#include <string.h>

namespace rocks::test {

namespace {

// 환경 변수 값의 UTF-16 단위 상한(끝 NUL 포함)이다(Windows 환경 블록 한 항목의 최대 길이).
constexpr DWORD EnvironmentValueMaxUnits = 32767;

} // namespace

bool ReadEnvironmentVariable(const char* name, StringBuilderBase& out) noexcept {
    platform::windows::WideString wideName;
    if (!wideName.AssignUtf8(name, strlen(name))) {
        return false;
    }

    // 빈 값은 1(끝 NUL), 없는 변수는 0이다.
    wchar_t value[EnvironmentValueMaxUnits];
    SetLastError(ERROR_SUCCESS);
    const DWORD units = GetEnvironmentVariableW(wideName.Value(), value, EnvironmentValueMaxUnits);
    if (units == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        return false;
    }

    if (units >= EnvironmentValueMaxUnits) {
        return false;
    }

    platform::windows::Utf8String text;
    if (units != 0 && !text.AssignWide(value, units)) {
        return false;
    }

    return units == 0 || static_cast<bool>(out.TryAppend(StringView(text.Value(), text.Bytes())));
}

} // namespace rocks::test
