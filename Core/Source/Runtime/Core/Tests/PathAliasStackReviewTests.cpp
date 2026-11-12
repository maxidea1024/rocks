#include <Core/Memory/StackAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

TEST(StringPathInPlaceKeepsOverlappingTailWithoutAllocation) {
    SystemAllocator system;
    {
        StackAllocator stack(system);
        {
            String text(stack);
            REQUIRE(text.TryReserve(64));
            REQUIRE(text.TryAssign("abcdXYZ"));
            REQUIRE(text.TryAssign("abc"));
            const char* address = text.Data();
            const StringView suffix(address + 4, 3);
            const auto allocations = stack.Stats().allocations.totalAllocations;
            REQUIRE(suffix == StringView("XYZ"));
            auto result = (text /= suffix);
            REQUIRE(result);
            CHECK(text.View() == StringView("abc/XYZ"));
            CHECK(text.Data() == address);
            CHECK(stack.Stats().allocations.totalAllocations == allocations);
        }
        CHECK(stack.Stats().allocations.liveAllocations == 0);
        REQUIRE(stack.Trim());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(StringPathInPlaceMovesOverlappingTailBeforeTerminator) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        REQUIRE(text.TryReserve(64));
        REQUIRE(text.TryAssign("abc_한글ZZ"));
        REQUIRE(text.TryAssign("abc"));
        const char* address = text.Data();
        const StringView suffix(address + 4, 8);
        REQUIRE(suffix == StringView("한글ZZ"));
        failing.FailOnAttempt(1);
        auto result = (text /= suffix);
        REQUIRE(result);
        CHECK(failing.Attempts() == 0);
        CHECK(text.Data() == address);
        CHECK(text.View() == StringView("abc/한글ZZ"));
    }
    CHECK(system.Stats().liveAllocations == 0);
}
