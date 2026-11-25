#include "../Private/Common/PointerTouchMapping.h"

#include <TestSupport/Test.h>

using namespace rocks;
namespace pointer_touch_mapping_tests_file {

namespace pointer = platform::pointer;
using pointer::PointerTouchAction;

// WM_POINTER 메시지·플래그 → 터치 동작(Docs/Design/TouchInput.ko.md 4.1절).
TEST(PointerTouchMapping_MessagesAndCancelFlag) {
    CHECK(pointer::TouchActionOf(pointer::PointerDownMessage, 0) == PointerTouchAction::Began);
    CHECK(pointer::TouchActionOf(pointer::PointerUpdateMessage, 0) == PointerTouchAction::Moved);
    CHECK(pointer::TouchActionOf(pointer::PointerUpMessage, 0) == PointerTouchAction::Ended);
    CHECK(pointer::TouchActionOf(pointer::PointerUpMessage, pointer::PointerFlagCanceled) ==
          PointerTouchAction::Cancelled);
    // 다른 플래그(INCONTACT 0x4 등)는 끝의 종류를 바꾸지 않는다.
    CHECK(pointer::TouchActionOf(pointer::PointerUpMessage, 0x4) == PointerTouchAction::Ended);
    CHECK(pointer::TouchActionOf(pointer::PointerCaptureChangedMessage, 0) ==
          PointerTouchAction::Cancelled);
    CHECK(pointer::TouchActionOf(0x0200, 0) == PointerTouchAction::None);
}

// 터치·펜만 터치로 보내고, 마우스(PT_MOUSE 4)·기본(PT_POINTER 1)·터치패드(PT_TOUCHPAD 5)는 아니다.
TEST(PointerTouchMapping_PointerTypesAndPressure) {
    CHECK(pointer::IsTouchPointerType(pointer::PointerTypeTouch));
    CHECK(pointer::IsTouchPointerType(pointer::PointerTypePen));
    CHECK(!pointer::IsTouchPointerType(1) && !pointer::IsTouchPointerType(4) &&
          !pointer::IsTouchPointerType(5));
    CHECK(pointer::NormalizePointerPressure(0) == 0.0f);
    CHECK(pointer::NormalizePointerPressure(512) == 0.5f);
    CHECK(pointer::NormalizePointerPressure(1024) == 1.0f);
    CHECK(pointer::NormalizePointerPressure(5000) == 1.0f);
}

} // namespace pointer_touch_mapping_tests_file
