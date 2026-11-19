#include "../Private/Common/ProcessEntryInternal.h"

#include <Base/Compiler.h>
#include <Platform/ProcessEntry.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::platform;

// 진입점 인자 변환과 종료 요청 판정(첫 요청은 처리기, 두 번째부터 OS 기본 동작)을 검사한다. 실제
// 콘솔 Ctrl-C·신호는 시험 실행기를 끝낼 수 있어 보내지 않고 OS 처리기가 부르는 판정 함수를 직접
// 부른다.
namespace {

uint32 handlerCalls = 0;

void CountRequest() noexcept {
    ++handlerCalls;
}

void OtherRequest() noexcept {
}

} // namespace

// 첫 요청만 처리기를 부르고(Handled), 두 번째부터는 OS로 넘긴다. 처리기가 없으면 늘 OS로 넘긴다.
// 처리기 칸은 하나다.
TEST(ProcessEntry_TerminationRequestDispatch) {
    handlerCalls = 0;
    CHECK(detail::DispatchTerminationRequest() == detail::TerminationRequestOutcome::PassToSystem);
    REQUIRE(detail::AcquireTerminationRequestHandler(&CountRequest));
    CHECK(!detail::AcquireTerminationRequestHandler(&OtherRequest));
    CHECK(!detail::AcquireTerminationRequestHandler(nullptr));
    CHECK(detail::DispatchTerminationRequest() == detail::TerminationRequestOutcome::Handled);
    CHECK(handlerCalls == 1);
    CHECK(detail::DispatchTerminationRequest() == detail::TerminationRequestOutcome::PassToSystem);
    CHECK(handlerCalls == 1);
    detail::ReleaseTerminationRequestHandler();
    CHECK(detail::DispatchTerminationRequest() == detail::TerminationRequestOutcome::PassToSystem);
    // 다시 걸면 요청 수가 처음부터다.
    REQUIRE(detail::AcquireTerminationRequestHandler(&CountRequest));
    CHECK(detail::DispatchTerminationRequest() == detail::TerminationRequestOutcome::Handled);
    CHECK(handlerCalls == 2);
    detail::ReleaseTerminationRequestHandler();
}

// OS 처리기 등록·해제는 두 번 걸 수 없고, 뗀 뒤에는 다시 걸 수 있다(요청은 보내지 않는다).
TEST(ProcessEntry_TerminationRequestHandlerInstallsOnce) {
    REQUIRE(InstallTerminationRequestHandler(&CountRequest));
    CHECK(!InstallTerminationRequestHandler(&OtherRequest));
    RemoveTerminationRequestHandler();
    RemoveTerminationRequestHandler();
    REQUIRE(InstallTerminationRequestHandler(&OtherRequest));
    RemoveTerminationRequestHandler();
    CHECK(!InstallTerminationRequestHandler(nullptr));
}

// 이 플랫폼의 진입점 형태만 변환하고, 다른 형태는 Unsupported다. 칸이 모자라면 Overflow와 필요한
// 길이, nullptr은 InvalidArgument다.
TEST(ProcessEntry_ConvertEntryArgument) {
    char buffer[16];
    usize bytes = 0;
    wchar_t wide[] = L"한글";
    const char narrow[] = "\xED\x95\x9C\xEA\xB8\x80";
    const FileStatus native = IsWindows
                                  ? ConvertEntryArgument(wide, buffer, sizeof(buffer), bytes)
                                  : ConvertEntryArgument(narrow, buffer, sizeof(buffer), bytes);
    REQUIRE(native);
    CHECK(bytes == sizeof(narrow) - 1);
    CHECK(memcmp(buffer, narrow, bytes) == 0);
    CHECK(buffer[bytes] == '\0');
    const FileStatus other = IsWindows ? ConvertEntryArgument(narrow, buffer, sizeof(buffer), bytes)
                                       : ConvertEntryArgument(wide, buffer, sizeof(buffer), bytes);
    CHECK(other.code == FileCode::Unsupported);
    char small[4];
    const FileStatus overflow = IsWindows
                                    ? ConvertEntryArgument(wide, small, sizeof(small), bytes)
                                    : ConvertEntryArgument(narrow, small, sizeof(small), bytes);
    CHECK(overflow.code == FileCode::Overflow);
    CHECK(bytes == sizeof(narrow) - 1);
    CHECK(small[0] == '\0');
    const wchar_t* nullWide = nullptr;
    CHECK(ConvertEntryArgument(nullWide, buffer, sizeof(buffer), bytes).code ==
          FileCode::InvalidArgument);
    // UCRT는 넓은 문자 진입점(wmain·wWinMain)일 때 __wargv를 채운다(좁은 문자 main이면 비어 있다).
    // 시험 실행기는 Launch의 wmain이므로 Windows에서는 인자를 얻는다. 다른 플랫폼은 늘 false다.
    int argc = 0;
    wchar_t** argv = nullptr;
    if (IsWindows) {
        CHECK(GetWindowedEntryArguments(argc, argv));
        CHECK(argc >= 1 && argv != nullptr);
    } else {
        CHECK(!GetWindowedEntryArguments(argc, argv));
    }
}
