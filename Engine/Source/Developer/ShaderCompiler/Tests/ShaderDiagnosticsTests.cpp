#include "ShaderSourceTestSupport.h"

#include <Base/InitializerList.h>
#include <Core/Text/String.h>
#include <ShaderCompiler/ShaderDiagnostics.h>
#include <ShaderCompiler/ShaderSourceTree.h>
#include <TestSupport/Test.h>

// 셰이더 진단 위치 해석(Docs/Design/ShaderSystem 16.3절 SSD72)과 감시·로그 경로(SSD69·SSD72)
// 시험이다. 진단 원문은 실제 도구(DXC·FXC·전처리기·관리자 머리말)가 내는 꼴 그대로다.
using namespace rocks;
namespace shader_diagnostics_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;

namespace {

bool Is(const ShaderDiagnostic& diagnostic, StringView path, uint32 line, uint32 column,
        ShaderDiagnosticSeverity severity, StringView message) noexcept {
    const bool same = diagnostic.virtualPath.View() == path && diagnostic.line == line &&
                      diagnostic.column == column && diagnostic.severity == severity &&
                      diagnostic.message.View() == message;
    if (!same) {
        fprintf(stderr, "diagnostic '%s' %u:%u %s '%s'\n", diagnostic.virtualPath.CStr(),
                diagnostic.line, diagnostic.column,
                ShaderDiagnosticSeverityName(diagnostic.severity), diagnostic.message.CStr());
    }

    return same;
}

} // namespace

// DXC(clang 꼴)·FXC(MSVC 꼴, 열 범위)·Rocks 전처리기 꼴과 관리자 머리말("DXC compile failed: "),
// 이어지는 원문 줄·'^' 표지, 위치 없는 줄, 심각도 낱말.
TEST(ShaderDiagnostics_ParsesToolLocations) {
    const StringView text =
        "DXC compile failed: /Plugin/SandboxReload/ReloadColor.rsh:3:21: error: use of undeclared "
        "identifier 'colr'\n"
        "#define RELOAD_COLOR colr\n"
        "                    ^\n"
        "FXC: /Engine/Private/Utility/CopyTexture.rsf(12,5-9): error X3004: undeclared identifier "
        "'uvv'\r\n"
        "/Engine/Public/Platform.rsh:40: UnterminatedComment: comment is not closed\n"
        "/Project/A.rsf(7): warning X3206: implicit truncation of vector type\n"
        "/Project/A.rsf:9:1: note: candidate function\n"
        "\n"
        "fallback (DXC, SPIR-V, HLSL 5.0, FXC): DXC compile failed: /Project/B.rsh:2:3: fatal "
        "error: 'x.rsh' file not found\n";
    Array<ShaderDiagnostic> diagnostics(TestAllocator());
    REQUIRE(ParseShaderDiagnostics(text, diagnostics));
    REQUIRE(diagnostics.Size() == 6);
    CHECK(Is(diagnostics[0], "/Plugin/SandboxReload/ReloadColor.rsh", 3, 21,
             ShaderDiagnosticSeverity::Error, "use of undeclared identifier 'colr'"));
    CHECK(diagnostics[0].context.View() ==
          StringView("#define RELOAD_COLOR colr\n                    ^"));
    CHECK(Is(diagnostics[1], "/Engine/Private/Utility/CopyTexture.rsf", 12, 5,
             ShaderDiagnosticSeverity::Error, "X3004: undeclared identifier 'uvv'"));
    CHECK(Is(diagnostics[2], "/Engine/Public/Platform.rsh", 40, 0, ShaderDiagnosticSeverity::Error,
             "UnterminatedComment: comment is not closed"));
    CHECK(Is(diagnostics[3], "/Project/A.rsf", 7, 0, ShaderDiagnosticSeverity::Warning,
             "X3206: implicit truncation of vector type"));
    CHECK(Is(diagnostics[4], "/Project/A.rsf", 9, 1, ShaderDiagnosticSeverity::Note,
             "candidate function"));
    CHECK(Is(diagnostics[5], "/Project/B.rsh", 2, 3, ShaderDiagnosticSeverity::Error,
             "'x.rsh' file not found"));
    for (const ShaderDiagnostic& diagnostic : diagnostics) {
        CHECK(diagnostic.occurrences == 1);
    }

    // 위치가 없으면 위치 없는 진단 하나와 그 이어지는 줄이다. 가상 경로가 아닌 경로(OS 경로·규칙
    // 위반)·줄 번호 없는 콜론·자리 수가 넘는 숫자는 위치가 아니다.
    Array<ShaderDiagnostic> plain(TestAllocator());
    REQUIRE(ParseShaderDiagnostics("ShaderCompileWorker exited with code 3\n"
                                   "C:/Temp/x.rsf:3:4: error: os path\n"
                                   "/Engine/Bad Name.rsf:3: error: space\n"
                                   "/Engine/A.txt:3: error: extension\n"
                                   "/Engine/A.rsf: error: no line\n"
                                   "/Engine/A.rsf:1234567890: error: too many digits\n",
                                   plain));
    REQUIRE(plain.Size() == 1);
    CHECK(Is(plain[0], "", 0, 0, ShaderDiagnosticSeverity::Error,
             "ShaderCompileWorker exited with code 3"));
    // 이어지는 줄은 MaxShaderDiagnosticContextLines까지다.
    uint32 lines = 1;
    for (const char c : plain[0].context.View()) {
        lines += c == '\n' ? 1u : 0u;
    }

    CHECK(lines == MaxShaderDiagnosticContextLines);

    // 문구 없는 위치 줄(include 머리)은 앞 진단의 이어지는 줄이다. 빈 원문은 진단이 없다.
    Array<ShaderDiagnostic> included(TestAllocator());
    REQUIRE(ParseShaderDiagnostics("/Engine/A.rsf:4:2: error: bad\n"
                                   "In file included from /Engine/B.rsh:3:\n",
                                   included));
    REQUIRE(included.Size() == 1);
    CHECK(included[0].context.View() == StringView("In file included from /Engine/B.rsh:3:"));
    Array<ShaderDiagnostic> empty(TestAllocator());
    REQUIRE(ParseShaderDiagnostics("", empty));
    REQUIRE(ParseShaderDiagnostics("\n\n  \n", empty));
    CHECK(empty.IsEmpty());
}

