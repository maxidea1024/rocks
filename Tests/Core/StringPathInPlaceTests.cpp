#include <Core/Memory/StackAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

TEST(StringPathNoOpPreservesAddressCapacityAndAvoidsAllocation) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        REQUIRE(text.TryReserve(64));
        REQUIRE(text.TryAssign("root/"));
        const char* address = text.Data();
        const usize capacity = text.ByteCapacity();
        const auto before = system.Stats().liveAllocations;
        failing.FailOnAttempt(1);
        auto result = (text /= "");
        REQUIRE(result);
        CHECK(failing.Attempts() == 0);
        CHECK(text.Data() == address && text.ByteCapacity() == capacity);
        CHECK(text.View() == StringView("root/"));
        CHECK(system.Stats().liveAllocations == before);

        failing.DisableFailure();
        text.Clear();
        failing.FailOnAttempt(1);
        auto empty = (text /= "");
        REQUIRE(empty);
        CHECK(failing.Attempts() == 0);
        CHECK(text.IsEmpty() && text.Data() == address && text.ByteCapacity() == capacity);
        CHECK(system.Stats().liveAllocations == before);

        // 아무 변경도 없더라도 경로의 embedded NUL을 검사한다.
        constexpr char invalid[] = {'a', '\0', '/'};
        failing.DisableFailure();
        REQUIRE(text.TryAssign(StringView(invalid, sizeof(invalid))));
        auto rejected = (text /= "");
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
        CHECK(text.Data() == address && text.View() == StringView(invalid, sizeof(invalid)));
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(StringPathAppendWithStackBackingUsesReservedCapacity) {
    SystemAllocator system;
    {
        StackAllocator stack(system);
        {
            String text(stack);
            REQUIRE(text.TryReserve(128));
            REQUIRE(text.TryAssign("root"));
            const char* address = text.Data();
            const usize capacity = text.ByteCapacity();
            const auto before = stack.Stats().allocations.totalAllocations;
            auto append = (text /= "child");
            REQUIRE(append);
            CHECK(text.View() == StringView("root/child"));
            CHECK(text.Data() == address && text.ByteCapacity() == capacity);
            CHECK(stack.Stats().allocations.totalAllocations == before);

            auto self = (text /= text.View());
            REQUIRE(self);
            CHECK(text.View() == StringView("root/child/root/child"));
            CHECK(text.Data() == address && text.ByteCapacity() == capacity);
            CHECK(stack.Stats().allocations.totalAllocations == before);
        }
        CHECK(stack.Stats().allocations.liveAllocations == 0);
        REQUIRE(stack.Trim());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(StringPathInPlaceAppendRetainsSuffixBehindOldTerminator) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        REQUIRE(text.TryReserve(64));
        REQUIRE(text.TryAssign("abcdXYZ"));
        REQUIRE(text.TryAssign("abc"));
        const char* address = text.Data();
        const StringView suffix(address + 4, 3);
        REQUIRE(suffix == StringView("XYZ"));
        const auto before = system.Stats().liveAllocations;

        // 종단 NUL 뒤의 자기 buffer를 빌려도 여유 용량이 있으면 할당 없이 처리한다.
        failing.FailOnAttempt(1);
        auto appended = (text /= suffix);
        REQUIRE(appended);
        CHECK(failing.Attempts() == 0);
        CHECK(text.Data() == address);
        CHECK(text.View() == StringView("abc/XYZ"));
        CHECK(system.Stats().liveAllocations == before);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(StringPathGrowthFailurePreservesSelfAliasedInput) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        REQUIRE(text.TryAssign("root"));
        const char* address = text.Data();
        const usize capacity = text.ByteCapacity();
        const auto before = system.Stats().liveAllocations;
        const StringView source = text.View();
        failing.FailOnAttempt(1);
        auto failed = (text /= source);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(text.Data() == address && text.ByteCapacity() == capacity);
        CHECK(text.View() == StringView("root") && source == StringView("root"));
        CHECK(system.Stats().liveAllocations == before);
    }
    CHECK(system.Stats().liveAllocations == 0);
}
