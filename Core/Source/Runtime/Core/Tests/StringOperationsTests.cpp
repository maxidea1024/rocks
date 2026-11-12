#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

char TestFold(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

// 검색 구현과 다른 독립 참조 모델로 모든 후보 offset을 앞에서 열거한다.
usize ReferenceFind(StringView text, StringView needle, bool ignoreCase, bool reverse,
                    usize boundary) noexcept {
    const usize size = text.ByteCount();
    boundary = boundary < size ? boundary : (reverse || size == 0 ? size : size - 1);
    if (needle.IsEmpty()) {
        return reverse ? (boundary == 0 ? StringView::NotFound : boundary - 1) : boundary;
    }
    usize answer = StringView::NotFound;
    for (usize candidate = 0; candidate < size; ++candidate) {
        if (needle.ByteCount() > size - candidate || (!reverse && candidate < boundary) ||
            (reverse && (candidate > boundary || needle.ByteCount() > boundary - candidate))) {
            continue;
        }
        bool equal = true;
        for (usize i = 0; i < needle.ByteCount(); ++i) {
            char lhs = text.Data()[candidate + i];
            char rhs = needle.Data()[i];
            if (ignoreCase) {
                lhs = TestFold(lhs);
                rhs = TestFold(rhs);
            }
            if (lhs != rhs) {
                equal = false;
                break;
            }
        }
        if (equal) {
            answer = candidate;
            if (!reverse) {
                break;
            }
        }
    }
    return answer;
}

} // namespace

TEST(StringViewFindDirectionCaseAndLengthBasedBytes) {
    const StringView text("aBAba");
    CHECK(text.Find("ab") == 0);
    CHECK(text.Find("ab", StringComparison::CaseSensitive) == StringView::NotFound);
    CHECK(text.Find("ab", StringComparison::IgnoreAsciiCase, StringSearchDirection::FromEnd) == 2);
    CHECK(text.Find("ab", StringComparison::IgnoreAsciiCase, StringSearchDirection::FromEnd, 2) ==
          0);
    CHECK(text.Find("ab", StringComparison::IgnoreAsciiCase, StringSearchDirection::FromEnd, 1) ==
          StringView::NotFound);
    CHECK(text.Find("ab", StringComparison::IgnoreAsciiCase, StringSearchDirection::FromStart, 1) ==
          2);
    CHECK(text.Find("", StringComparison::CaseSensitive, StringSearchDirection::FromEnd) == 4);
    CHECK(text.Find("", StringComparison::CaseSensitive, StringSearchDirection::FromStart, 99) ==
          4);
    CHECK(text.Contains("BA"));
    CHECK(text.StartsWith("AB"));
    CHECK(text.EndsWith("BA"));
    CHECK(!text.StartsWith("AB", StringComparison::CaseSensitive));
    CHECK(StringView().Contains(""));
    CHECK(StringView().StartsWith(""));
    CHECK(StringView().EndsWith(""));

    constexpr char embedded[] = {'A', '\0', 'b', '\0', 'B'};
    const StringView bytes(embedded, sizeof(embedded));
    constexpr char needle[] = {'\0', 'B'};
    CHECK(bytes.Find(StringView(needle, sizeof(needle))) == 1);
    CHECK(bytes.Find(StringView(needle, sizeof(needle)), StringComparison::CaseSensitive) == 3);
    CHECK(StringView("한글a").Find("A") == 6);
    CHECK(!StringView("한글").Contains("韓"));
    CHECK(!StringView("\xC3\x89").Contains("\xC3\xA9"));      // ASCII 밖은 case folding하지 않는다.
    CHECK(StringView("한").Find(StringView("\x9C", 1)) == 2); // 이 질의는 raw byte다.
}

