#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <SparksTestHost/Conformance.h>
#include <TestSupport/NFailAllocator.h>
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
                         "line 1: unknown directive (expected 'error', 'clean' or 'stage')",
                         allocator));
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
                         "line 1: the file path comes before the position", allocator));
        CHECK(ParseFails("error S0101 A.spark 1:1 extra\n",
                         "line 1: unknown directive (expected 'error', 'clean' or 'stage')",
                         allocator));
        CHECK(ParseFails("error S0101 A.spark 1\n",
                         "line 1: position must look like 14:7 (line and column from 1)",
                         allocator));
        CHECK(ParseFails("clean extra\n",
                         "line 1: unknown directive (expected 'error', 'clean' or 'stage')",
                         allocator));
        // stage: 한 번, 알려진 이름만, 그것만으로는 기대 지시가 아니다.
        CHECK(ParseFails(
            "stage syntax\n",
            "line 0: the file has no directive; write 'clean' when no diagnostic is expected",
            allocator));
        CHECK(ParseFails("stage values\nclean\n",
                         "line 1: unknown stage (expected 'syntax', 'imports', 'names', "
                         "'types', 'flow', 'rules' or 'codegen')",
                         allocator));
        CHECK(ParseFails("stage syntax\nstage names\nclean\n", "line 2: 'stage' appears twice",
                         allocator));
        CHECK(ParseFails("stage\nclean\n",
                         "line 1: unknown directive (expected 'error', 'clean' or 'stage')",
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
        auto schema = Path::Combine(allocator, StringView(engine, found.Value()),
                                    "Source/Developer/SparksTestHost/Schema");
        REQUIRE(schema);
        Report report(allocator);
        REQUIRE(RunCorpus(allocator, platform::PhysicalFileSystem(), corpus.Value().View(),
                          schema.Value().View(), report));
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
        REQUIRE(write(area.Value().View(), "NoSource.names", ""));
        REQUIRE(write(area.Value().View(), "NoSource.types", ""));
        REQUIRE(write(area.Value().View(), "NoSource.ir", ""));
        REQUIRE(write(area.Value().View(), "Good.names", ""));
        REQUIRE(write(area.Value().View(), "Good.types", ""));
        REQUIRE(write(area.Value().View(), "Good.ir", ""));
        REQUIRE(write(area.Value().View(), "OnlyTree.ast", ""));
        REQUIRE(write(area.Value().View(), "Notes.txt", "x"));
        REQUIRE(write(rootPath, "Stray.spark", "x"));

        Report report(allocator);
        REQUIRE(RunCorpus(allocator, files, rootPath, "", report));
        PrintFailures(report);
        CHECK(report.CasesRun() == 1);
        REQUIRE(report.Failures().Size() == 9);
        // 낯선 파일은 목록을 훑을 때, 짝 없는 파일은 stem 순으로 맞춰 볼 때 기록한다. 같은 stem의
        // 짝 없는 파일은 .expect, .ast, .names, .types, .ir 순이다.
        CHECK(report.Failures()[0].View() ==
              StringView("Area/Notes.txt: unexpected file (only .spark, .expect, .ast, .names, "
                         ".types, .ir, .bc, .run and case folders)"));
        CHECK(report.Failures()[1].View() ==
              StringView("Area/NoExpect.spark: missing the .expect file"));
        CHECK(report.Failures()[2].View() ==
              StringView("Area/NoSource.expect: missing the .spark file or case folder"));
        CHECK(report.Failures()[3].View() ==
              StringView("Area/NoSource.ast: missing the .spark file or case folder"));
        CHECK(report.Failures()[4].View() ==
              StringView("Area/NoSource.names: missing the .spark file or case folder"));
        CHECK(report.Failures()[5].View() ==
              StringView("Area/NoSource.types: missing the .spark file or case folder"));
        CHECK(report.Failures()[6].View() ==
              StringView("Area/NoSource.ir: missing the .spark file or case folder"));
        CHECK(report.Failures()[7].View() ==
              StringView("Area/OnlyTree.ast: missing the .spark file or case folder"));
        CHECK(report.Failures()[8].View() ==
              StringView(
                  "Stray.spark: unexpected file at the corpus root (cases live in area folders)"));

        Report missing(allocator);
        auto absent = Path::Combine(allocator, rootPath, "Absent");
        REQUIRE(absent);
        auto ran = RunCorpus(allocator, files, absent.Value().View(), "", missing);
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
        REQUIRE(RunCorpus(allocator, files, rootPath, "", report));
        CHECK(report.CasesRun() == 2);
        REQUIRE(report.Failures().Size() == 1);
        CHECK(report.Failures()[0].View() == StringView("Area/Nested: missing the .expect file"));

        // corpus 경로가 폴더가 아니면 실행기 오류다.
        auto notFolder = Path::Combine(allocator, area.Value().View(), "Bom.spark");
        REQUIRE(notFolder);
        Report fileRoot(allocator);
        auto ran = RunCorpus(allocator, files, notFolder.Value().View(), "", fileRoot);
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

// 구문 사례(Lexical·Syntax·SyntaxErrors — corpus에서는 stage syntax)를 전체 단계로 컴파일한다.
// 복구된 트리(Error 노드·빠진 자식)가 이름 해석을 깨지 않아야 하고, 어휘·구문 진단이 있는
// 모듈에서는 "찾지 못함"(S0317·S0318)이 나지 않아야 한다(3.9절 연쇄 방지).
TEST(SparksConformance_SyntaxCorpusThroughNameResolution) {
    SystemAllocator allocator;
    {
        constexpr usize PathCapacity = 4096;
        char engine[PathCapacity];
        auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
        REQUIRE(found);
        platform::FileSystem& files = platform::PhysicalFileSystem();
        uint32 compiled = 0;
        uint32 broken = 0;
        constexpr StringView Areas[] = {"Lexical", "Syntax", "SyntaxErrors"};
        for (const StringView area : Areas) {
            StringBuilder<PathCapacity> areaPath(allocator);
            areaPath.Append(StringView(engine, found.Value()))
                .Append("/Source/Developer/SparksTestHost/Conformance/")
                .Append(area);
            REQUIRE(!areaPath.HasFailed());
            struct Listing {
                Allocator* allocator = nullptr;
                Array<String>* names = nullptr;
                bool failed = false;
            };

            Array<String> names(allocator);
            Listing listing{&allocator, &names, false};
            auto collect = [](const platform::DirectoryEntry& entry, void* context) noexcept {
                auto& list = *static_cast<Listing*>(context);
                const StringView name(entry.name, entry.nameBytes);
                if (entry.info.type == platform::FileType::Directory ||
                    !name.EndsWith(".spark", StringComparison::CaseSensitive)) {
                    return true;
                }

                String copy(*list.allocator);
                list.failed =
                    list.failed || !copy.TryAssign(name) || !list.names->TryPushBack(Move(copy));
                return !list.failed;
            };
            REQUIRE(files.EnumerateDirectory(areaPath.View().Data(), areaPath.ByteCount(), collect,
                                             &listing));
            REQUIRE(!listing.failed);
            for (const String& name : names) {
                StringBuilder<PathCapacity> filePath(allocator);
                filePath.Append(areaPath.View()).Append("/").Append(name.View());
                platform::File file;
                REQUIRE(file.Open(files, filePath.View().Data(), filePath.ByteCount(),
                                  platform::FileOpenMode::ReadExisting));
                uint64 size = 0;
                REQUIRE(file.Size(size));
                Array<char> bytes(allocator);
                REQUIRE(bytes.TryResize(static_cast<usize>(size)));
                const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
                REQUIRE(read.status && read.bytes == bytes.Size());
                REQUIRE(file.Close());

                SourceSet sources(allocator);
                DiagnosticList diagnostics(allocator);
                Compilation compilation(allocator, sources);
                auto manifest =
                    sources.TryAdd("SparksPackage.json5",
                                   "{ name: \"Test\", sparks: 1, profile: \"full\" }", diagnostics);
                REQUIRE(manifest);
                auto package = compilation.AddPackage(manifest.Value());
                REQUIRE(package);
                auto source = sources.TryAdd(name.View(), StringView(bytes.Data(), bytes.Size()),
                                             diagnostics);
                REQUIRE(source);
                REQUIRE(compilation.AddModule(package.Value(), source.Value(), "Main.spark"));
                REQUIRE(compilation.Check(diagnostics));
                ++compiled;
                bool syntaxErrors = false;
                bool unknownNames = false;
                for (const Diagnostic& diagnostic : diagnostics.View()) {
                    const uint32 code = static_cast<uint32>(diagnostic.Code());
                    constexpr uint32 FirstSyntaxCode = 101;
                    constexpr uint32 LastSyntaxCode = 299;
                    syntaxErrors =
                        syntaxErrors || (code >= FirstSyntaxCode && code <= LastSyntaxCode);
                    unknownNames = unknownNames ||
                                   diagnostic.Code() == DiagnosticCode::UnknownName ||
                                   diagnostic.Code() == DiagnosticCode::UnknownType;
                }

                if (syntaxErrors) {
                    ++broken;
                    if (unknownNames) {
                        platform::WriteDiagnostic(name.Data(), name.ByteCount());
                        platform::WriteDiagnostic("\n", 1);
                    }

                    CHECK(!unknownNames);
                }
            }
        }

        CHECK(compiled == 29); // 구문 사례 전부(사례를 더하면 함께 고친다)
        CHECK(broken >= 20);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 이름 해석 golden(`.names`)과 `stage` 지시(13.1절).
TEST(SparksConformance_ComparesNamesGoldenAndStages) {
    SystemAllocator allocator;
    {
        constexpr StringView Names = "1:7 decl const Test:Main.X\n"
                                     "1:10 type int\n";
        CaseInput input;
        input.name = "Area/Names.spark";
        input.source = "const X: int = 1;\n";
        input.expect = "clean\n";
        input.hasNames = true;
        input.names = Names;
        Report report(allocator);
        REQUIRE(RunCase(allocator, input, report));
        PrintFailures(report);
        CHECK(report.Failures().IsEmpty());

        input.names = "1:7 decl const Test:Main.Y\n";
        REQUIRE(RunCase(allocator, input, report));
        REQUIRE(report.Failures().Size() == 1);
        CHECK(report.Failures()[0].View() ==
              StringView("Area/Names.spark: names differ from .names at line 1\n"
                         "  expected: 1:7 decl const Test:Main.Y\n"
                         "  actual:   1:7 decl const Test:Main.X\n"
                         "  actual names:\n1:7 decl const Test:Main.X\n1:10 type int\n"));

        // 이름 해석까지 가지 않으면 `.names`는 실패다.
        input.names = Names;
        input.expect = "stage imports\nclean\n";
        REQUIRE(RunCase(allocator, input, report));
        REQUIRE(report.Failures().Size() == 2);
        CHECK(report.Failures()[1].View() ==
              StringView("Area/Names.spark: has a .names file but the module was not "
                         "name-resolved"));

        // stage syntax는 뒤 단계의 진단을 보지 않는다.
        Report staged(allocator);
        REQUIRE(Run(allocator, "Area/Stage.spark", "import Nope;\nfn F(): int { return X; }\n",
                    "stage syntax\nclean\n", staged));
        REQUIRE(Run(allocator, "Area/Stage2.spark", "import Nope;\nfn F(): int { return X; }\n",
                    "stage imports\nerror S0301 1:8\n", staged));
        REQUIRE(Run(allocator, "Area/Stage3.spark", "fn F(): int { return X; }\n",
                    "stage names\nerror S0317 1:22\n", staged));
        REQUIRE(Run(allocator, "Area/Stage4.spark", "fn F(): int { return X; }\n",
                    "error S0317 1:22\n", staged));
        PrintFailures(staged);
        CHECK(staged.Failures().IsEmpty());
        CHECK(staged.CasesRun() == 4);

        Expectation expectation(allocator);
        String problem(allocator);
        auto parsed = TryParseExpectation("stage imports\nclean\n", expectation, problem);
        REQUIRE(parsed && parsed.Value());
        CHECK(expectation.Stage() == CompilationStage::Imports);
        parsed = TryParseExpectation("clean\n", expectation, problem);
        REQUIRE(parsed && parsed.Value());
        CHECK(expectation.Stage() == CompilationStage::All);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksConformance_RunGolden) {
    // `.run` 실행 golden(Docs/Design/SparksVm.ko.md 7절): `->` 앞의 호출만 읽어 실행하고 줄 전체를
    // 실제 결과로 다시 써서 비교한다. 주석·빈 줄은 그대로 둔다.
    SystemAllocator allocator;
    constexpr StringView Source = R"(enum Side { Left, Right }

fn Add(a: int, b: int): int {
    return a + b;
}

fn Flip(side: Side): Side {
    if (side == Side.Left) {
        return Side.Right;
    }

    return Side.Left;
}

fn Same(text: string): bool {
    return text == text;
}

fn Word(): string {
    return "a, \"b\" -> c";
}

fn Half(x: float32): float32 {
    return x * 0.5;
}

fn Pass(r: Result<int>): Result<int> {
    let value = try r;
    return value;
}

struct Pair {
    var A: int = 0;
}

fn First(pair: Pair): int {
    return pair.A;
}

fn Make(): Pair {
    return Pair(A: 1);
}

fn Label(): Tag? {
    return null;
}

fn Total(items: Map<string, Array<int>>): int {
    var total = 0;
    for (_, values in items) {
        for (value in values) {
            total += value;
        }
    }

    Log.Info("total ${total}");
    return total;
}

fn Size(items: Set<int>): int {
    return items.Count;
}
)";
    auto run = [&](StringView golden, Report& report) noexcept {
        CaseInput input;
        input.name = "Run/Case.spark";
        input.source = Source;
        input.expect = "clean\n";
        input.hasRun = true;
        input.run = golden;
        return RunCase(allocator, input, report);
    };
    {
        Report report(allocator);
        // 문자열 인자 안의 `,`·`->`·escape는 나누지 않는다.
        REQUIRE(run("# comment\n\nAdd(1, 2) -> 3 (2 steps)\nFlip(Left) -> Right (5 steps)\n"
                    "Same(\"x, y -> z\") -> true (2 steps)\n"
                    "Word() -> \"a, \\\"b\\\" -> c\" (2 steps)\n"
                    "Half(3.0) -> 1.5 (3 steps)\nPass(error(3)) -> error(3) (2 steps)\n"
                    "Make() -> Pair(A: 1) (4 steps)\n"
                    "Total({\"a, b\": [1, 2], \"c\": []}) -> 3 (54 steps)\n"
                    "  log info \"total 3\"\n"
                    "Size({}) -> 0 (4 steps)\n",
                    report));
        PrintFailures(report);
        CHECK(report.Failures().IsEmpty());
    }
    {
        // 결과가 다르면(또는 아직 결과가 없으면) 실제 줄과 전체 실제 결과를 보인다.
        Report report(allocator);
        REQUIRE(run("Add(1, 2) -> 4 (2 steps)\nAdd(2, 2)\n", report));
        REQUIRE(report.Failures().Size() == 1);
        CHECK(report.Failures()[0].View() ==
              StringView("Run/Case.spark: run differ from .run at line 1\n"
                         "  expected: Add(1, 2) -> 4 (2 steps)\n"
                         "  actual:   Add(1, 2) -> 3 (2 steps)\n"
                         "  actual run:\n"
                         "Add(1, 2) -> 3 (2 steps)\n"
                         "Add(2, 2) -> 4 (2 steps)\n"));
    }
    struct Bad {
        StringView golden;
        StringView reason;
    };
    const Bad bad[] = {
        {"Add\n", "Run/Case.spark: .run line 1: expected '<function>(<arguments>)'"},
        {"\nNope(1)\n", "Run/Case.spark: .run line 2: no top-level function or method 'Nope'"},
        {"Add(1)\n", "Run/Case.spark: .run line 1: too few arguments"},
        {"Add(1, 2, 3)\n", "Run/Case.spark: .run line 1: too many arguments"},
        {"Add(x, 1)\n",
         "Run/Case.spark: .run line 1: cannot read 'x' as a value of this parameter"},
        {"Add(9223372036854775808, 1)\n",
         "Run/Case.spark: .run line 1: cannot read '9223372036854775808' as a value of this "
         "parameter"},
        {"Flip(Up)\n",
         "Run/Case.spark: .run line 1: cannot read 'Up' as a value of this parameter"},
        {"First(1)\n",
         "Run/Case.spark: .run line 1: a parameter type is not supported by .run yet"},
        {"Label()\n", "Run/Case.spark: .run line 1: the return type is not supported by .run yet"},
        {"Same(abc)\n",
         "Run/Case.spark: .run line 1: cannot read 'abc' as a value of this parameter"},
        {"Same(\"a\\q\")\n",
         "Run/Case.spark: .run line 1: cannot read '\"a\\q\"' as a value of this parameter"},
        {"Half(1e39)\n",
         "Run/Case.spark: .run line 1: cannot read '1e39' as a value of this parameter"},
        {"Half(inf)\n",
         "Run/Case.spark: .run line 1: cannot read 'inf' as a value of this parameter"},
        {"new #1 Nope\n", "Run/Case.spark: .run line 1: 'new' needs a class of the module"},
        {"fields #3\n",
         "Run/Case.spark: .run line 1: the object has no instance (use 'new' first)"},
        {"detach x\n",
         "Run/Case.spark: .run line 1: expected an object '#<number>' after the directive"},
        {"Total([])\n",
         "Run/Case.spark: .run line 1: cannot read '[]' as a value of this parameter"},
        {"Total({\"a\"})\n", "Run/Case.spark: .run line 1: cannot read '\"a\"' as a map entry"},
        {"Total({\"a\": [1], \"a\": [2]})\n",
         "Run/Case.spark: .run line 1: cannot make '{\"a\": [1], \"a\": [2]}' (repeated key?)"},
        {"Total({\"a\": [x]})\n",
         "Run/Case.spark: .run line 1: cannot read 'x' as a value of this parameter"},
        {"Size({1, 1})\n", "Run/Case.spark: .run line 1: cannot make '{1, 1}' (repeated key?)"},
        {"Pass(error(18446744073709551615))\n",
         "Run/Case.spark: .run line 1: cannot read 'error(18446744073709551615)' as a value of "
         "this parameter"},
    };
    for (const Bad& entry : bad) {
        Report report(allocator);
        REQUIRE(run(entry.golden, report));
        REQUIRE(report.Failures().Size() == 1);
        if (report.Failures()[0].View() != entry.reason) {
            PrintFailures(report);
        }

        CHECK(report.Failures()[0].View() == entry.reason);
    }

    {
        // 진단이 있는 사례는 실행할 bytecode가 없다.
        CaseInput input;
        input.name = "Run/Broken.spark";
        input.source = "fn F(): int {\n    return true;\n}\n";
        input.expect = "clean\n";
        input.hasRun = true;
        input.run = "F()\n";
        Report report(allocator);
        REQUIRE(RunCase(allocator, input, report));
        bool found = false;
        for (const String& failure : report.Failures()) {
            found = found || failure.View() ==
                                 StringView("Run/Broken.spark: has a .run file but the module was "
                                            "not lowered to bytecode (a file case without "
                                            "diagnostics is needed)");
        }

        CHECK(found);
    }
}

TEST(SparksConformance_RunGoldenAllocationFailure) {
    // 언어 함수(문자열·컨테이너·서식·로그)를 부르는 사례를 모든 할당 실패 자리에서 돌린다: 실패는
    // OutOfMemory로 돌아오고(보고에 거짓 실패를 남기지 않는다) 새는 할당이 없다.
    SystemAllocator backing;
    constexpr StringView Source = R"(fn Mix(text: string): string {
    var parts = text.Split(",");
    parts.Add("z");
    parts.Sort();
    var counts: Map<string, int> = [];
    for (part in parts) {
        counts[part] = (counts[part] ?? 0) + 1;
    }

    let removed = counts.Remove("a");
    var seen: Set<string> = [];
    for (key in counts.Keys()) {
        let added = seen.Add(key.ToUpperAscii());
        Assert(added, "keys are unique");
    }

    Log.Info("mixed ${removed}");
    return "${Format.Fixed(1.25, 1)}${seen.Count}${parts.Count}";
}
)";
    constexpr StringView Golden = "Mix(\"b,a,b\") -> \"1.224\" (0 steps)\n";
    uint64 attempts = 0;
    uint64 failures = 0;
    SystemAllocator side; // 기대값은 세는 allocator 밖에 둔다
    String expected(side);
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        const uint64 before = backing.Stats().liveAllocations;
        {
            CaseInput input;
            input.name = "Run/Mix.spark";
            input.source = Source;
            input.expect = "clean\n";
            input.hasRun = true;
            input.run = failAt == 0 ? Golden : expected.View();
            Report report(allocator);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto ran = RunCase(allocator, input, report);
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                // 첫 바퀴는 실제 결과(명령 수 포함)를 얻는다 — 이후 바퀴의 기대값이다.
                REQUIRE(ran);
                REQUIRE(report.Failures().Size() == 1);
                const StringView failure = report.Failures()[0].View();
                const StringView marker = "  actual run:\n";
                usize at = 0;
                while (at + marker.ByteCount() <= failure.ByteCount() &&
                       StringView(failure.Data() + at, marker.ByteCount()) != marker) {
                    ++at;
                }

                REQUIRE(at + marker.ByteCount() <= failure.ByteCount());
                const StringView actual(failure.Data() + at + marker.ByteCount(),
                                        failure.ByteCount() - at - marker.ByteCount());
                CHECK(actual.StartsWith("Mix(\"b,a,b\") -> \"1.224\" ("));
                CHECK(actual.EndsWith("  log info \"mixed true\"\n"));
                REQUIRE(expected.TryAppend(actual));
                attempts = used;
            } else if (!ran) {
                CHECK(ran.Error() == ErrorCode::OutOfMemory);
                ++failures;
            } else {
                // 실패를 삼킨 경로는 결과가 같아야 한다(거짓 실패 없음).
                if (!report.Failures().IsEmpty()) {
                    PrintFailures(report);
                }

                CHECK(report.Failures().IsEmpty());
            }
        }

        CHECK(backing.Stats().liveAllocations == before);
    }

    CHECK(failures > 0);
}

