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
