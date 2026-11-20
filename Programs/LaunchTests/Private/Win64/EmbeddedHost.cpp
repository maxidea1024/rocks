#include "LaunchProbe.h"

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// Rocks를 내장한 호스트 프로세스의 시험판이다(설계 8.8절). 호스트가 자기 진입점(wmain)과 처리되지
// 않은 예외 filter를 갖고, Rocks는 내장 진입(RunEmbeddedApplication)으로 부른다. Rocks는 호스트의
// filter를 바꾸지 않고, 크래시를 보고(종료 콜백·CrashInfo.json)한 뒤 호스트 filter로 넘긴다. 호스트
// filter는 예외 코드를 한 줄 쓰고 HostHandledExitCode로 끝낸다(프로세스 종료는 호스트가 정한다).
namespace {

using rocks::uint32;
using rocks::usize;

constexpr UINT HostHandledExitCode = 77;
constexpr usize HexDigitsPerCode = 8;
constexpr uint32 BitsPerHexDigit = 4;
constexpr char HexDigits[] = "0123456789ABCDEF";

LONG WINAPI HostFilter(EXCEPTION_POINTERS* pointers) noexcept {
    char line[] = "LAUNCH HOST FILTER 0x00000000";
    const DWORD code = pointers->ExceptionRecord->ExceptionCode;
    const usize start = sizeof(line) - 1 - HexDigitsPerCode;
    for (usize index = 0; index < HexDigitsPerCode; ++index) {
        const uint32 shift = static_cast<uint32>(HexDigitsPerCode - 1 - index) * BitsPerHexDigit;
        line[start + index] = HexDigits[(code >> shift) & 0x0F];
    }

    rocks::launch::probe::WriteProbeLine(line);
    TerminateProcess(GetCurrentProcess(), HostHandledExitCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description =
        rocks::launch::probe::MakeDescription("LaunchEmbeddedHost",
                                              rocks::launch::CrashReportingMode::Full);
    return Description;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // 호스트 자신의 정책이다(대화상자 없이 filter로 받는다).
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    (void)SetUnhandledExceptionFilter(&HostFilter);
    const int exitCode = rocks::launch::RunEmbeddedApplication(Describe(), argc, argv);
    // Rocks가 호스트 filter를 바꾸지 않았는지 교환으로 확인하고 되돌린다.
    const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(&HostFilter);
    rocks::launch::probe::WriteProbeLine(current == &HostFilter ? "LAUNCH HOST FILTER INTACT"
                                                                : "LAUNCH HOST FILTER REPLACED");
    return exitCode;
}
