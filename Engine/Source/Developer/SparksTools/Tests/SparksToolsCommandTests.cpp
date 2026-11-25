#include <Core/Algorithms.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <SparksTools/SparksCommand.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include "SparksToolsTestSupport.h"

// `sparks` 명령 시험이다(Docs/Design/SparksTools.ko.md 5절 — LC8a): 임시 폴더의 패키지로 check·
// dump를 같은 프로세스에서 돌려 출력·종료 코드를 대조한다.
using namespace rocks;
using namespace rocks::sparks::tools;
namespace sparks_tools_command_tests_file {

using namespace rocks::test::sparks_tools;

TEST(SparksTools_CheckReportsDiagnosticsAndExitCodes) {
    SystemAllocator allocator;
    TempFolder temp(allocator);
    REQUIRE(temp.Write("Pkg/SparksPackage.json5", Manifest));
    REQUIRE(temp.Write("Pkg/Main.spark", "fn F(): int {\n    return 1;\n}\n"));
    REQUIRE(temp.Write("Pkg/Combat/Score.spark", "import Nope;\n"));
    REQUIRE(temp.Write("Pkg/.hidden/Skip.spark", "this is not Sparks\n"));
    REQUIRE(temp.Write("Pkg/notes.txt", "ignored\n"));
    const String folder = temp.PathOf("Pkg");
    const String shown = Slashed(allocator, folder.View());
    {
        BufferOutput output(allocator);
        const StringView arguments[] = {"check", folder.View()};
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
              ExitFailure);
        StringBuilder<256> expected(allocator);
        REQUIRE(expected.TryAppendFormat("{}/Combat/Score.spark:1:8: error S0301:", shown.View()));
        CHECK(output.output.View().StartsWith(expected.View()));
        CHECK(output.output.View().EndsWith("1 errors in 2 modules\n"));
        CHECK(output.error.IsEmpty());
    }

    // 파일 인자: 그 파일의 패키지 전체를 검사한다(진단 경로는 인자 표기 기준).
    REQUIRE(temp.Write("Pkg/Combat/Score.spark", "fn G(): int {\n    return 2;\n}\n"));
    {
        BufferOutput output(allocator);
        const String file = temp.PathOf("Pkg/Main.spark");
        const StringView arguments[] = {"check", file.View()};
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
              ExitSuccess);
        CHECK(output.output.View() == "0 errors in 2 modules\n");
    }

    // 패키지 밖 폴더: 패키지가 없다(1), 패키지 밖 원문(1).
    REQUIRE(temp.Write("Loose/Lone.spark", "fn F() {\n}\n"));
    {
        BufferOutput output(allocator);
        const String loose = temp.PathOf("Loose");
        const StringView arguments[] = {"check", loose.View()};
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
              ExitFailure);
        CHECK(output.error.View().Contains("the file is outside every package",
                                           StringComparison::CaseSensitive));
    }

    {
        BufferOutput output(allocator);
        const String missing = temp.PathOf("Missing");
        const StringView arguments[] = {"check", missing.View()};
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
              ExitFailure);
        CHECK(output.error.View().Contains("not a folder or a .spark file",
                                           StringComparison::CaseSensitive));
    }
}