TEST(SparksConformance_RunGoldenAllocationFailureWithHost) {
    // 언어 함수(문자열·컨테이너·서식·로그)를 부르는 사례를 모든 할당 실패 자리에서 돌린다: 실패는
    // OutOfMemory로 돌아오고(보고에 거짓 실패를 남기지 않는다) 새는 할당이 없다.
    SystemAllocator backing;
    // 가짜 host 경로(링크·수신자 객체·필드 보관 버퍼·native 기록)의 할당 실패: 보관 버퍼가 어느
    // 출구에서도 놓이고(LC5d1 리뷰 B5), 수신자 할당 실패는 OutOfMemory다(B6).
    constexpr StringView Source = R"(class Keeper : ScriptableComponent {
    var Names: Array<string> = [];

    fn Keep(actor: Actor, name: string): int {
        Names.Add(name);
        actor.Location = actor.GetForwardVector();
        return Names.Count + Actor.CountAll();
    }
}
)";
    constexpr StringView Golden = "Keeper.Keep(#1, #2, \"x\") -> 1 (0 steps)\n";
    constexpr StringView Schema = R"({
  "format": 1, "package": "Rocks", "module": "Engine",
  "types": [
    {"name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object",
     "fields": [{"name": "Location", "type": "vec3", "access": "readWrite", "computed": true}],
     "functions": [
       {"name": "GetForwardVector", "id": "5a7e0000000000000000000000000203", "returns": "vec3"},
       {"name": "CountAll", "id": "5a7e0000000000000000000000000206", "returns": "int", "static": true}]},
    {"name": "Component", "id": "5a7e0000000000000000000000000005", "kind": "object",
     "fields": [{"name": "Owner", "id": "5a7e0000000000000000000000000131", "type": "Actor"}]},
    {"name": "ScriptableComponent", "id": "5a7e0000000000000000000000000007", "kind": "object",
     "base": "Component", "scriptExtensible": true}
  ]
})";
    const CaseFile schemas[] = {{"Schema/Engine.sparkbind.json", Schema}};

    uint64 attempts = 0;
    uint64 failures = 0;
    SystemAllocator side; // 기대값은 세는 allocator 밖에 둔다
    String expected(side);
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        const uint64 before = backing.Stats().liveAllocations;
        {
            CaseInput input;
            input.name = "Run/Keeper.spark";
            input.schemas = schemas;
            input.source = Source;
            input.expect = "clean\n";
            input.hasRun = true;
            input.run = failAt == 0 ? Golden : expected.View();
            Report report(allocator);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto ran = RunCase(allocator, input, report);
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                // 첫 바퀴는 실제 결과(명령 수 포함)를 얻는다 — 이후 바퀴의 기대값이다.
                REQUIRE(ran);
                REQUIRE(report.Failures().Size() == 1);
                const StringView failure = report.Failures()[0].View();
                const StringView marker = "  actual run:\n";
                usize at = 0;
                while (at + marker.ByteCount() <= failure.ByteCount() &&
                       StringView(failure.Data() + at, marker.ByteCount()) != marker) {
                    ++at;
                }

                REQUIRE(at + marker.ByteCount() <= failure.ByteCount());
                const StringView actual(failure.Data() + at + marker.ByteCount(),
                                        failure.ByteCount() - at - marker.ByteCount());
                CHECK(actual.StartsWith("Keeper.Keep(#1, #2, \"x\") -> 1 ("));
                CHECK(actual.EndsWith(
                    "  host Actor.GetForwardVector(#2) -> 0\n  host Actor.CountAll() -> 0\n"));
                REQUIRE(expected.TryAppend(actual));
                attempts = used;
            } else if (!ran) {
                CHECK(ran.Error() == ErrorCode::OutOfMemory);
                ++failures;
            } else {
                // 실패를 삼킨 경로는 결과가 같아야 한다(거짓 실패 없음).
                if (!report.Failures().IsEmpty()) {
                    PrintFailures(report);
                }

                CHECK(report.Failures().IsEmpty());
            }
        }

        CHECK(backing.Stats().liveAllocations == before);
    }

    CHECK(failures > 0);
}