// 형식·퍼뮤테이션마다 반복되는 같은 진단은 하나로 합쳐 센다(처음 나온 순서 유지). 줄·열·심각도·
// 문구 가운데 하나라도 다르면 따로다.
TEST(ShaderDiagnostics_MergesRepeatedDiagnostics) {
    Array<ShaderDiagnostic> merged(TestAllocator());
    const StringView jobs[] = {
        "/Engine/A.rsf:3:7: error: bad\n/Engine/A.rsf:5:1: error: worse\n",
        "/Engine/A.rsf:3:7: error: bad\n",
        "/Engine/A.rsf:3:8: error: bad\n/Engine/A.rsf:3:7: warning: bad\n",
    };
    for (const StringView job : jobs) {
        Array<ShaderDiagnostic> parsed(TestAllocator());
        REQUIRE(ParseShaderDiagnostics(job, parsed));
        REQUIRE(MergeShaderDiagnostics(merged, parsed));
        CHECK(parsed.IsEmpty());
    }

    REQUIRE(merged.Size() == 4);
    CHECK(Is(merged[0], "/Engine/A.rsf", 3, 7, ShaderDiagnosticSeverity::Error, "bad") &&
          merged[0].occurrences == 2);
    CHECK(Is(merged[1], "/Engine/A.rsf", 5, 1, ShaderDiagnosticSeverity::Error, "worse") &&
          merged[1].occurrences == 1);
    CHECK(Is(merged[2], "/Engine/A.rsf", 3, 8, ShaderDiagnosticSeverity::Error, "bad"));
    CHECK(Is(merged[3], "/Engine/A.rsf", 3, 7, ShaderDiagnosticSeverity::Warning, "bad"));
}

