#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"

#include <AssetCooker/CookRootLock.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 쿠킹 루트 잠금 시험이다(Docs/Design/AssetSystem 10.3.1절 A4f4 1번): 잡기·잠금 파일 위치
// (`<Saved>/Locks/Cooked-<Profile>.lock`)·같은 루트 두 번째 Busy·같은 객체 AlreadyExists·다른
// 프로필은 독립·IsHeldFor 대조(철자·프로필·놓은 뒤)·놓은 뒤 다시 잡기·빈 인자·경로 조립 할당 실패.
using namespace rocks;

namespace cook_root_lock_tests_file {

using namespace rocks::asset_database_test;

TEST(CookRootLock_AcquireAndRelease) {
    SystemAllocator allocator;
    TempProject root("cook-root-lock");
    REQUIRE(root.made);
    StringBuilder<128> saved(allocator);
    REQUIRE(saved.TryAppendFormat("{}/Saved", root.Root()));
    {
        CookRootLock first;
        CHECK(!first.IsHeld() && !first.IsHeldFor(saved.View(), "Windows"));
        REQUIRE(first.Acquire(allocator, saved.View(), "Windows", 0));
        CHECK(first.IsHeld() && first.IsHeldFor(saved.View(), "Windows"));
        CHECK(!first.IsHeldFor(saved.View(), "Android") &&
              !first.IsHeldFor(root.Root(), "Windows") &&
              !first.IsHeldFor(StringView(), "Windows") && !first.IsHeldFor(saved.View(), ""));
        StringBuilder<160> file(allocator);
        REQUIRE(file.TryAppendFormat("{}/Locks/Cooked-Windows.lock", saved.View()));
        bool exists = false;
        REQUIRE(platform::PhysicalFileSystem().FileExists(file.View().Data(),
                                                          file.View().ByteCount(), exists));
        CHECK(exists);
        CHECK(first.Acquire(allocator, saved.View(), "Windows", 0).Error() ==
              ErrorCode::AlreadyExists);

        // 같은 쿠킹 루트의 두 번째 잠금은 Busy, 다른 프로필은 독립이다.
        CookRootLock second;
        CHECK(second.Acquire(allocator, saved.View(), "Windows", 0).Error() == ErrorCode::Busy);
        CHECK(!second.IsHeld());
        CookRootLock android;
        REQUIRE(android.Acquire(allocator, saved.View(), "Android", 0));
        CHECK(android.IsHeldFor(saved.View(), "Android") &&
              !android.IsHeldFor(saved.View(), "Windows"));

        first.Release();
        CHECK(!first.IsHeld() && !first.IsHeldFor(saved.View(), "Windows"));
        REQUIRE(second.Acquire(allocator, saved.View(), "Windows", 0));
        CHECK(second.IsHeldFor(saved.View(), "Windows"));
    }

    CookRootLock empty;
    CHECK(empty.Acquire(allocator, StringView(), "Windows", 0).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(empty.Acquire(allocator, saved.View(), StringView(), 0).Error() ==
          ErrorCode::InvalidArgument);

    // 경로 조립의 할당 실패(내장 버퍼를 넘는 긴 프로필)는 그 오류이고 잠금은 없다.
    test::NFailAllocator failing(allocator);
    String longProfile(allocator);
    // 파일 이름은 255 byte 안(Cooked- + 200 + .lock)이고 경로는 내장 256 byte를 넘는다.
    for (usize index = 0; index < 200; ++index) {
        REQUIRE(longProfile.TryAppend("P"));
    }

    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        CookRootLock lock;
        failing.FailOnAttempt(attempt);
        auto acquired = lock.Acquire(failing, saved.View(), longProfile.View(), 0);
        const bool hit = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!hit) {
            REQUIRE(acquired);
            break;
        }

        CHECK(acquired.Error() == ErrorCode::OutOfMemory && !lock.IsHeld());
        ++failures;
    }

    CHECK(failures > 0);
}

} // namespace cook_root_lock_tests_file
