#include "../Private/Common/PointerLock.h"

#include <TestSupport/Test.h>

// 커서 잠금 판정 시험이다(Docs/Design/UiToolkit 23.5절): 첫 기준은 마지막 위치(잠금 전 움직임을
// 잃지 않음), 되돌린 뒤의 이동량, 되돌림이 만든 이동은 내지 않음, 풀기·커서 옮김 뒤 위치.
using namespace rocks;
using namespace rocks::platform;
namespace pointer_lock_tests_file {

TEST(PlatformWindow_PointerLockFirstDeltaStartsFromLastTranslatedPosition) {
    PointerLock lock;
    lock.NoteMove(100, 50);
    CHECK(!lock.Move(105, 50).push);
    // 잠금 자리는 누른 자리(100, 50)이지만 첫 기준은 마지막으로 바꾼 위치(105, 50)다.
    lock.Lock(100, 50);
    REQUIRE(lock.IsLocked());
    const PointerLockMove first = lock.Move(110, 47);
    CHECK(first.push && first.warp);
    CHECK(first.deltaX == 5 && first.deltaY == -3);
    CHECK(first.x == 100 && first.y == 50);
    // 되돌림이 만든 이동(잠금 자리)은 이동량 0이라 내지 않는다.
    const PointerLockMove synthetic = lock.Move(100, 50);
    CHECK(!synthetic.push && !synthetic.warp);
    // 다음 이동은 잠금 자리에서 잰다.
    const PointerLockMove second = lock.Move(98, 60);
    CHECK(second.push && second.deltaX == -2 && second.deltaY == 10);
}

TEST(PlatformWindow_PointerLockUnlockAndWarpUpdateTheLastPosition) {
    PointerLock lock;
    lock.NoteMove(10, 10);
    lock.Lock(20, 20);
    (void)lock.Move(30, 30);
    lock.Unlock();
    CHECK(!lock.IsLocked());
    // 다시 잠그면 첫 기준은 풀 때의 잠금 자리다(커서가 거기 있다).
    lock.Lock(20, 20);
    CHECK(lock.Move(21, 20).deltaX == 1);
    // 잠근 동안 커서를 옮기면 기준도 그 자리다.
    lock.NoteWarp(40, 40);
    const PointerLockMove moved = lock.Move(41, 42);
    CHECK(moved.deltaX == 1 && moved.deltaY == 2);
    lock.Unlock();
    lock.Unlock();
    CHECK(!lock.IsLocked());
}

TEST(PlatformWindow_PointerLockRelockKeepsTheReference) {
    PointerLock lock;
    lock.NoteMove(0, 0);
    lock.Lock(5, 5);
    // 이미 잠갔으면 잠금 자리만 바뀐다 — 기준(0, 0)은 그대로다.
    lock.Lock(7, 7);
    const PointerLockMove move = lock.Move(3, 4);
    CHECK(move.deltaX == 3 && move.deltaY == 4 && move.x == 7 && move.y == 7);
}

} // namespace pointer_lock_tests_file