TEST(StringViewFindDeterministicIndependentOracle) {
    uint32 seed = 0x2a7365u;
    const char alphabet[] = {'a', 'A', 'b', 'B', '\0', 'x'};
    char haystack[15]{};
    char needle[6]{};
    for (uint32 iteration = 0; iteration < 600; ++iteration) {
        seed = seed * 1664525u + 1013904223u;
        const usize size = seed % (sizeof(haystack) + 1);
        seed = seed * 1664525u + 1013904223u;
        const usize patternSize = seed % (sizeof(needle) + 1);
        for (usize i = 0; i < size; ++i) {
            seed = seed * 1664525u + 1013904223u;
            haystack[i] = alphabet[(seed >> 16) % sizeof(alphabet)];
        }
        for (usize i = 0; i < patternSize; ++i) {
            seed = seed * 1664525u + 1013904223u;
            needle[i] = alphabet[(seed >> 16) % sizeof(alphabet)];
        }
        const StringView text(haystack, size);
        const StringView pattern(needle, patternSize);
        for (usize boundary = 0; boundary <= size; ++boundary) {
            for (uint32 ignoreIndex = 0; ignoreIndex < 2; ++ignoreIndex) {
                const bool ignore = ignoreIndex != 0;
                for (uint32 reverseIndex = 0; reverseIndex < 2; ++reverseIndex) {
                    const bool reverse = reverseIndex != 0;
                    const auto mode = ignore ? StringComparison::IgnoreAsciiCase
                                             : StringComparison::CaseSensitive;
                    const auto direction =
                        reverse ? StringSearchDirection::FromEnd : StringSearchDirection::FromStart;
                    CHECK(text.Find(pattern, mode, direction, boundary) ==
                          ReferenceFind(text, pattern, ignore, reverse, boundary));
                }
            }
        }
    }
}

TEST(StringViewByteSlicesEnforceUtf8Boundaries) {
    StringView text("A한🎮Z");
    auto left = text.TryLeftBytes(4);
    auto right = text.TryRightBytes(5);
    auto mid = text.TryMidBytes(1, 7);
    auto clipped = text.TryMidBytes(100, 100);
    REQUIRE(left && right && mid && clipped);
    CHECK(left.Value() == StringView("A한"));
    CHECK(right.Value() == StringView("🎮Z"));
    CHECK(mid.Value() == StringView("한🎮"));
    CHECK(clipped.Value().IsEmpty());
    CHECK(!text.TryLeftBytes(2));
    CHECK(!text.TryRightBytes(3));
    CHECK(!text.TryMidBytes(2, 0));
    const StringView malformed("\xED\xA0\x80", 3);
    CHECK(!malformed.TryLeftBytes(0));
    CHECK(!malformed.TryMidBytes(90));
    REQUIRE(text.TryLeftBytes(MaxSize));
    CHECK(text.TryLeftBytes(MaxSize).Value() == text);
}

TEST(StringViewSplitAndTrimBorrowWithoutAllocation) {
    StringView input(" One:TWO:Three ");
    StringView left("unchanged"), right("unchanged");
    CHECK(!input.Split("missing", left, right));
    CHECK(left == StringView("unchanged") && right == StringView("unchanged"));
    CHECK(input.Split(":", left, right));
    CHECK(left == StringView(" One") && right == StringView("TWO:Three "));
    CHECK(input.Split(":", input, right, StringComparison::CaseSensitive,
                      StringSearchDirection::FromEnd));
    CHECK(input == StringView(" One:TWO") && right == StringView("Three "));
    CHECK(input.TrimAscii() == StringView("One:TWO"));
    CHECK(StringView("\t\r\n \v\f").TrimAscii().IsEmpty());
    CHECK(StringView("\xC2\xA0 A \xC2\xA0").TrimAscii() == StringView("\xC2\xA0 A \xC2\xA0"));
    StringView empty;
    CHECK(empty.TrimAscii().IsEmpty());
    CHECK(empty.Split("", left, right));
    CHECK(left.IsEmpty() && right.IsEmpty());
}