TEST(SparksConformance_RunGoldenAllocationFailureAsync) {
    // async 실행(SparksRuntime 2.4·2.5절)의 모든 할당 실패 자리: 정지(전체·하위 실행 경계), 대기
    // 표, 묶음, 이벤트 대기의 전달 순회 안 재개, 하위 실행의 끝·fault 전파, 취소. 실패는
    // OutOfMemory로 돌아오고 새는 할당이 없다.
    SystemAllocator backing;
    constexpr StringView Source = R"(class Pulse : ScriptableComponent {
    var Count = 0;
    event Go(n: int);

    fn Fire(n: int) {
        emit Go(n);
    }

    async fn Child() {
        let text = "c${Count}";
        await NextTick();
        Count += 1;
        Log.Info(text);
    }

    async fn Quick() {
        if (Count > 1000) {
            await NextTick();
        }

        Count += 1;
    }

    async fn Listen() {
        await Go;
        Count += 10;
    }

    async fn Bad() {
        await NextTick();
        Assert(false, "bad");
    }

    async fn Work() {
        let label = "w${Count}";
        await All(Child(), Quick(), Delay(0.1, Clock.Game));
        await Race(Listen(), Delay(5.0, Clock.UI));
        Log.Info(label);
    }

    async fn Fails() {
        await All(Bad(), NextTick());
    }

    async fn Never() {
        await Race(Child(), Delay(9.0, Clock.Game));
    }

    var Hum: Loop?;

    fn Swap() {
        Hum = Owner.StartLoop();
    }

    async fn Hold() {
        let held = Owner.StartLoop();
        await NextTick();
        held?.Stop();
    }

    fn Fail() {
        Assert(false, "fail");
    }
}
)";
    constexpr StringView Golden = R"(new #1 Pulse -> ok
