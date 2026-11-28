#include "../Private/Common/ModifierLatch.h"

#include <TestSupport/Test.h>

// 놓친 modifier 뗌 판정 시험이다(Docs/Design/UiToolkit 23.5a절): 누름 적기, 물리 뗌이면 KeyUp 목록,
// 큐에 키 메시지가 있으면 보류, 보정 뒤 늦은 뗌 버림, modifier 아닌 키는 통과, 비우기.
using namespace rocks;
using namespace rocks::platform;
namespace modifier_latch_tests_file {

using input::Key;

TEST(PlatformWindow_ModifierLatchReleasesModifiersTheSystemNoLongerHolds) {
    // Win+. 이모지 패널: Win 누름은 창에 오고 뗌은 오지 않는다.
    ModifierLatch latch;
    latch.NoteDown(Key::LeftSuper);
    latch.NoteDown(Key::LeftSuper);
    latch.NoteDown(Key::LeftShift);
    CHECK(latch.Held() ==
          (ModifierLatch::Bit(Key::LeftSuper) | ModifierLatch::Bit(Key::LeftShift)));
    Key released[ModifierLatch::Capacity] = {};
    // Shift는 아직 눌려 있고 Win은 떼어졌다.
    const uint32 count = latch.Reconcile(false, ModifierLatch::Bit(Key::LeftShift), released);
    REQUIRE(count == 1);
    CHECK(released[0] == Key::LeftSuper);
    CHECK(latch.Held() == ModifierLatch::Bit(Key::LeftShift));
    // 다시 보정해도 같은 뗌을 두 번 내지 않는다.
    CHECK(latch.Reconcile(false, ModifierLatch::Bit(Key::LeftShift), released) == 0);
}

TEST(PlatformWindow_ModifierLatchWaitsForQueuedKeyMessages) {
    // 빨리 뗀 Control의 진짜 뗌이 아직 큐에 있다 — 앞 키의 chord가 바뀌지 않게 보정하지 않는다.
    ModifierLatch latch;
    latch.NoteDown(Key::LeftControl);
    latch.NoteDown(Key::RightControl);
    Key released[ModifierLatch::Capacity] = {};
    CHECK(latch.Reconcile(true, 0, released) == 0);
    CHECK(latch.Held() ==
          (ModifierLatch::Bit(Key::LeftControl) | ModifierLatch::Bit(Key::RightControl)));
    // 큐가 비었으면 좌우를 따로, 비트 순서로 낸다.
    REQUIRE(latch.Reconcile(false, 0, released) == 2);
    CHECK(released[0] == Key::LeftControl && released[1] == Key::RightControl);
    CHECK(latch.Held() == 0);
}

TEST(PlatformWindow_ModifierLatchDropsLateAndUnreportedReleases) {
    ModifierLatch latch;
    latch.NoteDown(Key::LeftAlt);
    Key released[ModifierLatch::Capacity] = {};
    REQUIRE(latch.Reconcile(false, 0, released) == 1);
    // 보정 뒤 늦게 온 진짜 뗌은 버린다.
    CHECK(!latch.NoteUp(Key::LeftAlt));
    // 창 밖에서 누른 modifier의 뗌도 누름을 보고하지 않았으므로 버린다.
    CHECK(!latch.NoteUp(Key::RightShift));
    // 적힌 modifier의 뗌은 보내고 지운다.
    latch.NoteDown(Key::RightSuper);
    CHECK(latch.NoteUp(Key::RightSuper));
    CHECK(latch.Held() == 0);
    // modifier 아닌 키는 늘 보낸다.
    latch.NoteDown(Key::Backspace);
    CHECK(latch.Held() == 0);
    CHECK(latch.NoteUp(Key::Backspace));
}

TEST(PlatformWindow_ModifierLatchClearAndBitTable) {
    ModifierLatch latch;
    latch.NoteDown(Key::LeftShift);
    latch.NoteDown(Key::RightAlt);
    latch.Clear();
    CHECK(latch.Held() == 0);
    // 비트 표는 좌우 여덟 modifier만이고 왕복한다.
    const Key modifiers[ModifierLatch::Capacity] = {
        Key::LeftShift, Key::RightShift, Key::LeftControl, Key::RightControl,
        Key::LeftAlt,   Key::RightAlt,   Key::LeftSuper,   Key::RightSuper};
    uint8 all = 0;
    for (const Key key : modifiers) {
        const uint8 bit = ModifierLatch::Bit(key);
        CHECK(bit != 0 && (all & bit) == 0);
        CHECK(ModifierLatch::KeyOf(bit) == key);
        all = static_cast<uint8>(all | bit);
    }

    CHECK(all == 0xff);
    CHECK(ModifierLatch::Bit(Key::A) == 0);
    CHECK(ModifierLatch::KeyOf(0) == Key::None);
    CHECK(ModifierLatch::KeyOf(3) == Key::None);
}

} // namespace modifier_latch_tests_file
