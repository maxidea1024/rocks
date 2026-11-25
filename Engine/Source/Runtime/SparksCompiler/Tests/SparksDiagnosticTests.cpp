#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// Sparks 진단 시험이다(Docs/Design/SparksLanguage 8.1절 "형식"·"출력 세부"·"순서"): 출력
// golden, 밑줄 폭(code point·최소 1·줄 끝 자름), 표시 정리(tab·CR·잘못된 UTF-8), 파일 전체 진단,
// 정렬, 코드 표.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_diagnostic_tests_file {

namespace {

// 진단을 그려 기대 문자열과 비교한다.
bool RendersAs(const SourceSet& sources, const DiagnosticList& diagnostics, StringView expected,
               Allocator& allocator) noexcept {
    StringBuilder<512> out(allocator);
    if (!AppendDiagnostics(out, sources, diagnostics)) {
        return false;
    }

    return out.View() == expected;
}

} // namespace

TEST(SparksDiagnostic_RendersTheDocumentedFormat) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("UI/HealthBar.spark",
                                 "class A {\n    let hp = Target.Current;\n}\n", diagnostics);
        REQUIRE(id);
        // "Target"은 둘째 줄 14열부터 6 byte다.
        const uint32 begin = sources.Get(id.Value()).LineBegin(2) + 13;
        REQUIRE(diagnostics.TryAdd(
            DiagnosticCode::InvalidUtf8, id.Value(), SourceSpan{begin, begin + 6},
            "'Target' may be null.",
            "'Target' has type 'Health?'.\nAnother actor reads as null once destroyed.",
            "if (let health = Target) { let hp = health.Current; }"));
        CHECK(RendersAs(sources, diagnostics,
                        "UI/HealthBar.spark:2:14: error S0101: 'Target' may be null.\n"
                        "        let hp = Target.Current;\n"
                        "                 ^^^^^^\n"
                        "  'Target' has type 'Health?'.\n"
                        "  Another actor reads as null once destroyed.\n"
                        "  fix: if (let health = Target) { let hp = health.Current; }\n",
                        allocator));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 밑줄 폭은 code point 수다. 빈 범위는 `^` 하나, 줄 끝을 넘는 범위(여러 줄)는 첫 줄 끝에서 자른다.
// 설명·수정 예가 없으면 그 줄이 없다.
TEST(SparksDiagnostic_UnderlinesCodePointsAndClipsAtLineEnd) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        // "한글"(6 byte) 뒤에 ab, 다음 줄 cd.
        auto id = sources.TryAdd("K.spark",
                                 "\xED\x95\x9C\xEA\xB8\x80"
                                 "ab\ncd",
                                 diagnostics);
        REQUIRE(id);
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::InvalidUtf8, id.Value(), SourceSpan{0, 6},
                                   "Two letters.", "", ""));
        CHECK(RendersAs(sources, diagnostics,
                        "K.spark:1:1: error S0101: Two letters.\n"
                        "    \xED\x95\x9C\xEA\xB8\x80"
                        "ab\n"
                        "    ^^\n",
                        allocator));

        DiagnosticList empty(allocator);
        REQUIRE(empty.TryAdd(DiagnosticCode::InvalidUtf8, id.Value(), SourceSpan{7, 7},
                             "Empty span.", "", ""));
        CHECK(RendersAs(sources, empty,
                        "K.spark:1:4: error S0101: Empty span.\n"
                        "    \xED\x95\x9C\xEA\xB8\x80"
                        "ab\n"
                        "       ^\n",
                        allocator));

        DiagnosticList clipped(allocator);
        REQUIRE(clipped.TryAdd(DiagnosticCode::InvalidUtf8, id.Value(), SourceSpan{6, 11},
                               "Two lines.", "", "fix it"));
        CHECK(RendersAs(sources, clipped,
                        "K.spark:1:3: error S0101: Two lines.\n"
                        "    \xED\x95\x9C\xEA\xB8\x80"
                        "ab\n"
                        "      ^^\n"
                        "  fix: fix it\n",
                        allocator));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 원문 줄의 tab·CR 등 제어 문자는 공백 한 칸, 잘못된 UTF-8 byte는 U+FFFD로 보인다(열 계산과 맞고
// 출력은 항상 UTF-8).
TEST(SparksDiagnostic_SanitizesTheDisplayedLine) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto tab = sources.TryAdd("Tab.spark", "\tx\ry\n", diagnostics);
        REQUIRE(tab);
        REQUIRE(diagnostics.Count() == 1); // S0102(홀로 선 CR)
        CHECK(RendersAs(sources, diagnostics,
                        "Tab.spark:1:3: error S0102: Carriage return without a line feed.\n"
                        "     x y\n"
                        "      ^\n"
                        "  Some editors treat a lone CR as a line break and others do not, so line "
                        "numbers would disagree.\n"
                        "  fix: Use LF or CRLF line endings.\n",
                        allocator));

        DiagnosticList invalid(allocator);
        auto bad = sources.TryAdd("Bad.spark", "ab\xFF\x80z", invalid);
        REQUIRE(bad);
        REQUIRE(invalid.Count() == 1);
        CHECK(RendersAs(sources, invalid,
                        "Bad.spark:1:3: error S0101: Source is not valid UTF-8.\n"
                        "    ab\xEF\xBF\xBD\xEF\xBF\xBDz\n"
                        "      ^\n"
                        "  The byte 0xFF at this position does not start a valid UTF-8 sequence.\n"
                        "  fix: Save the file with UTF-8 encoding.\n",
                        allocator));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksDiagnostic_FileLevelDiagnosticHasNoPosition) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("Whole.spark", "x", diagnostics);
        REQUIRE(id);
        REQUIRE(diagnostics.TryAddFileLevel(DiagnosticCode::SourceTooLarge, id.Value(),
                                            "Too large.", "Because.", "Split it."));
        CHECK(RendersAs(sources, diagnostics,
                        "Whole.spark: error S0103: Too large.\n"
                        "  Because.\n"
                        "  fix: Split it.\n",
                        allocator));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 정렬: 경로 byte 순 → 파일 전체 진단 먼저 → 시작 위치 → 코드 → 더한 순서.
