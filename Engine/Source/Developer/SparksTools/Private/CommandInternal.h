#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksTools/SparksCommand.h>
#include "Workspace.h"

// `sparks` 명령들이 함께 쓰는 인자·작업 공간 상태다(Docs/Design/SparksTools.ko.md 2절·7절).
namespace rocks::sparks::tools::detail {

// 출력 builder의 inline 크기다(넘치면 allocator에서 늘린다).
inline constexpr usize OutputInlineBytes = 4096;

enum class DumpKind : uint8 {
    None,
    Syntax,
    Names,
    Types,
    Ir,
    Bytecode
};

// 명령 인자 분해 결과다(T5).
struct CommandArguments {
    explicit CommandArguments(Allocator& allocator) noexcept
        : schemas(allocator), paths(allocator) {
    }

    StringView command;
    Array<StringView> schemas;
    Array<StringView> paths;
    DumpKind dump = DumpKind::None;
    StringView out; // build의 `--out`
    bool hasOut = false;
    bool check = false; // fmt의 `--check`
};

// 명령 하나의 작업 공간과 그 컴파일이다.
struct CommandContext {
    CommandContext(Allocator& allocator, platform::FileSystem& files,
                   const CommandArguments& arguments, CommandOutput& output) noexcept
        : allocator(allocator), files(files), arguments(arguments), output(output),
          workspace(allocator), sources(allocator), diagnostics(allocator),
          compilation(allocator, sources), modules(allocator), packages(allocator) {
    }

    Allocator& allocator;
    platform::FileSystem& files;
    const CommandArguments& arguments;
    CommandOutput& output;
    Workspace workspace;
    SourceSet sources;
    DiagnosticList diagnostics;
    Compilation compilation;
    Array<uint32> modules;  // 작업 공간 파일마다 모듈 번호(모듈이 아니면 NoModule)
    Array<uint32> packages; // 작업 공간 패키지마다 compilation 패키지 번호
};

// 모듈 파일 번호(Workspace::Files 색인)를 진단 경로 byte 순으로 모은다. selectedOnly면 인자로 고른
// 모듈(파일 인자 그 자신, 폴더 인자 아래)만. 실패: 할당 실패.
Result<void> ModuleOrder(const Workspace& workspace, bool selectedOnly,
                         Array<usize>& order) noexcept;

// 요약 줄 `N errors in M modules`를 붙인다.
void AppendSummary(StringBuilderBase& text, const CommandContext& context) noexcept;

// 표준 오류에 `<path>: <message>` 한 줄을 쓰고 code를 돌려준다(path가 비면 `sparks: <message>`,
// 쓰기·할당 실패면 ExitError).
int32 ReportProblem(CommandContext& context, StringView path, StringView message,
                    int32 code) noexcept;

// 명령 본문이다. text에는 진단이 붙어 있고 본문이 표준 출력 내용을 더 붙인다. 종료 코드를
// 돌려준다.
int32 RunBuild(CommandContext& context, StringBuilderBase& text) noexcept;
int32 RunIds(CommandContext& context, StringBuilderBase& text) noexcept;
int32 RunFmt(CommandContext& context, StringBuilderBase& text) noexcept;

} // namespace rocks::sparks::tools::detail