TEST(SparksTools_UsageErrors) {
    SystemAllocator allocator;
    struct Case {
        Span<const StringView> arguments;
        StringView message;
    };

    const StringView none[] = {"check"};
    const StringView unknown[] = {"frobnicate", "x"};
    const StringView option[] = {"check", "--bogus", "x"};
    const StringView schema[] = {"check", "x", "--schema"};
    const StringView dumpKind[] = {"dump", "x"};
    const StringView twoKinds[] = {"dump", "--ast", "--ir", "x"};
    const StringView checkKind[] = {"check", "--ast", "x"};
    const StringView buildOut[] = {"build", "x"};
    const StringView checkOut[] = {"check", "--out", "o", "x"};
    const StringView twoOuts[] = {"build", "--out", "o", "--out", "p", "x"};
    const StringView outValue[] = {"build", "x", "--out"};
    const StringView unknownWithOut[] = {"frob", "--out", "o", "x"};
    const StringView checkFrames[] = {"check", "--max-frames", "5", "x"};
    const StringView twoFrames[] = {"test", "--max-frames", "5", "--max-frames", "6", "x"};
    const StringView zeroFrames[] = {"test", "--max-frames", "0", "x"};
    const StringView wordFrames[] = {"test", "--max-frames", "ten", "x"};
    const StringView signFrames[] = {"test", "--max-frames", "+5", "x"};
    const StringView manyFrames[] = {"test", "--max-frames", "1000001", "x"};
    const StringView longFrames[] = {"test", "--max-frames", "99999999999", "x"};
    const StringView noFrames[] = {"test", "x", "--max-frames"};
    const Case cases[] = {
        {{}, "sparks: no command given\n"},
        {none, "sparks: no path given\n"},
        {unknown, "sparks: unknown command\n"},
        {option, "sparks: unknown option\n"},
        {schema, "sparks: '--schema' needs a folder\n"},
        {dumpKind, "sparks: 'dump' needs --ast, --names, --types, --ir or --bytecode\n"},
        {twoKinds, "sparks: 'dump' takes one dump option\n"},
        {checkKind, "sparks: a dump option is only for 'dump'\n"},
        {buildOut, "sparks: 'build' needs --out <folder>\n"},
        {checkOut, "sparks: '--out' is only for 'build'\n"},
        {twoOuts, "sparks: 'build' takes one --out\n"},
        {outValue, "sparks: '--out' needs a folder\n"},
        {unknownWithOut, "sparks: unknown command\n"},
        {checkFrames, "sparks: '--max-frames' is only for 'test'\n"},
        {twoFrames, "sparks: 'test' takes one --max-frames\n"},
        {zeroFrames, "sparks: '--max-frames' needs a number from 1 to 1000000\n"},
        {wordFrames, "sparks: '--max-frames' needs a number from 1 to 1000000\n"},
        {signFrames, "sparks: '--max-frames' needs a number from 1 to 1000000\n"},
        {manyFrames, "sparks: '--max-frames' needs a number from 1 to 1000000\n"},
        {longFrames, "sparks: '--max-frames' needs a number from 1 to 1000000\n"},
        {noFrames, "sparks: '--max-frames' needs a number from 1 to 1000000\n"},
    };
    for (const Case& entry : cases) {
        BufferOutput output(allocator);
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), entry.arguments, output) ==
              ExitUsage);
        CHECK(output.error.View().StartsWith(entry.message));
        CHECK(output.error.View().Contains("usage: sparks", StringComparison::CaseSensitive));
        CHECK(output.output.IsEmpty());
    }
}

TEST(SparksTools_DumpPrintsEachModule) {
    SystemAllocator allocator;
    TempFolder temp(allocator);
    REQUIRE(temp.Write("Pkg/SparksPackage.json5", Manifest));
    REQUIRE(temp.Write("Pkg/B.spark", "fn G(): int {\n    return 2;\n}\n"));
    REQUIRE(temp.Write("Pkg/A.spark", "fn F(): int {\n    return 1;\n}\n"));
    const String folder = temp.PathOf("Pkg");
    const String shown = Slashed(allocator, folder.View());
    {
        // 모듈은 진단 경로 byte 순이고 `== 경로` 머리 줄이 앞선다.
        BufferOutput output(allocator);
        const StringView arguments[] = {"dump", "--names", folder.View()};
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
              ExitSuccess);
        StringBuilder<256> first(allocator);
        REQUIRE(first.TryAppendFormat("== {}/A.spark\n", shown.View()));
        CHECK(output.output.View().StartsWith(first.View()));
        StringBuilder<256> second(allocator);
        REQUIRE(second.TryAppendFormat("== {}/B.spark\n", shown.View()));
        CHECK(output.output.View().Contains(second.View(), StringComparison::CaseSensitive));
        CHECK(output.output.View().Contains("decl fn Game:A.F", StringComparison::CaseSensitive));
    }

    {
        // 파일 인자는 그 모듈만, bytecode 덤프.
        BufferOutput output(allocator);
        const String file = temp.PathOf("Pkg/B.spark");
        const StringView arguments[] = {"dump", "--bytecode", file.View()};
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
              ExitSuccess);
        CHECK(!output.output.View().Contains("A.spark", StringComparison::CaseSensitive));
        CHECK(output.output.View().Contains("B.spark", StringComparison::CaseSensitive));
        CHECK(output.output.View().Contains("RET", StringComparison::CaseSensitive));
    }

    {
        // 진단이 있으면 IR이 없다(머리 줄 뒤 안내), 종료 코드 1.
        REQUIRE(temp.Write("Pkg/A.spark", "fn F(): int {\n    return true;\n}\n"));
        BufferOutput output(allocator);
        const StringView arguments[] = {"dump", "--ir", folder.View()};
        CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
              ExitFailure);
        CHECK(output.output.View().Contains("(no dump: the module stopped before this stage)",
                                            StringComparison::CaseSensitive));
    }
}