TEST(SparksDiagnostic_SortsDeterministically) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto b = sources.TryAdd("b.spark", "0123456789", diagnostics);
        auto a = sources.TryAdd("a.spark", "0123456789", diagnostics);
        REQUIRE(a && b);
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::SourceTooLarge, b.Value(), SourceSpan{1, 2},
                                   "b1", "", ""));
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::LoneCarriageReturn, a.Value(), SourceSpan{5, 6},
                                   "a5 102", "", ""));
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::InvalidUtf8, a.Value(), SourceSpan{5, 6},
                                   "a5 101 first", "", ""));
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::InvalidUtf8, a.Value(), SourceSpan{5, 6},
                                   "a5 101 second", "", ""));
        REQUIRE(diagnostics.TryAddFileLevel(DiagnosticCode::SourceTooLarge, a.Value(), "a file", "",
                                            ""));
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::InvalidUtf8, a.Value(), SourceSpan{2, 3}, "a2",
                                   "", ""));
        diagnostics.Sort(sources);
        const StringView expected[] = {"a file",        "a2",     "a5 101 first",
                                       "a5 101 second", "a5 102", "b1"};
        REQUIRE(diagnostics.Count() == sizeof(expected) / sizeof(expected[0]));
        for (usize index = 0; index < diagnostics.Count(); ++index) {
            CHECK(diagnostics[index].Message() == expected[index]);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksDiagnostic_RejectsInvalidArgumentsWithoutChanges) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("A.spark", "abc", diagnostics);
        REQUIRE(id);
        auto noMessage = diagnostics.TryAdd(DiagnosticCode::InvalidUtf8, id.Value(),
                                            SourceSpan{0, 1}, "", "", "");
        CHECK(!noMessage && noMessage.Error() == ErrorCode::InvalidArgument);
        auto reversed = diagnostics.TryAdd(DiagnosticCode::InvalidUtf8, id.Value(),
                                           SourceSpan{2, 1}, "m", "", "");
        CHECK(!reversed && reversed.Error() == ErrorCode::InvalidArgument);
        auto badText = diagnostics.TryAdd(DiagnosticCode::InvalidUtf8, id.Value(), SourceSpan{0, 1},
                                          "m", "\xFF", "");
        CHECK(!badText && badText.Error() == ErrorCode::InvalidArgument);
        CHECK(diagnostics.IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksDiagnostic_AddIsAtomicUnderAllocationFailure) {
    SystemAllocator backing;
    for (uint64 failAt = 1;; ++failAt) {
        test::NFailAllocator allocator(backing);
        bool succeeded = false;
        {
            SourceSet sources(allocator);
            DiagnosticList diagnostics(allocator);
            REQUIRE(sources.TryAdd("A.spark", "abc", diagnostics));
            allocator.FailOnAttempt(failAt);
            auto added = diagnostics.TryAdd(DiagnosticCode::InvalidUtf8, SourceId{0},
                                            SourceSpan{0, 1}, "message", "explanation", "fix");
            allocator.DisableFailure();
            if (added) {
                succeeded = true;
                CHECK(diagnostics.Count() == 1);
            } else {
                CHECK(added.Error() == ErrorCode::OutOfMemory);
                CHECK(diagnostics.IsEmpty());
            }
        }

        CHECK(backing.Stats().liveAllocations == 0);
        if (succeeded) {
            break;
        }
    }
}

