#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

TEST(StringPathInPlaceUtf8AliasesStayValid) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        REQUIRE(text.TryReserve(64));
        REQUIRE(text.TryAssign("한글🎮"));
        const char* address = text.Data();
        const usize capacity = text.ByteCapacity();
        failing.FailOnAttempt(1);
        auto repeated = (text /= text.View());
        REQUIRE(repeated);
        CHECK(text.View() == StringView("한글🎮/한글🎮"));
        CHECK(text.Data() == address && text.ByteCapacity() == capacity);
        CHECK(failing.Attempts() == 0);

        failing.DisableFailure();
        REQUIRE(text.TryAssign("한글X🎮"));
        REQUIRE(text.TryAssign("한글"));
        // 앞에 둔 ASCII X를 새 종단 NUL이 덮는다. 그 뒤 이모지 byte는 살아 있다.
        const StringView tail(text.Data() + text.ByteCount() + 1, 4);
        REQUIRE(tail == StringView("🎮"));
        failing.FailOnAttempt(1);
        auto behindTerminator = (text /= tail);
        REQUIRE(behindTerminator);
        CHECK(text.View() == StringView("한글/🎮"));
        CHECK(text.Data() == address && text.ByteCapacity() == capacity);
        CHECK(failing.Attempts() == 0);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(StringPathInPlaceHandlesNoSeparatorAndReservedEmpty) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        REQUIRE(text.TryReserve(64));
        const char* address = text.Data();
        const usize capacity = text.ByteCapacity();
        REQUIRE(text.TryAssign("root/"));
        failing.FailOnAttempt(1);
        auto trailingSlash = (text /= "child");
        REQUIRE(trailingSlash);
        CHECK(text.View() == StringView("root/child"));
        CHECK(text.Data() == address && text.ByteCapacity() == capacity);
        CHECK(failing.Attempts() == 0);

        failing.DisableFailure();
        REQUIRE(text.TryAssign("root"));
        failing.FailOnAttempt(1);
        auto leadingSlash = (text /= "/child");
        REQUIRE(leadingSlash);
        CHECK(text.View() == StringView("root/child"));
        CHECK(text.Data() == address && text.ByteCapacity() == capacity);
        CHECK(failing.Attempts() == 0);

        text.Clear();
        failing.FailOnAttempt(1);
        auto reservedEmpty = (text /= "child");
        REQUIRE(reservedEmpty);
        CHECK(text.View() == StringView("child"));
        CHECK(text.Data() == address && text.ByteCapacity() == capacity);
        CHECK(failing.Attempts() == 0);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(StringPathInPlaceAcceptsExactCapacityBoundary) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        // "root" 4 byte + '/' 1 byte + "한" 3 byte가 capacity와 정확히 같다.
        REQUIRE(text.TryReserve(8));
        REQUIRE(text.TryAssign("root"));
        REQUIRE(text.ByteCapacity() == 8);
        const char* address = text.Data();
        failing.FailOnAttempt(1);
        auto exact = (text /= "한");
        REQUIRE(exact);
        CHECK(text.ByteCount() == 8 && text.View() == StringView("root/한"));
        CHECK(text.Data() == address && text.ByteCapacity() == 8);
        CHECK(text.CStr()[8] == '\0');
        CHECK(failing.Attempts() == 0);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(StringPathInPlaceInvalidSuffixPreservesAllState) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        REQUIRE(text.TryReserve(64));
        REQUIRE(text.TryAssign("한글"));
        const char* address = text.Data();
        const usize capacity = text.ByteCapacity();
        const auto live = system.Stats().liveAllocations;
        constexpr char embedded[] = {'x', '\0', 'y'};
        const StringView cases[] = {
            StringView("\xED\xA0\x80", 3),
            StringView(embedded, sizeof(embedded)),
            // 현재 payload 끝의 NUL까지 빌린 view도 길이 기반으로 검사한다.
            StringView(text.Data(), text.ByteCount() + 1),
        };
        for (StringView suffix : cases) {
            failing.FailOnAttempt(1);
            auto invalid = (text /= suffix);
            CHECK(!invalid && invalid.Error() == ErrorCode::InvalidArgument);
            CHECK(text.Data() == address && text.ByteCapacity() == capacity);
            CHECK(text.View() == StringView("한글"));
            CHECK(text.CStr()[text.ByteCount()] == '\0');
            CHECK(system.Stats().liveAllocations == live);
            CHECK(failing.Attempts() == 0);
        }
    }
    CHECK(system.Stats().liveAllocations == 0);
}