// 로그 한 줄은 편집기에서 눌러 이동하는 MSVC 꼴이다(UE GetErrorStringWithSourceLocation).
TEST(ShaderDiagnostics_FormatsEditorLocations) {
    Array<ShaderDiagnostic> diagnostics(TestAllocator());
    REQUIRE(ParseShaderDiagnostics("/Engine/A.rsf:3:7: error: bad\n"
                                   "/Engine/A.rsf(9): warning X1: shady\n"
                                   "loose text\n",
                                   diagnostics));
    REQUIRE(diagnostics.Size() == 2);
    String line(TestAllocator());
    REQUIRE(FormatShaderDiagnostic(diagnostics[0], "C:/Rocks/Engine/Shaders/A.rsf", line));
    CHECK(line.View() == StringView("C:/Rocks/Engine/Shaders/A.rsf(3,7): error: bad"));
    REQUIRE(FormatShaderDiagnostic(diagnostics[0], StringView(), line));
    CHECK(line.View() == StringView("/Engine/A.rsf(3,7): error: bad"));
    REQUIRE(FormatShaderDiagnostic(diagnostics[1], StringView(), line));
    CHECK(line.View() == StringView("/Engine/A.rsf(9): warning: X1: shady"));
    CHECK(diagnostics[1].context.View() == StringView("loose text"));

    Array<ShaderDiagnostic> plain(TestAllocator());
    REQUIRE(ParseShaderDiagnostics("worker failed", plain));
    REQUIRE(FormatShaderDiagnostic(plain[0], "ignored", line));
    CHECK(line.View() == StringView("error: worker failed"));
    CHECK(StringView(ShaderDiagnosticSeverityName(ShaderDiagnosticSeverity::Note)) == "note");
}

// 가상 경로 → 디스크 경로(mount 실제 디렉터리 + 상대 경로), 메모리 원문·mount 밖·규칙 위반은 없음.
// 감시 이벤트 경로는 원문 확장자만 가상 경로가 된다.
TEST(ShaderSourceTree_ResolvesDiskPathsAndWatchedPaths) {
    Fixture fixture;
    REQUIRE(fixture.Make("diskpath"));
    ShaderSourceTree tree(TestAllocator());
    REQUIRE(MountFixture(fixture, tree));
    char plugin[FixturePathBytes];
    REQUIRE(fixture.Child("Plugin", plugin, sizeof(plugin)));
    CHECK(tree.MountCount() == 2 && tree.MountedDirectory(1) == StringView(FixturePlugin) &&
          tree.MountedRealDirectory(1) == StringView(plugin));

    String disk(TestAllocator());
    REQUIRE(tree.ResolveDiskPath("/Plugin/Test/Deep/A.rsh", disk));
    char expected[FixturePathBytes];
    snprintf(expected, sizeof(expected), "%s/Deep/A.rsh", plugin);
    CHECK(disk.View() == StringView(expected));
    CHECK(tree.ResolveDiskPath("/Plugin/Test/Generated/X.rsh", disk).Error() ==
          ErrorCode::NotFound);
    CHECK(disk.IsEmpty());
    CHECK(tree.ResolveDiskPath("/Project/A.rsf", disk).Error() == ErrorCode::NotFound);
    CHECK(tree.ResolveDiskPath("/Plugin/Test/../A.rsf", disk).Error() ==
          ErrorCode::InvalidArgument);

    String watched(TestAllocator());
    CHECK(MakeWatchedShaderPath("/Plugin/Test", "Deep/A.rsh", watched) &&
          watched.View() == StringView("/Plugin/Test/Deep/A.rsh"));
    CHECK(MakeWatchedShaderPath("/Engine", "Private/Utility/Copy.rsf", watched) &&
          watched.View() == StringView("/Engine/Private/Utility/Copy.rsf"));
    CHECK(MakeWatchedShaderPath("/Engine", "Shared/Slots.h", watched));
    // 원문 확장자가 아니거나(임시 파일·디렉터리) 규칙에 맞지 않으면 없다.
    for (const char* relative :
         {"Private/A.rsh.123.tmp", "Private", "Private/A.RSF", "Private/A b.rsf", "", "A.rsh~"}) {
        CHECK(!MakeWatchedShaderPath("/Engine", relative, watched) && watched.IsEmpty());
    }
}

} // namespace shader_diagnostics_tests_file
