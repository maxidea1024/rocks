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
