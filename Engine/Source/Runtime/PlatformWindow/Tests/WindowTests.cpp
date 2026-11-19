#include <Core/Memory/SystemAllocator.h>
#include <PlatformWindow/Window.h>
#include <TestSupport/Test.h>

using namespace rocks;

// 창을 실제로 띄우는 시험은 데스크톱 세션이 필요하므로 Sandbox --self-test에서 한다. 여기서는 창을
// 만들지 않는 입력 검증만 확인한다.
TEST(PlatformWindow_ZeroSizeIsRejectedBeforeCreatingWindow) {
    SystemAllocator allocator;
    platform::WindowDesc desc;
    desc.width = 0;
    CHECK(platform::Window::Create(allocator, desc).Error() == ErrorCode::InvalidArgument);
    desc.width = 640;
    desc.height = 0;
    CHECK(platform::Window::Create(allocator, desc).Error() == ErrorCode::InvalidArgument);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 잘못된 UTF-8 제목(잘린 sequence·overlong)은 창을 만들기 전에 InvalidArgument다. SetTitle의 같은
// 규칙은 실제 창이 필요해 여기서는 시험하지 않는다(변환 자체는 WindowsWideStringTests가 본다).
TEST(PlatformWindow_InvalidUtf8TitleIsRejectedBeforeCreatingWindow) {
    SystemAllocator allocator;
    const char* invalid[] = {"\xEA\xB0", "\xC0\xAF", "ok\xFF"};
    for (const char* title : invalid) {
        platform::WindowDesc desc;
        desc.title = StringView(title);
        platform::WindowCreateReport report;
        CHECK(platform::Window::Create(allocator, desc, &report).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(report.code == ErrorCode::InvalidArgument && report.call[0] == '\0');
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// report는 입력 오류에도 결과 코드를 채우고, OS 호출 전이라 호출 이름·OS 오류는 비어 있다.
TEST(PlatformWindow_CreateReportCarriesInputFailure) {
    SystemAllocator allocator;
    platform::WindowDesc desc;
    desc.height = 0;
    platform::WindowCreateReport report;
    report.osError = 5;
    report.call[0] = 'x';
    CHECK(platform::Window::Create(allocator, desc, &report).Error() == ErrorCode::InvalidArgument);
    CHECK(report.code == ErrorCode::InvalidArgument && report.osError == 0 &&
          report.call[0] == '\0' && !report.truncated);
}

// 세션 상태 조회는 할당 없이 세 값 가운데 하나를 돌려준다. 시험 기계의 세션 상태(콘솔·원격·끊김)는
// 정할 수 없으므로 값 자체는 단언하지 않는다.
TEST(PlatformWindow_DesktopSessionStateIsOneOfTheKnownValues) {
    const platform::DesktopSessionState state = platform::QueryDesktopSessionState();
    CHECK(state == platform::DesktopSessionState::Connected ||
          state == platform::DesktopSessionState::Disconnected ||
          state == platform::DesktopSessionState::Unknown);
}
