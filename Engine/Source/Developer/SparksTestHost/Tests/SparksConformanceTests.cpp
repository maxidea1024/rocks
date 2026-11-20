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

// 구문 트리 golden 없이 사례 하나를 돈다.
Result<void> Run(Allocator& allocator, StringView name, StringView source, StringView expect,
                 Report& report) noexcept {
    CaseInput input;
    input.name = name;
    input.source = source;
    input.expect = expect;
    return RunCase(allocator, input, report);
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
        REQUIRE(Run(allocator, "Area/Clean.spark", "const X: int = 1;\n", "clean\n", report));
        REQUIRE(
            Run(allocator, "Area/Cr.spark", "const A: int = 1;\r", "error S0102 1:18\n", report));
        REQUIRE(Run(allocator, "Area/Big.spark", "", "error S0103\n", report));
        REQUIRE(Run(allocator, "Area/Wrong.spark", "const A: int = 1;\r", "clean\n", report));
        REQUIRE(Run(allocator, "Area/BadExpect.spark", "x", "", report));
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
                       "Area/Wrong.spark:1:18: error S0102: Carriage return without a line "
                       "feed.\n"
                       "    const A: int = 1; \n"
                       "                     ^\n"
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
        REQUIRE(write(area.Value().View(), "Good.spark", ""));
        REQUIRE(write(area.Value().View(), "Good.expect", "clean\n"));
        REQUIRE(write(area.Value().View(), "Good.ast",
                      "File @1:1-1:1\n  List @1:1-1:1\n  List @1:1-1:1\n"));
        REQUIRE(write(area.Value().View(), "NoExpect.spark", ""));
        REQUIRE(write(area.Value().View(), "NoExpect.ast", ""));
        REQUIRE(write(area.Value().View(), "NoSource.expect", "clean\n"));
        REQUIRE(write(area.Value().View(), "NoSource.ast", ""));
        REQUIRE(write(area.Value().View(), "OnlyTree.ast", ""));
        REQUIRE(write(area.Value().View(), "Notes.txt", "x"));
        REQUIRE(write(rootPath, "Stray.spark", "x"));

        Report report(allocator);
        REQUIRE(RunCorpus(allocator, files, rootPath, report));
        PrintFailures(report);
        CHECK(report.CasesRun() == 1);
        REQUIRE(report.Failures().Size() == 6);
        // 낯선 파일은 목록을 훑을 때, 짝 없는 파일은 stem 순으로 맞춰 볼 때 기록한다. 같은 stem의
        // 짝 없는 파일은 .expect, .ast 순이다.
        CHECK(report.Failures()[0].View() ==
              StringView("Area/Notes.txt: unexpected file (only .spark, .expect and .ast)"));
        CHECK(report.Failures()[1].View() ==
              StringView("Area/NoExpect.spark: missing the .expect file"));
        CHECK(report.Failures()[2].View() ==
              StringView("Area/NoSource.expect: missing the .spark file"));
        CHECK(report.Failures()[3].View() ==
              StringView("Area/NoSource.ast: missing the .spark file"));
        CHECK(report.Failures()[4].View() ==
              StringView("Area/OnlyTree.ast: missing the .spark file"));
        CHECK(report.Failures()[5].View() ==
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

// 사례 이름에 점이 있어도 stem으로 짝을 맞춘다("Bom.expect" < "Bom.lf.expect" < "Bom.lf.spark" <
// "Bom.spark" 순서라 이웃한 이름으로 맞추면 틀린다). 영역 안의 폴더와 파일인 corpus 경로도
// 시험한다.
TEST(SparksConformance_PairsCasesByStemAndRejectsOddLayouts) {
    SystemAllocator allocator;
    {
        platform::FileSystem& files = platform::PhysicalFileSystem();
        StringBuilder<64> root(allocator);
        REQUIRE(root.TryAppendFormat("rocks-sparks-stem-{}-{}", platform::CurrentProcessId(),
                                     platform::MonotonicTicks()));
        const StringView rootPath = root.View();
        auto area = Path::Combine(allocator, rootPath, "Area");
        REQUIRE(area);
        auto nested = Path::Combine(allocator, area.Value().View(), "Nested");
        REQUIRE(nested);
        REQUIRE(
            platform::MakeDirectoryTree(files, nested.Value().Data(), nested.Value().ByteCount()));
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
        REQUIRE(write(area.Value().View(), "Bom.spark", "\n"));
        REQUIRE(write(area.Value().View(), "Bom.expect", "clean\n"));
        REQUIRE(write(area.Value().View(), "Bom.lf.spark", "\n\n"));
        REQUIRE(write(area.Value().View(), "Bom.lf.expect", "clean\n"));
        // golden도 stem으로 짝을 맞춘다(Bom.lf.ast는 Bom.lf 사례의 것이다).
        REQUIRE(write(area.Value().View(), "Bom.lf.ast",
                      "File @1:1-3:1\n  List @3:1-3:1\n  List @3:1-3:1\n"));

        Report report(allocator);
        REQUIRE(RunCorpus(allocator, files, rootPath, report));
        CHECK(report.CasesRun() == 2);
        REQUIRE(report.Failures().Size() == 1);
        CHECK(report.Failures()[0].View() ==
              StringView("Area/Nested: unexpected folder in a corpus area"));

        // corpus 경로가 폴더가 아니면 실행기 오류다.
        auto notFolder = Path::Combine(allocator, area.Value().View(), "Bom.spark");
        REQUIRE(notFolder);
        Report fileRoot(allocator);
        auto ran = RunCorpus(allocator, files, notFolder.Value().View(), fileRoot);
        CHECK(!ran && ran.Error() == ErrorCode::InvalidArgument);

        CHECK(platform::DeleteDirectoryTree(files, rootPath.Data(), rootPath.ByteCount()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// `.ast`가 있으면 구문 트리 덤프와 byte 단위로 비교한다. 다르면 첫 다른 줄과 실제 트리 전체를
// 보인다. 원문 검사가 막아 트리가 없으면 그것도 실패다. 진단과 트리는 따로 판정한다.
TEST(SparksConformance_ComparesSyntaxTreeGolden) {
    SystemAllocator allocator;
    {
        constexpr StringView Tree = "File @1:1-2:1\n"
                                    "  List @1:1-1:1\n"
                                    "  List @1:1-1:18\n"
                                    "    Const @1:1-1:18\n"
                                    "      -\n"
                                    "      List @1:1-1:1\n"
                                    "      Name \"X\" @1:7-1:8\n"
                                    "      Type @1:10-1:13\n"
                                    "        Path @1:10-1:13\n"
                                    "          Name \"int\" @1:10-1:13\n"
                                    "        -\n"
                                    "      IntegerLiteral \"1\" @1:16-1:17\n";
        CaseInput input;
        input.name = "Area/Tree.spark";
        input.source = "const X: int = 1;\n";
        input.expect = "clean\n";
        input.hasSyntaxTree = true;
        input.syntaxTree = Tree;
        Report report(allocator);
        REQUIRE(RunCase(allocator, input, report));
        PrintFailures(report);
        CHECK(report.Failures().IsEmpty());

        // 끝의 LF 하나만 달라도 실패다.
        input.syntaxTree = StringView(Tree.Data(), Tree.ByteCount() - 1);
        REQUIRE(RunCase(allocator, input, report));
        REQUIRE(report.Failures().Size() == 1);
        CHECK(report.Failures()[0].View().StartsWith(
            "Area/Tree.spark: syntax tree differs from .ast at line 13\n"
            "  expected: \n"
            "  actual:   \n"
            "  actual tree:\nFile @1:1-2:1\n"));

        input.syntaxTree = "File @1:1-2:1\n  List @1:1-1:2\n";
        REQUIRE(RunCase(allocator, input, report));
        REQUIRE(report.Failures().Size() == 2);
        CHECK(report.Failures()[1].View().StartsWith(
            "Area/Tree.spark: syntax tree differs from .ast at line 2\n"
            "  expected:   List @1:1-1:2\n"
            "  actual:     List @1:1-1:1\n"));

        // 진단이 틀리고 트리도 틀리면 실패 항목이 둘이다.
        input.expect = "error S0201 1:1\n";
        REQUIRE(RunCase(allocator, input, report));
        CHECK(report.Failures().Size() == 4);

        // 원문 검사가 막으면(S0101) 트리가 없다.
        CaseInput blocked;
        blocked.name = "Area/Blocked.spark";
        blocked.source = "\xFF";
        blocked.expect = "error S0101 1:1\n";
        blocked.hasSyntaxTree = true;
        blocked.syntaxTree = Tree;
        Report blockedReport(allocator);
        REQUIRE(RunCase(allocator, blocked, blockedReport));
        REQUIRE(blockedReport.Failures().Size() == 1);
        CHECK(blockedReport.Failures()[0].View() ==
              StringView("Area/Blocked.spark: has a .ast file but the source check stopped "
                         "before parsing"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 진단 순서도 비교 대상이다(8.1절 순서). 같은 진단을 다른 순서로 적으면 실패다.
TEST(SparksConformance_OrderMatters) {
    SystemAllocator allocator;
    {
        Report report(allocator);
        // `#`는 Invalid 토큰으로 남고 구문 분석은 그 자리에서 진단을 더 내지 않는다.
        constexpr StringView Source = "const A: int = 1 # 2;\nconst B: int = 3 # 4;\n";
        REQUIRE(Run(allocator, "Area/Order.spark", Source, "error S0104 2:18\nerror S0104 1:18\n",
                    report));
        REQUIRE(report.Failures().Size() == 1);
        REQUIRE(Run(allocator, "Area/Order.spark", Source, "error S0104 1:18\nerror S0104 2:18\n",
                    report));
        CHECK(report.Failures().Size() == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// `.expect`는 처음의 BOM을 무시하고 tab을 단어 구분으로 받는다. 숫자는 0–9만 받으며 UTF-8이
// 아니면 형식 오류다.
TEST(SparksConformance_ExpectationEncodingAndNumbers) {
    SystemAllocator allocator;
    {
        Expectation expectation(allocator);
        String problem(allocator);
        auto bomTabs = TryParseExpectation("\xEF\xBB\xBF"
                                           "error\tS0101\t2:3\n",
                                           expectation, problem);
        REQUIRE(bomTabs && bomTabs.Value());
        REQUIRE(expectation.Diagnostics().Size() == 1);
        CHECK(expectation.Diagnostics()[0].line == 2 && expectation.Diagnostics()[0].column == 3);

        CHECK(ParseFails("error S+101 1:1\n", "line 1: diagnostic code must look like S0412",
                         allocator));
        CHECK(ParseFails("error S0101 +1:+1\n",
                         "line 1: position must look like 14:7 (line and column from 1)",
                         allocator));
        CHECK(ParseFails("clean\n\xFF\n", "line 0: the file is not valid UTF-8", allocator));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace sparks_conformance_tests_file
