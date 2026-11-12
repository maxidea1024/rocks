#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

TEST(StringPathOperatorsPreserveUnrealAppendRules) {
    struct Case {
        const char* left;
        const char* right;
        const char* expected;
    };
    // UE 5.6 PathAppend의 구분자 경계다. 정규화나 절대 경로 치환은 하지 않는다.
    const Case cases[] = {{"", "", ""},
                          {"", "item", "item"},
                          {"dir", "", "dir/"},
                          {"dir/", "", "dir/"},
                          {"dir\\", "", "dir\\"},
                          {"dir", "item", "dir/item"},
                          {"dir/", "item", "dir/item"},
                          {"dir\\", "item", "dir\\item"},
                          {"dir", "/item", "dir/item"},
                          {"dir/", "/item", "dir//item"},
                          {"dir", "\\item", "dir/\\item"},
                          {"dir", "../item", "dir/../item"},
                          {"dir", "C:/item", "dir/C:/item"}};
    SystemAllocator allocator;
    for (const Case& sample : cases) {
        String left(allocator);
        REQUIRE(left.TryAssign(sample.left));
        const char* original = left.Data();
        auto combined = left / sample.right;
        REQUIRE(combined);
        CHECK(combined.Value().View() == StringView(sample.expected));
        CHECK(left.Data() == original && left.View() == StringView(sample.left));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringPathOperatorsChainWithStringsViewsAndAllocatorOwnership) {
    SystemAllocator firstAllocator;
    SystemAllocator secondAllocator;
    {
        String base(firstAllocator);
        String child(secondAllocator);
        REQUIRE(base.TryAssign("root"));
        REQUIRE(child.TryAssign("한글"));

        auto chain = base / "Content" / child / StringView("🎮");
        REQUIRE(chain);
        CHECK(chain.Value().View() == StringView("root/Content/한글/🎮"));
        CHECK(&chain.Value().GetAllocator() == &firstAllocator);
        CHECK(base.View() == StringView("root"));
        CHECK(child.View() == StringView("한글"));

        auto leftView = StringView("prefix") / child;
        REQUIRE(leftView);
        CHECK(leftView.Value().View() == StringView("prefix/한글"));
        CHECK(&leftView.Value().GetAllocator() == &secondAllocator);
        auto leftLiteral = "prefix" / child;
        REQUIRE(leftLiteral);
        CHECK(leftLiteral.Value().View() == StringView("prefix/한글"));
    }
    CHECK(firstAllocator.Stats().liveAllocations == 0);
    CHECK(secondAllocator.Stats().liveAllocations == 0);
}

TEST(StringPathAppendIsAtomicAndSupportsAliasedInput) {
    SystemAllocator allocator;
    {
        String value(allocator);
        REQUIRE(value.TryAssign("root"));
        auto first = (value /= value.View());
        REQUIRE(first);
        CHECK(value.View() == StringView("root/root"));

        auto piece = value.View().TrySliceBytes(0, 4);
        REQUIRE(piece);
        auto second = (value /= piece.Value());
        REQUIRE(second);
        CHECK(value.View() == StringView("root/root/root"));

        auto third = (value /= value);
        REQUIRE(third);
        CHECK(value.View() == StringView("root/root/root/root/root/root"));

        const char* before = value.Data();
        const StringView unchanged = value.View();
        constexpr char nul[] = {'a', '\0', 'b'};
        auto invalid = (value /= StringView(nul, sizeof(nul)));
        CHECK(!invalid && invalid.Error() == ErrorCode::InvalidArgument);
        CHECK(value.Data() == before && value.View() == unchanged);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringPathOperatorsPropagateErrorsAndEveryAllocationFailure) {
    SystemAllocator system;
    {
        test::NFailAllocator failing(system);
        String base(failing);
        REQUIRE(base.TryAssign("base"));
        const char* original = base.Data();
        const auto baseline = system.Stats().liveAllocations;

        failing.DisableFailure();
        uint64 attempts = 0;
        {
            auto success = base / "first" / "second";
            REQUIRE(success);
            CHECK(success.Value().View() == StringView("base/first/second"));
            attempts = failing.Attempts();
        }
        REQUIRE(attempts > 0);
        CHECK(system.Stats().liveAllocations == baseline);
        for (uint64 attempt = 1; attempt <= attempts; ++attempt) {
            failing.FailOnAttempt(attempt);
            auto failed = base / "first" / "second";
            CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
            CHECK(base.Data() == original && base.View() == StringView("base"));
            CHECK(system.Stats().liveAllocations == baseline);
        }

        failing.FailOnAttempt(1);
        auto failedAppend = (base /= "child");
        CHECK(!failedAppend && failedAppend.Error() == ErrorCode::OutOfMemory);
        CHECK(base.Data() == original && base.View() == StringView("base"));
        CHECK(system.Stats().liveAllocations == baseline);

        failing.DisableFailure();
        constexpr char nul[] = {'x', '\0', 'y'};
        auto bad = base / StringView(nul, sizeof(nul));
        CHECK(!bad && bad.Error() == ErrorCode::InvalidArgument);
        auto malformed = base / StringView("x\xFF", 2);
        CHECK(!malformed && malformed.Error() == ErrorCode::InvalidArgument);
        auto earlier = Result<String>::Failure(ErrorCode::DependencyFailed) / StringView(nul, 3);
        CHECK(!earlier && earlier.Error() == ErrorCode::DependencyFailed);
        CHECK(base.Data() == original && base.View() == StringView("base"));
    }
    CHECK(system.Stats().liveAllocations == 0);
}