TEST(StringOperationsAccessibleAndInPlaceAscii) {
    SystemAllocator allocator;
    {
        String text(allocator);
        REQUIRE(text.TryAssign(" \tHeLLo 한글🎮 \r\n"));
        const auto allocationCount = allocator.Stats().totalAllocations;
        const char* address = text.Data();
        text.TrimAscii();
        CHECK(text.View() == StringView("HeLLo 한글🎮"));
        CHECK(text.Contains("HELLO"));
        CHECK(text.Find("한글") == 6);
        CHECK(text.TryLeftBytes(5).Value() == StringView("HeLLo"));
        text.ToUpperAscii();
        CHECK(text.View() == StringView("HELLO 한글🎮"));
        text.ToLowerAscii();
        CHECK(text.View() == StringView("hello 한글🎮"));
        CHECK(text.Data() == address && allocator.Stats().totalAllocations == allocationCount);
        text.Clear();
        text.TrimAscii();
        text.TrimAsciiStart();
        text.TrimAsciiEnd();
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringReplaceIsAtomicHandlesAliasAndEmbeddedNull) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        REQUIRE(text.TryAssign("aXaXb"));
        const char* original = text.Data();
        const usize capacity = text.ByteCapacity();
        const StringView from(text.Data() + 1, 1);
        const StringView to(text.Data(), 2);
        failing.FailOnAttempt(1);
        auto failure = text.TryReplace(from, to);
        CHECK(!failure && failure.Error() == ErrorCode::OutOfMemory);
        CHECK(text.Data() == original && text.ByteCapacity() == capacity);
        CHECK(text.View() == StringView("aXaXb"));
        failing.DisableFailure();
        auto replaced = text.TryReplace(from, to);
        REQUIRE(replaced);
        CHECK(replaced.Value() == 2);
        CHECK(text.View() == StringView("aaXaaXb"));
        const auto allocations = system.Stats().totalAllocations;
        auto absent = text.TryReplace("missing", "longer");
        REQUIRE(absent);
        CHECK(absent.Value() == 0 && system.Stats().totalAllocations == allocations);
        auto empty = text.TryReplace("", "z");
        REQUIRE(empty);
        CHECK(empty.Value() == 0);
        const char* unchanged = text.Data();
        CHECK(!text.TryReplace("\xFF", "z"));
        CHECK(!text.TryReplace("x", "\xFF"));
        CHECK(text.Data() == unchanged && text.View() == StringView("aaXaaXb"));

        constexpr char content[] = {'a', '\0', 'A', '\0', 'b'};
        constexpr char search[] = {'\0', 'a'};
        constexpr char value[] = {'X', '\0'};
        REQUIRE(text.TryAssign(StringView(content, sizeof(content))));
        auto embedded =
            text.TryReplace(StringView(search, sizeof(search)), StringView(value, sizeof(value)));
        REQUIRE(embedded);
        constexpr char expected[] = {'a', 'X', '\0', '\0', 'b'};
        CHECK(embedded.Value() == 1);
        CHECK(text.View() == StringView(expected, sizeof(expected)));
        auto removed = text.TryReplace(text.View(), "");
        REQUIRE(removed);
        CHECK(removed.Value() == 1 && text.IsEmpty());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(StringFindBoundaryMatchesUe56ByteEquivalent) {
    const StringView text("abc");
    CHECK(text.Find("c", StringComparison::CaseSensitive, StringSearchDirection::FromStart, 3) ==
          2);
    CHECK(text.Find("c", StringComparison::CaseSensitive, StringSearchDirection::FromStart, 99) ==
          2);
    CHECK(text.Find("", StringComparison::CaseSensitive, StringSearchDirection::FromStart, 99) ==
          2);
    CHECK(text.Find("", StringComparison::CaseSensitive, StringSearchDirection::FromEnd) == 2);
    CHECK(text.Find("", StringComparison::CaseSensitive, StringSearchDirection::FromEnd, 2) == 1);
    CHECK(text.Find("", StringComparison::CaseSensitive, StringSearchDirection::FromEnd, 0) ==
          StringView::NotFound);
    CHECK(StringView().Find("", StringComparison::CaseSensitive,
                            StringSearchDirection::FromStart) == 0);
    CHECK(StringView().Find("", StringComparison::CaseSensitive, StringSearchDirection::FromEnd) ==
          StringView::NotFound);
    StringView left, right;
    CHECK(text.Split("", left, right, StringComparison::CaseSensitive,
                     StringSearchDirection::FromEnd));
    CHECK(left == StringView("ab") && right == StringView("c"));
}

TEST(StringReplaceSameSensitiveDoesNotAllocateOrInvalidate) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        String text(failing);
        REQUIRE(text.TryAssign("aBaB"));
        const StringView borrowed = text.View();
        const StringView from(text.Data() + 1, 1);
        const char* address = text.Data();
        const usize capacity = text.ByteCapacity();
        failing.FailOnAttempt(1);
        auto same = text.TryReplace(from, "B", StringComparison::CaseSensitive);
        REQUIRE(same);
        CHECK(same.Value() == 0 && failing.Attempts() == 0);
        CHECK(text.Data() == address && text.ByteCapacity() == capacity && text.View() == borrowed);
        auto sameBorrowed = text.TryReplace(borrowed, borrowed, StringComparison::CaseSensitive);
        REQUIRE(sameBorrowed);
        CHECK(sameBorrowed.Value() == 0 && failing.Attempts() == 0);
        CHECK(text.Data() == address && text.View() == borrowed);
        // IgnoreAsciiCase는 UE와 같이 일치 횟수를 유지하며 일반 치환 경로를 사용한다.
        failing.DisableFailure();
        auto ignore = text.TryReplace("B", "B", StringComparison::IgnoreAsciiCase);
        REQUIRE(ignore);
        CHECK(ignore.Value() == 2 && text.View() == StringView("aBaB"));
    }
    CHECK(system.Stats().liveAllocations == 0);
}
