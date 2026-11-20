#include <TestSupport/Test.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Unknwn.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// 두 번째 구간에서 처음 여는 COM·DXGI 헤더다. 앞 구간이 숨긴 interface·FAR 등이 되돌려져 있어야
// 파싱된다.
#include <Platform/Win64/WindowsHeadersBegin.h>

#include <dxgidebug.h>
#include <objidl.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// Platform/Win64/WindowsHeadersBegin.h·WindowsHeadersEnd.h 시험이다(Docs/Design/UnityBuildPch.ko.md
// 6.1절 "매크로를 남기는 헤더"). 대부분 컴파일되는 것 자체가 단언이다. Windows 전용 시험이라
// TestSupport/Private/Win64에 둔다.
namespace windows_headers_tests_file {

// SDK 매크로가 쓰는 자리에서 FAR로 펼쳐진다(INITGUID가 없으면 extern 선언이다). far를 숨긴 뒤에도
// 컴파일되어야 한다.
DEFINE_GUID(ProbeGuid, 0x6b0c2a51, 0x8a1e, 0x4c38, 0x9a, 0x62, 0x3f, 0x1d, 0x2a, 0x7e, 0x0b, 0x12);

namespace {

// 숨긴 매크로 이름을 식별자로 쓴다. 매크로가 남아 있으면 컴파일되지 않는다(near·far는 빈 토큰,
// small은 char, interface는 struct가 된다).
struct Names {
    int near;
    int far;
    int small;
    int interface;
    int min;
    int max;
};

// A/W 함수 이름 매크로도 숨겨져 있어 같은 이름의 함수를 선언할 수 있다.
int GetMessage(int value) noexcept {
    return value + 1;
}

int CreateDirectory(int value) noexcept {
    return value + 2;
}

// 감싼 구간의 COM 메서드는 제 이름으로 선언된다. 구간을 파싱할 때 A/W 매크로가 살아 있었다면
// IDXGIInfoQueue의 메서드는 GetMessageA가 된다.
static_assert(
    requires { &IDXGIInfoQueue::GetMessage; },
    "COM methods in a wrapped section keep their own names");

} // namespace

TEST(PlatformWindowsHeadersHideLeakedMacros) {
    const Names names{1, 2, 3, 4, 5, 6};
    CHECK(names.near == 1 && names.far == 2 && names.small == 3);
    CHECK(names.interface == 4 && names.min == 5 && names.max == 6);
    CHECK(GetMessage(1) == 2 && CreateDirectory(1) == 3);
    // W 이름은 그대로 쓴다.
    CHECK(GetCurrentProcessId() != 0);
}

} // namespace windows_headers_tests_file