TEST(SparksDiagnostic_CodeTableIsSortedAndSearchable) {
    const Span<const DiagnosticCodeInfo> codes = AllDiagnosticCodes();
    REQUIRE(codes.Size() >= 3);
    for (usize index = 0; index < codes.Size(); ++index) {
        const uint32 number = static_cast<uint16>(codes[index].code);
        CHECK(number >= 100 && number <= 1199); // 8.1절 대역 S0100–S1199
        CHECK(!codes[index].name.IsEmpty());
        CHECK(FindDiagnosticCode(number) == &codes[index]);
        if (index > 0) {
            CHECK(static_cast<uint16>(codes[index - 1].code) < number);
        }
    }

    CHECK(FindDiagnosticCode(0) == nullptr);
    CHECK(FindDiagnosticCode(199) == nullptr);
    CHECK(FindDiagnosticCode(9999) == nullptr);
    CHECK(FindDiagnosticCode(101)->name == StringView("invalid-utf8"));
}

// 밑줄 경계: BOM 안에서 시작하는 범위는 BOM 뒤부터 세고, 줄 끝(LF 위치)의 빈 범위는 줄 끝 열의 `^`
// 하나다.
TEST(SparksDiagnostic_UnderlinesFromBomAndAtLineEnd) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList ignored(allocator);
        auto id = sources.TryAdd("B.spark",
                                 "\xEF\xBB\xBF"
                                 "abc\nd",
                                 ignored);
        REQUIRE(id);
        DiagnosticList fromBom(allocator);
        REQUIRE(fromBom.TryAdd(DiagnosticCode::InvalidCharacter, id.Value(), SourceSpan{0, 5},
                               "From BOM.", "", ""));
        CHECK(RendersAs(sources, fromBom,
                        "B.spark:1:1: error S0104: From BOM.\n"
                        "    abc\n"
                        "    ^^\n",
                        allocator));

        DiagnosticList atEnd(allocator);
        REQUIRE(atEnd.TryAdd(DiagnosticCode::InvalidCharacter, id.Value(), SourceSpan{6, 6},
                             "At end.", "", ""));
        CHECK(RendersAs(sources, atEnd,
                        "B.spark:1:4: error S0104: At end.\n"
                        "    abc\n"
                        "       ^\n",
                        allocator));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// C1 제어 문자(U+0085)와 bidi override(U+202E)는 공백 한 칸으로 보인다(원문이 다르게 보이지 않게).
// 설명의 빈 줄은 들여쓰지 않는다.
TEST(SparksDiagnostic_HidesBidiAndC1ControlsAndKeepsBlankNoteLines) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList ignored(allocator);
        auto id = sources.TryAdd("H.spark",
                                 "a\xC2\x85"
                                 "b\xE2\x80\xAE"
                                 "c",
                                 ignored);
        REQUIRE(id);
        DiagnosticList diagnostics(allocator);
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::InvalidCharacter, id.Value(), SourceSpan{7, 8},
                                   "Hidden.", "first\n\nthird", ""));
        CHECK(RendersAs(sources, diagnostics,
                        "H.spark:1:5: error S0104: Hidden.\n"
                        "    a b c\n"
                        "        ^\n"
                        "  first\n"
                        "\n"
                        "  third\n",
                        allocator));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 메시지와 수정 예는 한 줄이어야 한다(첫 줄 형식).
TEST(SparksDiagnostic_RejectsMultiLineMessageAndFix) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("A.spark", "abc", diagnostics);
        REQUIRE(id);
        auto message = diagnostics.TryAdd(DiagnosticCode::InvalidCharacter, id.Value(),
                                          SourceSpan{0, 1}, "two\nlines", "", "");
        CHECK(!message && message.Error() == ErrorCode::InvalidArgument);
        auto fix = diagnostics.TryAddFileLevel(DiagnosticCode::InvalidCharacter, id.Value(), "m",
                                               "", "a\nb");
        CHECK(!fix && fix.Error() == ErrorCode::InvalidArgument);
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::InvalidCharacter, id.Value(), SourceSpan{0, 1},
                                   "m", "explanation\nmay span lines", "one line"));
        CHECK(diagnostics.Count() == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 경로의 서로 다른 파일은 더한 순서로 정렬한다.
TEST(SparksDiagnostic_SamePathFilesSortInAddOrder) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto first = sources.TryAdd("Same.spark", "xyz", diagnostics);
        auto second = sources.TryAdd("Same.spark", "xyz", diagnostics);
        REQUIRE(first && second);
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::InvalidCharacter, second.Value(),
                                   SourceSpan{0, 1}, "second", "", ""));
        REQUIRE(diagnostics.TryAdd(DiagnosticCode::InvalidCharacter, first.Value(),
                                   SourceSpan{2, 3}, "first", "", ""));
        diagnostics.Sort(sources);
        CHECK(diagnostics[0].Message() == StringView("first"));
        CHECK(diagnostics[1].Message() == StringView("second"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace sparks_diagnostic_tests_file