TEST(SparksTools_CommandAllocationFailure) {
    // check·dump의 모든 할당 실패 자리: ExitError로 끝나고 새는 할당이 없다.
    SystemAllocator backing;
    TempFolder temp(backing);
    REQUIRE(temp.Write("Pkg/SparksPackage.json5", Manifest));
    REQUIRE(temp.Write("Pkg/Main.spark", "import Lib;\n\nfn F(): int {\n    return G();\n}\n"));
    REQUIRE(temp.Write("Pkg/Lib.spark", "fn G(): int {\n    return 1;\n}\n"));
    REQUIRE(temp.Write("Pkg/Bad.spark", "import Nope;\n"));
    const String folder = temp.PathOf("Pkg");
    const StringView commands[][3] = {{"check", folder.View(), ""},
                                      {"dump", "--ir", folder.View()}};
    for (const auto& command : commands) {
        const Span<const StringView> arguments(command, command[2].IsEmpty() ? 2 : 3);
        uint64 attempts = 0;
        int32 expected = 0;
        uint64 failures = 0;
        for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
            test::NFailAllocator allocator(backing);
            const uint64 before = backing.Stats().liveAllocations;
            {
                BufferOutput output(backing); // 출력은 세는 allocator 밖에 둔다
                if (failAt != 0) {
                    allocator.FailOnAttempt(failAt);
                }

                const int32 code =
                    RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output);
                const uint64 used = allocator.Attempts();
                allocator.DisableFailure();
                if (failAt == 0) {
                    expected = code;
                    attempts = used;
                    CHECK(code == ExitFailure);
                } else if (code == ExitError) {
                    ++failures;
                } else {
                    CHECK(code == expected);
                }
            }

            CHECK(backing.Stats().liveAllocations == before);
        }

        CHECK(failures > 0);
    }
}

namespace {

// 폴더 항목 이름을 모은다(이름 byte 순).
struct Listed {
    explicit Listed(Allocator& allocator) noexcept : names(allocator), folders(allocator) {
    }

    Array<String> names;
    Array<uint8> folders;
};

bool ListFolder(Allocator& allocator, StringView path, Listed& out) noexcept {
    struct Context {
        Allocator* allocator;
        Listed* out;
        bool ok;
    } context{&allocator, &out, true};
    auto collect = [](const platform::DirectoryEntry& entry, void* raw) noexcept {
        auto& list = *static_cast<Context*>(raw);
        String name(*list.allocator);
        list.ok =
            list.ok && name.TryAssign(StringView(entry.name, entry.nameBytes)) &&
            list.out->names.TryPushBack(Move(name)) &&
            list.out->folders.TryPushBack(entry.info.type == platform::FileType::Directory ? 1 : 0);
        return list.ok;
    };
    if (!platform::PhysicalFileSystem().EnumerateDirectory(path.Data(), path.ByteCount(), collect,
                                                           &context) ||
        !context.ok) {
        return false;
    }

    return true;
}

bool ReadText(Allocator& allocator, StringView path, String& out) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    Array<char> bytes(allocator);
    const bool ok = file.Size(size) && bytes.TryResize(static_cast<usize>(size)) &&
                    file.ReadExact(bytes.Data(), bytes.Size()).status;
    const bool closed = static_cast<bool>(file.Close());
    return ok && closed && out.TryAssign(StringView(bytes.Data(), bytes.Size()));
}

