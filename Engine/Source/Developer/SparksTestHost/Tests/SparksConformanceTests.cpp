#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <SparksTestHost/Conformance.h>
#include <TestSupport/Test.h>

// Sparks conformance 실행기 시험이다(Docs/Design/SparksLanguage.ko.md 13.1절 "`.expect` 형식"):
// 기대값 해석과 형식 오류, 사례 일치·불일치 보고, 그리고 저장소 corpus 전체 실행.
using namespace rocks;
using namespace rocks::sparks;
using namespace rocks::sparks::conformance;
namespace sparks_conformance_tests_file {

namespace {

// 실패 항목을 시험 출력에 보인다(corpus 실패의 원인을 바로 볼 수 있게).
void PrintFailures(const Report& report) noexcept {
    for (const String& failure : report.Failures()) {
        platform::WriteDiagnostic(failure.Data(), failure.ByteCount());
        platform::WriteDiagnostic("\n", 1);
    }
}

bool ParseFails(StringView text, StringView reason, Allocator& allocator) noexcept {
    Expectation expectation(allocator);
    String problem(allocator);
    auto parsed = TryParseExpectation(text, expectation, problem);
    return parsed && !parsed.Value() && problem.View() == reason;
}

} // namespace

TEST(SparksConformance_ParsesExpectations) {
    SystemAllocator allocator;
    {
        Expectation expectation(allocator);
        String problem(allocator);
        auto parsed = TryParseExpectation(
            "# comment\n\nerror S0101 2:3\r\n  error S0103  \nerror S0102 1:1\n", expectation,
            problem);
        REQUIRE(parsed && parsed.Value());
        CHECK(!expectation.IsClean());
        REQUIRE(expectation.Diagnostics().Size() == 3);
        CHECK(expectation.Diagnostics()[0].code == 101);
        CHECK(expectation.Diagnostics()[0].line == 2 && expectation.Diagnostics()[0].column == 3);
        CHECK(expectation.Diagnostics()[1].code == 103 && expectation.Diagnostics()[1].line == 0);
        CHECK(expectation.Diagnostics()[2].code == 102);

        auto clean = TryParseExpectation("clean\n", expectation, problem);
        REQUIRE(clean && clean.Value());
        CHECK(expectation.IsClean());
        CHECK(expectation.Diagnostics().IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 빈 기대값이 조용히 통과하지 않도록 형식 오류를 거부한다.
TEST(SparksConformance_RejectsMalformedExpectations) {
    SystemAllocator allocator;
    {
        CHECK(ParseFails(
            "", "line 0: the file has no directive; write 'clean' when no diagnostic is expected",
            allocator));
        CHECK(ParseFails(
            "# only a comment\n",
            "line 0: the file has no directive; write 'clean' when no diagnostic is expected",
            allocator));
        CHECK(ParseFails("clean\nerror S0101 1:1\n",
                         "line 0: 'clean' cannot be combined with 'error'", allocator));
        CHECK(ParseFails("clean\nclean\n", "line 2: 'clean' appears twice", allocator));
        CHECK(ParseFails("warning S0101 1:1\n",
                         "line 1: unknown directive (expected 'error' or 'clean')", allocator));
        CHECK(ParseFails("error 0101 1:1\n", "line 1: diagnostic code must look like S0412",
                         allocator));
        CHECK(ParseFails("error S9999 1:1\n", "line 1: unknown diagnostic code", allocator));
        CHECK(ParseFails("error S0101 0:1\n",
                         "line 1: position must look like 14:7 (line and column from 1)",
                         allocator));
        CHECK(ParseFails("error S0101 1\n",
                         "line 1: position must look like 14:7 (line and column from 1)",
                         allocator));
        CHECK(ParseFails("error S0101 1:1 extra\n",
                         "line 1: unknown directive (expected 'error' or 'clean')", allocator));
        CHECK(ParseFails("clean extra\n", "line 1: unknown directive (expected 'error' or 'clean')",
                         allocator));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksConformance_ReportsMatchesAndMismatches) {
    SystemAllocator allocator;
    {
        Report report(allocator);
        REQUIRE(RunCase(allocator, "Area/Clean.spark", "let x = 1;\n", "clean\n", report));
        REQUIRE(RunCase(allocator, "Area/Cr.spark", "a\rb", "error S0102 1:2\n", report));
        REQUIRE(RunCase(allocator, "Area/Big.spark", "x", "error S0103\n", report));
        REQUIRE(RunCase(allocator, "Area/Wrong.spark", "a\rb", "clean\n", report));
        REQUIRE(RunCase(allocator, "Area/BadExpect.spark", "x", "", report));
        CHECK(report.CasesRun() == 5);
        REQUIRE(report.Failures().Size() == 3);
        CHECK(report.Failures()[0].View() ==
              StringView("Area/Big.spark: diagnostics differ from .expect\n"
                         "  expected:\n"
                         "    error S0103\n"
                         "  actual:\n"
                         "    (no diagnostics)\n"));
        CHECK(
            report.Failures()[1].View() ==
            StringView("Area/Wrong.spark: diagnostics differ from .expect\n"
                       "  expected:\n"
                       "    clean\n"
                       "  actual:\n"
                       "Area/Wrong.spark:1:2: error S0102: Carriage return without a line feed.\n"
                       "    a b\n"
                       "     ^\n"
                       "  Some editors treat a lone CR as a line break and others do not, so line "
                       "numbers would disagree.\n"
                       "  fix: Use LF or CRLF line endings.\n"));
        CHECK(
            report.Failures()[2].View() ==
            StringView("Area/BadExpect.spark: invalid .expect: line 0: the file has no directive; "
                       "write 'clean' when no diagnostic is expected"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 저장소의 corpus(Engine/Source/Developer/SparksTestHost/Conformance)를 모두 실행한다.
TEST(SparksConformance_RepositoryCorpusPasses) {
    SystemAllocator allocator;
    {
        constexpr usize PathCapacity = 4096;
        char engine[PathCapacity];
        auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
        REQUIRE(found);
        auto corpus = Path::Combine(allocator, StringView(engine, found.Value()),
                                    "Source/Developer/SparksTestHost/Conformance");
        REQUIRE(corpus);
        Report report(allocator);
        REQUIRE(
            RunCorpus(allocator, platform::PhysicalFileSystem(), corpus.Value().View(), report));
        PrintFailures(report);
        CHECK(report.Failures().IsEmpty());
        CHECK(report.CasesRun() > 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// corpus 구조 오류(짝 없는 파일, 낯선 파일, 영역 밖 파일)를 실패로 기록한다.
TEST(SparksConformance_ReportsCorpusLayoutProblems) {
    SystemAllocator allocator;
    {
        platform::FileSystem& files = platform::PhysicalFileSystem();
        // 다른 시험과 같이 현재 폴더 아래에 프로세스·시각으로 구분한 임시 corpus를 만든다.
        StringBuilder<64> root(allocator);
        REQUIRE(root.TryAppendFormat("rocks-sparks-conformance-{}-{}", platform::CurrentProcessId(),
                                     platform::MonotonicTicks()));
        const StringView rootPath = root.View();
        auto area = Path::Combine(allocator, rootPath, "Area");
        REQUIRE(area);
        REQUIRE(platform::MakeDirectoryTree(files, area.Value().Data(), area.Value().ByteCount()));

        auto write = [&](StringView directory, StringView name,
                         StringView content) noexcept -> bool {
            auto path = Path::Combine(allocator, directory, name);
            if (!path) {
                return false;
            }

            platform::File file;
            if (!file.Open(files, path.Value().Data(), path.Value().ByteCount(),
                           platform::FileOpenMode::CreateAlways)) {
                return false;
            }

            const platform::FileTransfer written =
                file.WriteAll(content.Data(), content.ByteCount());
            const bool closed = static_cast<bool>(file.Close());
            return static_cast<bool>(written.status) && closed;
        };
        REQUIRE(write(area.Value().View(), "Good.spark", "x\n"));
        REQUIRE(write(area.Value().View(), "Good.expect", "clean\n"));
        REQUIRE(write(area.Value().View(), "NoExpect.spark", "x\n"));
        REQUIRE(write(area.Value().View(), "NoSource.expect", "clean\n"));
        REQUIRE(write(area.Value().View(), "Notes.txt", "x"));
        REQUIRE(write(rootPath, "Stray.spark", "x"));

        Report report(allocator);
        REQUIRE(RunCorpus(allocator, files, rootPath, report));
        CHECK(report.CasesRun() == 1);
        REQUIRE(report.Failures().Size() == 4);
        CHECK(report.Failures()[0].View() ==
              StringView("Area/NoExpect.spark: missing the .expect file"));
        CHECK(report.Failures()[1].View() ==
              StringView("Area/NoSource.expect: missing the .spark file"));
        CHECK(report.Failures()[2].View() ==
              StringView("Area/Notes.txt: unexpected file (only .spark and .expect)"));
        CHECK(report.Failures()[3].View() ==
              StringView(
                  "Stray.spark: unexpected file at the corpus root (cases live in area folders)"));

        Report missing(allocator);
        auto absent = Path::Combine(allocator, rootPath, "Absent");
        REQUIRE(absent);
        auto ran = RunCorpus(allocator, files, absent.Value().View(), missing);
        CHECK(!ran && ran.Error() == ErrorCode::NotFound);

        CHECK(platform::DeleteDirectoryTree(files, rootPath.Data(), rootPath.ByteCount()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace sparks_conformance_tests_file
