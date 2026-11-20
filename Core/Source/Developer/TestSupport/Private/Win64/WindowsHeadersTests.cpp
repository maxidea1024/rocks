#include <TestSupport/Test.h>

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Unknwn.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// 두 번째 구간에서 처음 여는 COM·DXGI 헤더다. 앞 구간 끝에서 지운 interface·FAR 등을 Begin이 다시
// 정의해야 파싱된다.
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

// 숨긴 매크로 이름을 식별자로 쓴다. 매크로가 남아 있으면 컴파일되지 않는다(near·far·cdecl은 빈
// 토큰, small은 char, hyper는 __int64, pascal은 __stdcall, interface는 struct가 된다). min·max는
// 빌드가 NOMINMAX를 정의해 처음부터 매크로가 아니므로 여기서 볼 것이 없다.
struct Names {
    int near;
    int far;
    int small;
    int interface;
    int pascal;
    int cdecl;
    int hyper;
};

// A/W 함수 이름 매크로도 지워져 있다. 같은 이름의 함수를 선언하는 것으로는 알 수 없으므로(매크로가
// 남아 있어도 GetMessageA라는 이름으로 컴파일된다) 전처리 조건으로 직접 본다.
#if defined(GetMessage) || defined(CreateDirectory) || defined(RegisterClass) ||                   \
    defined(FindFirstFile)
#error "A/W function name macros must be undefined after WindowsHeadersEnd.h"
#endif

// 감싼 구간의 COM 메서드는 제 이름으로 선언된다. 구간을 파싱할 때 A/W 매크로가 살아 있었다면
// IDXGIInfoQueue의 메서드는 GetMessageA가 되고, 매크로가 지워진 여기서는 GetMessage로 찾지 못한다.
static_assert(
    requires { &IDXGIInfoQueue::GetMessage; },
    "COM methods in a wrapped section keep their own names");

} // namespace

TEST(PlatformWindowsHeadersHideLeakedMacros) {
    const Names names{1, 2, 3, 4, 5, 6, 7};
    CHECK(names.near == 1 && names.far == 2 && names.small == 3 && names.interface == 4);
    CHECK(names.pascal == 5 && names.cdecl == 6 && names.hyper == 7);
    // W 이름은 그대로 쓴다.
    CHECK(GetCurrentProcessId() != 0);
}

} // namespace windows_headers_tests_file
