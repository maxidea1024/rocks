#include <Core/Memory/SystemAllocator.h>
#include <Core/Containers/Array.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// Sparks 원문 관리 시험이다(Docs/Design/SparksLanguage.ko.md 8.1절 "위치"·"원문 검사"): 줄·열 계산
// (code point 단위, BOM, CRLF), 원문 검사 S0101·S0102·S0103과 그때의 IsUsable, 실패 원자성.
// CRLF·잘못된 UTF-8·BOM은 저장소가 줄 끝을 정규화하므로 corpus 파일이 아니라 여기서 시험한다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_source_tests_file {

TEST(SparksSource_LocatesLinesAndCodePointColumns) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        // 둘째 줄 앞의 한글 두 글자는 6 byte지만 열은 2만큼 늘어난다.
        const StringView text("let a = 1;\n\xED\x95\x9C\xEA\xB8\x80 x\n");
        auto id = sources.TryAdd("Test.spark", text, diagnostics);
        REQUIRE(id);
        CHECK(diagnostics.IsEmpty());
        const SourceFile& file = sources.Get(id.Value());
        CHECK(file.IsUsable());
        CHECK(file.ContentBegin() == 0);
        CHECK(file.LineCount() == 3);
        CHECK(file.LineText(1) == StringView("let a = 1;"));
        CHECK(file.LineText(2) == StringView("\xED\x95\x9C\xEA\xB8\x80 x"));
        CHECK(file.LineText(3).IsEmpty());

        const SourceLocation first = file.Locate(0);
        CHECK(first.line == 1 && first.column == 1);
        const SourceLocation semicolon = file.Locate(9);
        CHECK(semicolon.line == 1 && semicolon.column == 10);
        const SourceLocation lineEnd = file.Locate(10); // LF 자체는 그 줄의 끝 열이다.
        CHECK(lineEnd.line == 1 && lineEnd.column == 11);
        const SourceLocation x = file.Locate(11 + 7); // 한글 6 byte + 공백 뒤
        CHECK(x.line == 2 && x.column == 4);
        const SourceLocation end = file.Locate(static_cast<uint32>(text.ByteCount()));
        CHECK(end.line == 3 && end.column == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksSource_EmptyTextHasOneLine) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("Empty.spark", "", diagnostics);
        REQUIRE(id);
        CHECK(diagnostics.IsEmpty());
        const SourceFile& file = sources.Get(id.Value());
        CHECK(file.IsUsable());
        CHECK(file.LineCount() == 1);
        CHECK(file.LineText(1).IsEmpty());
        const SourceLocation location = file.Locate(0);
        CHECK(location.line == 1 && location.column == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// BOM은 허용하고 무시한다. 첫 줄의 열은 BOM 뒤부터 센다.
TEST(SparksSource_SkipsLeadingBom) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("Bom.spark", "\xEF\xBB\xBFvar x;", diagnostics);
        REQUIRE(id);
        CHECK(diagnostics.IsEmpty());
        const SourceFile& file = sources.Get(id.Value());
        CHECK(file.IsUsable());
        CHECK(file.ContentBegin() == Utf8BomBytes);
        CHECK(file.LineText(1) == StringView("var x;"));
        const SourceLocation v = file.Locate(static_cast<uint32>(Utf8BomBytes));
        CHECK(v.line == 1 && v.column == 1);
        const SourceLocation x = file.Locate(static_cast<uint32>(Utf8BomBytes) + 4);
        CHECK(x.line == 1 && x.column == 5);
        const SourceLocation insideBom = file.Locate(1);
        CHECK(insideBom.line == 1 && insideBom.column == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// CRLF의 CR은 줄 끝의 일부다(줄 내용에서 빠진다).
TEST(SparksSource_TreatsCrLfAsLineEnd) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("CrLf.spark", "a\r\nbc\r\n", diagnostics);
        REQUIRE(id);
        CHECK(diagnostics.IsEmpty());
        const SourceFile& file = sources.Get(id.Value());
        CHECK(file.LineCount() == 3);
        CHECK(file.LineText(1) == StringView("a"));
        CHECK(file.LineText(2) == StringView("bc"));
        const SourceLocation c = file.Locate(4);
        CHECK(c.line == 2 && c.column == 2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// LF가 뒤따르지 않는 CR은 S0102다. 처음 하나만 내고, 뒤 단계는 막지 않는다. 마지막 byte의 CR도
// 같다.
TEST(SparksSource_ReportsFirstLoneCarriageReturnOnly) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("Cr.spark", "ab\rcd\r\nef\rgh\r", diagnostics);
        REQUIRE(id);
        REQUIRE(diagnostics.Count() == 1);
        const Diagnostic& diagnostic = diagnostics[0];
        CHECK(diagnostic.Code() == DiagnosticCode::LoneCarriageReturn);
        CHECK(diagnostic.HasSpan());
        CHECK(diagnostic.Span().begin == 2 && diagnostic.Span().end == 3);
        const SourceFile& file = sources.Get(id.Value());
        CHECK(file.IsUsable());
        // 홀로 선 CR은 줄을 나누지 않는다.
        CHECK(file.LineCount() == 2);
        const SourceLocation location = file.Locate(diagnostic.Span().begin);
        CHECK(location.line == 1 && location.column == 3);

        DiagnosticList trailing(allocator);
        auto last = sources.TryAdd("TrailingCr.spark", "x\r", trailing);
        REQUIRE(last);
        REQUIRE(trailing.Count() == 1);
        CHECK(trailing[0].Code() == DiagnosticCode::LoneCarriageReturn);
        CHECK(trailing[0].Span().begin == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// UTF-8이 아니면 S0101 하나(처음 잘못된 sequence의 첫 byte)이고 뒤 단계를 막는다. 이때 CR 검사는
// 하지 않는다(원인 하나만 보인다).
TEST(SparksSource_ReportsFirstInvalidUtf8AndBlocksLaterStages) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("Bad.spark", "ok\n\xED\x95\x9C\xE1\x80Z\r\xFF", diagnostics);
        REQUIRE(id);
        REQUIRE(diagnostics.Count() == 1);
        const Diagnostic& diagnostic = diagnostics[0];
        CHECK(diagnostic.Code() == DiagnosticCode::InvalidUtf8);
        CHECK(diagnostic.Span().begin == 6);
        CHECK(diagnostic.Explanation() ==
              StringView("The byte 0xE1 at this position does not start a valid UTF-8 sequence."));
        const SourceFile& file = sources.Get(id.Value());
        CHECK(!file.IsUsable());
        const SourceLocation location = file.Locate(diagnostic.Span().begin);
        CHECK(location.line == 2 && location.column == 2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 16 MiB를 넘으면 S0103(파일 전체 진단)이고 원문을 복사하지 않는다. 정확히 16 MiB는 통과한다.
TEST(SparksSource_RejectsSourcesLargerThanTheLimit) {
    SystemAllocator allocator;
    {
        Array<char> big(allocator);
        REQUIRE(big.TryResize(MaxSourceBytes + 1));
        for (usize index = 0; index < big.Size(); ++index) {
            big[index] = 'a';
        }

        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto atLimit =
            sources.TryAdd("Limit.spark", StringView(big.Data(), MaxSourceBytes), diagnostics);
        REQUIRE(atLimit);
        CHECK(diagnostics.IsEmpty());
        CHECK(sources.Get(atLimit.Value()).IsUsable());

        auto tooLarge =
            sources.TryAdd("Big.spark", StringView(big.Data(), big.Size()), diagnostics);
        REQUIRE(tooLarge);
        REQUIRE(diagnostics.Count() == 1);
        CHECK(diagnostics[0].Code() == DiagnosticCode::SourceTooLarge);
        CHECK(!diagnostics[0].HasSpan());
        CHECK(diagnostics[0].Explanation() ==
              StringView("The file has 16777217 bytes; the limit is 16777216 bytes."));
        const SourceFile& file = sources.Get(tooLarge.Value());
        CHECK(!file.IsUsable());
        CHECK(file.Text().IsEmpty());
        CHECK(file.LineCount() == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 경로가 UTF-8이 아니면 InvalidArgument이고 아무것도 바뀌지 않는다.
TEST(SparksSource_RejectsNonUtf8PathWithoutChanges) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("\xFF.spark", "\xFF", diagnostics);
        CHECK(!id);
        CHECK(id.Error() == ErrorCode::InvalidArgument);
        CHECK(sources.Count() == 0);
        CHECK(diagnostics.IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 할당이 어디서 실패하든 SourceSet과 진단 목록은 그대로다(진단이 붙는 경로 포함).
TEST(SparksSource_AddIsAtomicUnderAllocationFailure) {
    SystemAllocator backing;
    const StringView text("a\rb\n\xED\x95\x9C");
    for (uint64 failAt = 1;; ++failAt) {
        test::NFailAllocator allocator(backing);
        allocator.FailOnAttempt(failAt);
        bool succeeded = false;
        {
            SourceSet sources(allocator);
            DiagnosticList diagnostics(allocator);
            auto id = sources.TryAdd("Atomic.spark", text, diagnostics);
            if (id) {
                succeeded = true;
                CHECK(sources.Count() == 1);
                CHECK(diagnostics.Count() == 1);
            } else {
                CHECK(id.Error() == ErrorCode::OutOfMemory);
                CHECK(sources.Count() == 0);
                CHECK(diagnostics.IsEmpty());
            }
        }

        CHECK(backing.Stats().liveAllocations == 0);
        if (succeeded) {
            break;
        }
    }
}

// 원문 경계: BOM만 있는 원문, BOM 바로 뒤·줄 처음·LF 바로 앞의 잘못된 byte.
TEST(SparksSource_BoundaryPositions) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto bomOnly = sources.TryAdd("BomOnly.spark", "\xEF\xBB\xBF", diagnostics);
        REQUIRE(bomOnly);
        const SourceFile& bomFile = sources.Get(bomOnly.Value());
        CHECK(bomFile.IsUsable());
        CHECK(bomFile.LineCount() == 1);
        CHECK(bomFile.LineText(1).IsEmpty());
        const SourceLocation bomEnd = bomFile.Locate(static_cast<uint32>(Utf8BomBytes));
        CHECK(bomEnd.line == 1 && bomEnd.column == 1);
        CHECK(diagnostics.IsEmpty());

        struct Case {
            StringView text;
            uint32 offset;
            uint32 line;
            uint32 column;
        };
        const Case cases[] = {
            {StringView("\xEF\xBB\xBF\xFF"), 3, 1, 1}, // BOM 바로 뒤
            {StringView("\xFF\nx"), 0, 1, 1},          // 줄 처음
            {StringView("a\xFF\n"), 1, 1, 2},          // LF 바로 앞
            {StringView("ok\n\xC3"), 3, 2, 1},         // 파일 끝에서 잘린 sequence
        };
        for (const Case& item : cases) {
            DiagnosticList invalid(allocator);
            auto id = sources.TryAdd("Invalid.spark", item.text, invalid);
            REQUIRE(id);
            REQUIRE(invalid.Count() == 1);
            CHECK(invalid[0].Code() == DiagnosticCode::InvalidUtf8);
            CHECK(invalid[0].Span().begin == item.offset);
            const SourceLocation location = sources.Get(id.Value()).Locate(item.offset);
            CHECK(location.line == item.line && location.column == item.column);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// CRLF의 CR·LF 위치는 LF만 쓴 파일의 LF 위치와 같은 열(줄 끝)이다. BOM과 CRLF가 함께 있어도 같다.
// 홀로 선 CR이 있는 줄의 내용에는 그 CR이 남는다(줄을 나누지 않는다).
TEST(SparksSource_LineEndColumnsMatchAcrossLineEndings) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto lf = sources.TryAdd("Lf.spark", "ab\ncd", diagnostics);
        auto crlf = sources.TryAdd("CrLf.spark",
                                   "\xEF\xBB\xBF"
                                   "ab\r\ncd",
                                   diagnostics);
        REQUIRE(lf && crlf);
        CHECK(diagnostics.IsEmpty());
        const SourceLocation lfEnd = sources.Get(lf.Value()).Locate(2);
        CHECK(lfEnd.line == 1 && lfEnd.column == 3);
        const SourceFile& crlfFile = sources.Get(crlf.Value());
        const SourceLocation atCr = crlfFile.Locate(static_cast<uint32>(Utf8BomBytes) + 2);
        const SourceLocation atLf = crlfFile.Locate(static_cast<uint32>(Utf8BomBytes) + 3);
        CHECK(atCr.line == 1 && atCr.column == 3);
        CHECK(atLf.line == 1 && atLf.column == 3);
        CHECK(crlfFile.LineText(1) == StringView("ab"));
        const SourceLocation c = crlfFile.Locate(static_cast<uint32>(Utf8BomBytes) + 4);
        CHECK(c.line == 2 && c.column == 1);

        DiagnosticList loneCr(allocator);
        auto lone = sources.TryAdd("Lone.spark", "a\rb\nc", loneCr);
        REQUIRE(lone);
        CHECK(sources.Get(lone.Value()).LineText(1) == StringView("a\rb"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 이미 파일과 진단이 있는 상태에서 실패를 주입해도 기존 내용은 그대로이고, 이후 성공한 진단은 앞
// 진단 뒤에 정렬된다(실패가 순서 번호를 소비해도 순서가 뒤집히지 않는다).
TEST(SparksSource_FailedAddKeepsExistingContents) {
    SystemAllocator backing;
    for (uint64 failAt = 1;; ++failAt) {
        test::NFailAllocator allocator(backing);
        bool succeeded = false;
        {
            SourceSet sources(allocator);
            DiagnosticList diagnostics(allocator);
            REQUIRE(sources.TryAdd("First.spark", "a\rb", diagnostics));
            REQUIRE(diagnostics.Count() == 1);
            allocator.FailOnAttempt(failAt);
            auto second = sources.TryAdd("First.spark", "c\rd", diagnostics);
            allocator.DisableFailure();
            if (second) {
                succeeded = true;
                CHECK(sources.Count() == 2);
                REQUIRE(diagnostics.Count() == 2);
                diagnostics.Sort(sources);
                CHECK(diagnostics[0].Source() == SourceId{0});
                CHECK(diagnostics[1].Source() == SourceId{1});
            } else {
                CHECK(sources.Count() == 1);
                CHECK(diagnostics.Count() == 1);
                CHECK(sources.Get(SourceId{0}).Path() == StringView("First.spark"));
            }
        }

        CHECK(backing.Stats().liveAllocations == 0);
        if (succeeded) {
            break;
        }
    }
}

} // namespace sparks_source_tests_file