hook #1 Enable -> ok
Pulse.Work(#1) -> ok
Pulse.Fails(#1) -> ok
Pulse.Never(#1) -> ok
Pulse.Swap(#1) -> ok
Pulse.Swap(#1) -> ok
Pulse.Hold(#1) -> ok
advance 0.1 -> ok
advance 0.1 -> ok
Pulse.Fire(#1, 1) -> ok
fields #1 -> ok
Pulse.Fail(#1) -> ok
Pulse.Fail(#1) -> ok
Pulse.Fail(#1) -> ok
Pulse.Fire(#1, 2) -> ok
request #1 disable -> ok
request #1 destroy -> ok
apply -> ok
endframe -> ok
close -> ok
)";
    constexpr StringView Schema = R"({
  "format": 1, "package": "Rocks", "module": "Engine",
  "types": [
    {"name": "Loop", "id": "5a7e0000000000000000000000000300", "kind": "object", "held": true,
     "functions": [{"name": "Stop", "id": "5a7e0000000000000000000000000302"}]},
    {"name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object",
     "functions": [{"name": "StartLoop", "id": "5a7e0000000000000000000000000301",
                    "returns": "Loop?", "effects": ["acquire"]}]},
    {"name": "Component", "id": "5a7e0000000000000000000000000005", "kind": "object",
     "fields": [{"name": "Owner", "id": "5a7e0000000000000000000000000131", "type": "Actor"}]},
    {"name": "ScriptableComponent", "id": "5a7e0000000000000000000000000007", "kind": "object",
     "base": "Component", "scriptExtensible": true}
  ]
})";
    const CaseFile schemas[] = {{"Schema/Engine.sparkbind.json", Schema}};
    uint64 attempts = 0;
    uint64 failures = 0;
    SystemAllocator side; // 기대값은 세는 allocator 밖에 둔다
    String expected(side);
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        const uint64 before = backing.Stats().liveAllocations;
        {
            CaseInput input;
            input.name = "Run/Pulse.spark";
            input.schemas = schemas;
            input.source = Source;
            input.expect = "clean\n";
            input.hasRun = true;
            input.run = failAt == 0 ? Golden : expected.View();
            Report report(allocator);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto ran = RunCase(allocator, input, report);
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                // 첫 바퀴는 실제 결과(명령 수 포함)를 얻는다 — 이후 바퀴의 기대값이다.
                REQUIRE(ran);
                REQUIRE(report.Failures().Size() == 1);
                const StringView failure = report.Failures()[0].View();
                const StringView marker = "  actual run:\n";
                usize at = 0;
                while (at + marker.ByteCount() <= failure.ByteCount() &&
                       StringView(failure.Data() + at, marker.ByteCount()) != marker) {
                    ++at;
                }

                REQUIRE(at + marker.ByteCount() <= failure.ByteCount());
                const StringView actual(failure.Data() + at + marker.ByteCount(),
                                        failure.ByteCount() - at - marker.ByteCount());
                CHECK(actual.StartsWith("new #1 Pulse -> ok ("));
                CHECK(actual.Contains("close -> ok (ledger 0 buffers)",
                                      StringComparison::CaseSensitive));
                CHECK(actual.Contains("host release", StringComparison::CaseSensitive));
                REQUIRE(expected.TryAppend(actual));
                attempts = used;
            } else if (!ran) {
                CHECK(ran.Error() == ErrorCode::OutOfMemory);
                ++failures;
            } else {
                // 실패를 삼킨 경로는 결과가 같아야 한다(거짓 실패 없음).
                if (!report.Failures().IsEmpty()) {
                    PrintFailures(report);
                }

                CHECK(report.Failures().IsEmpty());
            }
        }

        CHECK(backing.Stats().liveAllocations == before);
    }

    CHECK(failures > 0);
}

// 폴더 사례(13.1절 "패키지 사례"): 배치 오류는 사례마다 실패 하나이고, 폴더 사례의 지시는 파일
// 경로를 쓰며 파일 사례는 쓰지 않는다. schema 폴더에는 *.sparkbind.json만 둔다.
TEST(SparksConformance_FolderCasesAndSchemas) {
    SystemAllocator allocator;
    {
        platform::FileSystem& files = platform::PhysicalFileSystem();
        StringBuilder<64> root(allocator);
        REQUIRE(root.TryAppendFormat("rocks-sparks-folders-{}-{}", platform::CurrentProcessId(),
                                     platform::MonotonicTicks()));
        const StringView rootPath = root.View();
        auto write = [&](StringView relative, StringView content) noexcept -> bool {
            auto path = Path::Combine(allocator, rootPath, relative);
            if (!path) {
                return false;
            }

            const StringView full = path.Value().View();
            usize slash = full.ByteCount();
            while (slash > 0 && full.Data()[slash - 1] != '/' && full.Data()[slash - 1] != '\\') {
                --slash;
            }

            if (slash > 0 && !platform::MakeDirectoryTree(files, full.Data(), slash - 1)) {
                return false;
            }

            platform::File file;
            if (!file.Open(files, full.Data(), full.ByteCount(),
                           platform::FileOpenMode::CreateAlways)) {
                return false;
            }

            const platform::FileTransfer written =
                file.WriteAll(content.Data(), content.ByteCount());
            const bool closed = static_cast<bool>(file.Close());
            return static_cast<bool>(written.status) && closed;
        };
        auto folder = [&](StringView relative) noexcept -> bool {
            auto path = Path::Combine(allocator, rootPath, relative);
            return path && static_cast<bool>(platform::MakeDirectoryTree(files, path.Value().Data(),
                                                                         path.Value().ByteCount()));
        };
        constexpr StringView Manifest = "{ name: \"Game\", sparks: 1, profile: \"core\" }";
        REQUIRE(write("Corpus/Area/Good/SparksPackage.json5", Manifest));
        REQUIRE(write("Corpus/Area/Good/Combat/Main.spark", "import Combat.Main;\n"));
        REQUIRE(write("Corpus/Area/Good.expect", "error S0304 Combat/Main.spark 1:8\n"));
        REQUIRE(write("Corpus/Area/Good.ast", ""));
        // 폴더 사례의 `.names`: 모듈마다 "== 경로" 줄과 덤프.
        REQUIRE(write("Corpus/Area/Good.names", "== Combat/Main.spark\n"));
        REQUIRE(write("Corpus/Area/Both.spark", ""));
        REQUIRE(write("Corpus/Area/Both/SparksPackage.json5", Manifest));
        REQUIRE(write("Corpus/Area/Both.expect", "clean\n"));
        REQUIRE(folder("Corpus/Area/Empty"));
        REQUIRE(write("Corpus/Area/Empty.expect", "clean\n"));
        REQUIRE(write("Corpus/Area/File.spark", "import Nope;\n"));
        REQUIRE(write("Corpus/Area/File.expect", "error S0301 Main.spark 1:8\n"));
        REQUIRE(write("Corpus/Area/Nested/SparksPackage.json5", Manifest));
        REQUIRE(write("Corpus/Area/Nested/Sub/SparksPackage.json5", Manifest));
        REQUIRE(write("Corpus/Area/Nested.expect", "clean\n"));
        REQUIRE(write("Corpus/Area/NoManifest/Lib/Main.spark", ""));
        REQUIRE(write("Corpus/Area/NoManifest.expect", "clean\n"));
        REQUIRE(write("Corpus/Area/Odd/SparksPackage.json5", Manifest));
        REQUIRE(write("Corpus/Area/Odd/notes.txt", "x"));
        REQUIRE(write("Corpus/Area/Odd.expect", "clean\n"));
        REQUIRE(write("Corpus/Area/PathMissing/SparksPackage.json5", Manifest));
        REQUIRE(write("Corpus/Area/PathMissing.expect", "error S1108 1:1\n"));
        REQUIRE(write("Corpus/Area/RootFile/Main.spark", ""));
        REQUIRE(write("Corpus/Area/RootFile.expect", "clean\n"));
        // 두 패키지 사례: 경로는 사례 폴더 기준이다.
        REQUIRE(write("Corpus/Area/Two/Game/SparksPackage.json5",
                      "{ name: \"Game\", sparks: 1, profile: \"core\", dependencies: [\"Lib\"] }"));
        REQUIRE(write("Corpus/Area/Two/Game/Main.spark", "import Lib.Util;\nimport Lib.Gone;\n"));
        REQUIRE(write("Corpus/Area/Two/Lib/SparksPackage.json5",
                      "{ name: \"Lib\", sparks: 1, profile: \"core\" }"));
        REQUIRE(write("Corpus/Area/Two/Lib/Util.spark", ""));
        REQUIRE(write("Corpus/Area/Two.expect",
                      "error S0714 Game/Main.spark 1:8\nerror S0301 Game/Main.spark 2:8\n"));
        REQUIRE(write("Schema/Engine.sparkbind.json",
                      "{ \"format\": 1, \"package\": \"Rocks\", \"module\": \"Engine\", "
                      "\"types\": [] }"));
        REQUIRE(write("Schema/notes.txt", "x"));

        auto corpus = Path::Combine(allocator, rootPath, "Corpus");
        auto schema = Path::Combine(allocator, rootPath, "Schema");
        REQUIRE(corpus);
        REQUIRE(schema);
        Report report(allocator);
        REQUIRE(RunCorpus(allocator, files, corpus.Value().View(), schema.Value().View(), report));
        PrintFailures(report);
        CHECK(report.CasesRun() == 4); // File, Good, PathMissing, Two
        REQUIRE(report.Failures().Size() == 10);
        const StringView expected[] = {
            "Schema/notes.txt: unexpected item (only .sparkbind.json)",
            "Area/Both: a file case and a folder case share the name",
            "Area/Empty: the case folder has no files",
            "Area/File.spark: invalid .expect: a file case does not name files",
            "Area/Good.ast: .ast is only for file cases",
            "Area/Nested: 'Sub/SparksPackage.json5' is not at a package root",
            "Area/NoManifest: package folder 'Lib' has no SparksPackage.json5",
            "Area/Odd: unexpected file 'notes.txt' (only .spark and SparksPackage.json5)",
            "Area/PathMissing: invalid .expect: a folder case names the file of every diagnostic",
            "Area/RootFile: 'Main.spark' is at the case root, which has no SparksPackage.json5",
        };
        for (usize index = 0; index < report.Failures().Size(); ++index) {
            CHECK(report.Failures()[index].View() == expected[index]);
        }

        // schema 폴더가 없으면 실행기 오류다.
        auto absent = Path::Combine(allocator, rootPath, "NoSchema");
        REQUIRE(absent);
        Report missing(allocator);
        auto ran =
            RunCorpus(allocator, files, corpus.Value().View(), absent.Value().View(), missing);
        CHECK(!ran && ran.Error() == ErrorCode::NotFound);

        CHECK(platform::DeleteDirectoryTree(files, rootPath.Data(), rootPath.ByteCount()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// schema는 모든 사례와 함께 컴파일되며 진단 경로는 Schema/<파일>이다(파일 사례에서도 다른 파일의
// 진단은 기대와 맞지 않는다).
TEST(SparksConformance_SchemasCompileWithEveryCase) {
    SystemAllocator allocator;
    {
        const CaseFile schemas[] = {{"Schema/Bad.sparkbind.json", "{}"}};
        CaseInput input;
        input.name = "Area/Clean.spark";
        input.source = "";
        input.expect = "clean\n";
        input.schemas = schemas;
        Report report(allocator);
        REQUIRE(RunCase(allocator, input, report));
        REQUIRE(report.Failures().Size() == 1);
        CHECK(report.Failures()[0].View().StartsWith(
            "Area/Clean.spark: diagnostics differ from .expect\n"
            "  expected:\n"
            "    clean\n"
            "  actual:\n"
            "Schema/Bad.sparkbind.json:1:1: error S1120:"));
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