// `.expect`의 진단 지시를 `error <코드>[ <경로>][ <줄:열>]` 줄로 모은다. stage 지시가 있으면 false
// (CLI는 모든 단계를 컴파일한다 — 비교하지 않는다).
bool ExpectedLines(StringView expect, Array<String>& lines, Allocator& allocator) noexcept {
    usize at = 0;
    while (at < expect.ByteCount()) {
        usize end = at;
        while (end < expect.ByteCount() && expect.Data()[end] != '\n') {
            ++end;
        }

        StringView line(expect.Data() + at, end - at);
        at = end + 1;
        while (!line.IsEmpty() && (line.Data()[line.ByteCount() - 1] == '\r' ||
                                   line.Data()[line.ByteCount() - 1] == ' ')) {
            line = StringView(line.Data(), line.ByteCount() - 1);
        }

        if (line.IsEmpty() || line.StartsWith("#") || line == "clean") {
            continue;
        }

        if (line.StartsWith("stage ")) {
            return false;
        }

        String copy(allocator);
        if (!copy.TryAssign(line) || !lines.TryPushBack(Move(copy))) {
            return false;
        }
    }

    return true;
}

// CLI 출력의 진단 첫 줄(`<경로>:<줄>:<열>: error <코드>:` 또는 `<경로>: error <코드>:`)을
// `.expect` 줄 모양으로 바꿔 모은다. prefix(사례 폴더 표기 + `/`)는 떼고, schema 폴더 경로는
// `Schema/`로, 파일 사례(folder false)는 경로를 뺀다.
bool ActualLines(StringView output, StringView prefix, StringView schemaPrefix, bool folder,
                 Array<String>& lines, Allocator& allocator) noexcept {
    usize at = 0;
    while (at < output.ByteCount()) {
        usize end = at;
        while (end < output.ByteCount() && output.Data()[end] != '\n') {
            ++end;
        }

        const StringView line(output.Data() + at, end - at);
        at = end + 1;
        const bool caseFile = line.StartsWith(prefix);
        const bool schemaFile = line.StartsWith(schemaPrefix);
        if (!caseFile && !schemaFile) {
            continue;
        }

        const StringView rest(line.Data() + (caseFile ? prefix : schemaPrefix).ByteCount(),
                              line.ByteCount() - (caseFile ? prefix : schemaPrefix).ByteCount());
        usize marker = 0;
        const StringView error = ": error ";
        while (marker + error.ByteCount() <= rest.ByteCount() &&
               StringView(rest.Data() + marker, error.ByteCount()) != error) {
            ++marker;
        }

        if (marker + error.ByteCount() > rest.ByteCount()) {
            continue;
        }

        // 위치(`경로:줄:열` 또는 `경로`)와 코드.
        const StringView location(rest.Data(), marker);
        const StringView code(rest.Data() + marker + error.ByteCount(), 5);
        usize colon = location.ByteCount();
        uint32 colons = 0;
        for (usize index = location.ByteCount(); index > 0 && colons < 2; --index) {
            if (location.Data()[index - 1] == ':') {
                colon = index - 1;
                ++colons;
            }
        }

        const bool positioned = colons == 2;
        const StringView path(location.Data(), positioned ? colon : location.ByteCount());
        String text(allocator);
        bool ok = text.TryAppend("error ") && text.TryAppend(code);
        if (schemaFile) {
            ok = ok && text.TryAppend(" Schema/") && text.TryAppend(path);
        } else if (folder) {
            ok = ok && text.TryAppend(" ") && text.TryAppend(path);
        }

        if (positioned) {
            ok = ok && text.TryAppend(" ") &&
                 text.TryAppend(
                     StringView(location.Data() + colon + 1, location.ByteCount() - colon - 1));
        }

        if (!ok || !lines.TryPushBack(Move(text))) {
            return false;
        }
    }

    return true;
}

} // namespace

