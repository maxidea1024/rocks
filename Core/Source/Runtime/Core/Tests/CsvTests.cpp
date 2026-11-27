#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Csv.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// CSV 읽기 시험이다(Core/Text/Csv.h, Docs/Design/CurveAssets 5.2절): UE FCsvParser와 같은 규칙의
// 대조표(빈 줄·공백 줄·따옴표 위치·`""`·닫힌 뒤 글자·닫히지 않은 따옴표·줄 끝 셋·끝 쉼표·NUL),
// 물리 줄 번호, 할당 실패 때 out 그대로.
using namespace rocks;

namespace csv_tests_file {

namespace {

// 기대 행: 칸을 '|'로 이은 문자열과 시작 줄 번호다.
struct ExpectedRow {
    const char* cells;
    usize line;
};

struct CsvCase {
    const char* name;
    StringView text;
    Span<const ExpectedRow> rows;
};

bool Matches(const Array<CsvRow>& rows, Span<const ExpectedRow> expected,
             const char* name) noexcept {
    bool same = rows.Size() == expected.Size();
    for (usize index = 0; same && index < rows.Size(); ++index) {
        char joined[256] = {};
        usize used = 0;
        for (usize cell = 0; cell < rows[index].cells.Size(); ++cell) {
            const StringView text = rows[index].cells[cell].View();
            if (cell > 0 && used + 1 < sizeof(joined)) {
                joined[used++] = '|';
            }

            for (usize at = 0; at < text.ByteCount() && used + 1 < sizeof(joined); ++at) {
                joined[used++] = text.Data()[at];
            }
        }

        same =
            strcmp(joined, expected[index].cells) == 0 && rows[index].line == expected[index].line;
        if (!same) {
            printf("  %s row %zu: '%s' line %zu, expected '%s' line %zu\n", name, index, joined,
                   rows[index].line, expected[index].cells, expected[index].line);
        }
    }

    if (rows.Size() != expected.Size()) {
        printf("  %s: %zu rows, expected %zu\n", name, rows.Size(), expected.Size());
    }

    return same;
}

} // namespace

// UE FCsvParser 규칙 대조표(CsvParser.cpp:25-160).
TEST(Csv_UeParserRules) {
    SystemAllocator allocator;
    static constexpr ExpectedRow simple[] = {{"a|b|c", 1}, {"1|2|3", 2}};
    static constexpr ExpectedRow blankLines[] = {{"a", 2}, {"b", 4}};
    static constexpr ExpectedRow spaceLine[] = {{"a", 1}, {"  ", 2}, {"b", 3}};
    static constexpr ExpectedRow quoted[] = {{"a,b|c", 1}};
    static constexpr ExpectedRow escaped[] = {{"say \"hi\"|x", 1}};
    static constexpr ExpectedRow afterClose[] = {{"abcd|e", 1}};
    static constexpr ExpectedRow lateQuote[] = {{"a\"b|c", 1}};
    static constexpr ExpectedRow closedThenQuote[] = {{"abc\"d", 1}};
    static constexpr ExpectedRow unterminated[] = {{"a|b,c\nd", 1}};
    static constexpr ExpectedRow lineEnds[] = {{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}};
    static constexpr ExpectedRow trailingComma[] = {{"a|b|", 1}, {"c|", 2}};
    static constexpr ExpectedRow quotedNewline[] = {{"x\ny|z", 1}, {"w", 3}};
    static constexpr ExpectedRow onlyQuotes[] = {{"", 1}, {"\"", 2}};
    static constexpr ExpectedRow nulEnds[] = {{"a|b", 1}};
    static constexpr ExpectedRow emptyCells[] = {{"||", 1}};
    static constexpr ExpectedRow quotedCrLf[] = {{"a\r\nb|c", 1}, {"d", 3}};
    static constexpr ExpectedRow quotedCr[] = {{"a\rb", 1}, {"e", 3}};
    static constexpr ExpectedRow nulFirst[] = {{"", 1}};
    const CsvCase cases[] = {
        {"simple", "a,b,c\n1,2,3\n", simple},
        {"blankLines", "\na\n\nb\n\n", blankLines},
        {"spaceLine", "a\n  \nb", spaceLine},
        {"quoted", "\"a,b\",c", quoted},
        {"escaped", "\"say \"\"hi\"\"\",x", escaped},
        {"afterClose", "\"ab\"cd,e", afterClose},
        {"lateQuote", "a\"b,c", lateQuote},
        {"closedThenQuote", "\"ab\"c\"d", closedThenQuote},
        {"unterminated", "a,\"b,c\nd", unterminated},
        {"lineEnds", "a\r\nb\rc\nd", lineEnds},
        {"trailingComma", "a,b,\nc,", trailingComma},
        {"quotedNewline", "\"x\ny\",z\nw", quotedNewline},
        {"onlyQuotes", "\"\"\n\"\"\"\"", onlyQuotes},
        {"nulEnds", StringView("a,b\0c,d", 7), nulEnds},
        {"emptyCells", ",,", emptyCells},
        // 따옴표 안의 CRLF는 줄 하나, CR 단독도 줄 하나로 센다.
        {"quotedCrLf", "\"a\r\nb\",c\nd", quotedCrLf},
        {"quotedCr", "\"a\rb\"\ne", quotedCr},
        // 비지 않은 입력은 첫 byte가 NUL이어도 빈 칸 하나짜리 행이다(UE와 같다).
        {"nulFirst", StringView("\0a", 2), nulFirst},
    };
    usize failures = 0;
    for (const CsvCase& item : cases) {
        Array<CsvRow> rows(allocator);
        REQUIRE(TryParseCsv(item.text, rows));
        if (!Matches(rows, item.rows, item.name)) {
            ++failures;
        }
    }

    CHECK(failures == 0);

    // 빈 문자열과 줄 끝뿐인 문자열은 행이 없다.
    for (const StringView text : {StringView(), StringView("\n"), StringView("\r\n\r\n")}) {
        Array<CsvRow> rows(allocator);
        REQUIRE(TryParseCsv(text, rows));
        CHECK(rows.Size() == 0);
    }
}

// 붙이기: out에 이미 있는 행 뒤에 붙고, 할당이 실패하면 out은 그대로다.
TEST(Csv_AppendAndAllocationFailure) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    Array<CsvRow> rows(failing);
    REQUIRE(TryParseCsv("a,b\n", rows));
    REQUIRE(rows.Size() == 1);
    usize sawFailure = 0;
    for (uint64 attempt = 1; attempt <= 32; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto parsed = TryParseCsv("c,\"d\"\"e\"\nf", rows);
        failing.DisableFailure();
        if (!parsed) {
            ++sawFailure;
            CHECK(parsed.Error() == ErrorCode::OutOfMemory);
            CHECK(rows.Size() == 1 && rows[0].cells.Size() == 2);
        } else {
            CHECK(rows.Size() == 3 && rows[1].cells[1].View() == StringView("d\"e"));
            break;
        }
    }

    CHECK(sawFailure > 0);
}

} // namespace csv_tests_file