TEST(SparksTools_CheckMatchesConformanceCorpus) {
    // CLI와 conformance의 진단 동등성(Docs/Design/SparksTools.ko.md 5절): corpus의 폴더 사례는 그
    // 폴더를, 파일 사례는 임시 패키지(conformance 파일 사례와 같은 manifest·모듈 경로 Main.spark)를
    // `sparks check`로 돌려 진단 열(코드·경로·위치)이 `.expect`와 같은지 본다. stage 지시가 있는
    // 사례와 reload revision 원문은 뺀다.
    SystemAllocator allocator;
    {
        constexpr usize PathCapacity = 4096;
        char engine[PathCapacity];
        auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
        REQUIRE(found);
        auto corpus = Path::Combine(allocator, StringView(engine, found.Value()),
                                    "Source/Developer/SparksTestHost/Conformance");
        auto schema = Path::Combine(allocator, StringView(engine, found.Value()),
                                    "Source/Developer/SparksTestHost/Schema");
        REQUIRE(corpus);
        REQUIRE(schema);
        const String schemaShown = Slashed(allocator, schema.Value().View());
        StringBuilder<512> schemaPrefix(allocator);
        REQUIRE(schemaPrefix.TryAppendFormat("{}/", schemaShown.View()));
        TempFolder temp(allocator);
        REQUIRE(temp.Write("Case/SparksPackage.json5",
                           "{ name: \"Test\", sparks: 1, profile: \"full\" }\n"));
        Listed areas(allocator);
        REQUIRE(ListFolder(allocator, corpus.Value().View(), areas));
        uint32 compared = 0;
        uint32 mismatched = 0;
        for (usize area = 0; area < areas.names.Size(); ++area) {
            if (areas.folders[area] == 0) {
                continue;
            }

            auto areaPath =
                Path::Combine(allocator, corpus.Value().View(), areas.names[area].View());
            REQUIRE(areaPath);
            Listed items(allocator);
            REQUIRE(ListFolder(allocator, areaPath.Value().View(), items));
            for (usize at = 0; at < items.names.Size(); ++at) {
                const StringView name = items.names[at].View();
                const bool folder = items.folders[at] != 0;
                const bool file = !folder && name.EndsWith(".spark");
                if (!folder && !file) {
                    continue;
                }

                // 사례 stem과 `.expect`(없으면 revision 원문·폴더 — 빼낸다).
                const StringView stem(name.Data(), file ? name.ByteCount() - 6 : name.ByteCount());
                StringBuilder<256> expectName(allocator);
                REQUIRE(expectName.TryAppendFormat("{}.expect", stem));
                auto expectPath =
                    Path::Combine(allocator, areaPath.Value().View(), expectName.View());
                REQUIRE(expectPath);
                String expect(allocator);
                if (!ReadText(allocator, expectPath.Value().View(), expect)) {
                    continue;
                }

                Array<String> expected(allocator);
                if (!ExpectedLines(expect.View(), expected, allocator)) {
                    continue; // stage 지시
                }

                auto casePath = Path::Combine(allocator, areaPath.Value().View(), name);
                REQUIRE(casePath);
                String target(allocator);
                if (file) {
                    String source(allocator);
                    REQUIRE(ReadText(allocator, casePath.Value().View(), source));
                    REQUIRE(temp.Write("Case/Main.spark", source.View()));
                    target = temp.PathOf("Case");
                } else {
                    REQUIRE(target.TryAssign(casePath.Value().View()));
                }

                const String shown = Slashed(allocator, target.View());
                StringBuilder<512> prefix(allocator);
                REQUIRE(prefix.TryAppendFormat("{}/", shown.View()));
                BufferOutput output(allocator);
                const StringView arguments[] = {"check", "--schema", schema.Value().View(),
                                                target.View()};
                const int32 code =
                    RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output);
                Array<String> actual(allocator);
                REQUIRE(ActualLines(output.output.View(), prefix.View(), schemaPrefix.View(),
                                    folder, actual, allocator));
                bool same = actual.Size() == expected.Size() &&
                            code == (expected.IsEmpty() ? ExitSuccess : ExitFailure);
                for (usize index = 0; same && index < actual.Size(); ++index) {
                    same = actual[index].View() == expected[index].View();
                }

                ++compared;
                if (!same) {
                    ++mismatched;
                    StringBuilder<1024> report(allocator);
                    report.AppendFormat("CLI differs from the corpus: {}/{} (exit {})\n",
                                        areas.names[area].View(), name, code);
                    for (const String& line : expected) {
                        report.AppendFormat("  expected {}\n", line.View());
                    }

                    for (const String& line : actual) {
                        report.AppendFormat("  actual   {}\n", line.View());
                    }

                    if (!output.error.IsEmpty()) {
                        report.AppendFormat("  stderr   {}", output.error.View());
                    }

                    platform::WriteDiagnostic(report.View().Data(), report.View().ByteCount());
                }
            }
        }

        CHECK(compared > 0);
        CHECK(mismatched == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace sparks_tools_command_tests_file
