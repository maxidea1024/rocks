#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <SparksCompiler/Compilation.h>

// 작업 공간 적재다(Docs/Design/SparksTools.ko.md T3): 명령 인자 경로에서 패키지 뿌리를 찾아 그
// 아래 원문·manifest·ID manifest를, schema 폴더에서 binding schema를 읽는다.
namespace rocks::sparks::tools::detail {

// 폴더 훑기 깊이 상한이다(링크 순환·지나친 중첩 방어).
inline constexpr uint32 MaxWorkspaceDepth = 32;

// 파일 하나의 byte 상한이다. 원문 상한보다 하나 크게 읽어 컴파일러가 크기 초과(S0103)를 내게 한다.
inline constexpr uint64 MaxWorkspaceFileBytes = uint64(MaxSourceBytes) + 1;

inline constexpr uint32 NoPackage = NumericLimits<uint32>::Max();

// 모듈 원문 파일 이름의 끝이다.
inline constexpr StringView SourceExtension = ".spark";

// ID manifest 파일 이름의 끝이다(`<패키지>.rocksids.json`).
inline constexpr StringView IdsExtension = ".rocksids.json";

enum class WorkspaceFileKind : uint8 {
    Schema,
    Manifest,
    Ids,
    Module
};

// 읽은 파일 하나다.
struct WorkspaceFile {
    explicit WorkspaceFile(Allocator& allocator) noexcept
        : path(allocator), full(allocator), modulePath(allocator), text(allocator) {
    }

    WorkspaceFileKind kind = WorkspaceFileKind::Module;
    uint32 package = NoPackage; // Manifest·Ids·Module의 패키지 번호
    String path;                // 진단 경로(인자 경로 기준 — `/` 구분)
    String full;                // 파일 시스템 경로(인자 경로에 이름을 이은 것)
    String modulePath;          // Module: 패키지 뿌리 기준 경로(`Combat/Score.spark`)
    Array<char> text;
    bool selected = false; // 파일 인자로 고른 모듈(dump가 그 모듈만 쓴다)
};

class Workspace final {
public:
    explicit Workspace(Allocator& allocator) noexcept;

    // paths(폴더 또는 `.spark` 파일 — 그 자신이나 조상 폴더에 `SparksPackage.json5`가 있으면 그
    // 패키지 뿌리를, 없으면 그 폴더 아래 패키지들을)와 schemaFolders의 `*.sparkbind.json`을 읽는다.
    // 배치·입출력 문제는 problem에 한 문장을 쓰고 false(read는 입출력 문제면 true), 할당 실패는
    // Result 실패다.
    Result<bool> Load(platform::FileSystem& files, Span<const StringView> paths,
                      Span<const StringView> schemaFolders, StringBuilderBase& problem,
                      bool& ioFailure) noexcept;

    // 읽은 파일을 compilation에 더한다. modules는 파일마다 모듈 번호(Module이 아니면 NoModule),
    // packages는 작업 공간 패키지마다 compilation 패키지 번호다. ids가 비어 있지 않으면 작업 공간
    // 패키지마다 ID manifest 원문을 바꿔 넣는다(IdsOverride::text가 빈 패키지는 그대로 — 파일이
    // 없던 패키지는 IdsOverride::path를 진단 경로로 새로 더한다 — `sparks ids`의 쓰기 전 확인).
    struct IdsOverride {
        StringView text;
        StringView path;
    };

    Result<void> AddTo(SourceSet& sources, Compilation& compilation, DiagnosticList& diagnostics,
                       Array<uint32>& modules, Array<uint32>& packages,
                       Span<const IdsOverride> ids = Span<const IdsOverride>()) noexcept;

    Span<const WorkspaceFile> Files() const noexcept {
        return files_.View();
    }

    uint32 PackageCount() const noexcept {
        return static_cast<uint32>(roots_.Size());
    }

    // 패키지 뿌리의 파일 시스템 경로와 진단 경로다(진단 경로는 지금 폴더면 빈 글자).
    StringView RootPath(uint32 package) const noexcept {
        return roots_[package].View();
    }

    StringView RootDisplay(uint32 package) const noexcept {
        return rootDisplays_[package].View();
    }

    // 패키지의 ID manifest 파일 번호다(Files() 색인). 없으면 NoPackage.
    uint32 IdsFile(uint32 package) const noexcept;

private:
    Result<bool> LoadPath(platform::FileSystem& files, StringView path, StringBuilderBase& problem,
                          bool& ioFailure) noexcept;
    Result<bool> Walk(platform::FileSystem& files, StringView full, StringView display,
                      uint32 depth, uint32 package, StringBuilderBase& problem,
                      bool& ioFailure) noexcept;
    Result<bool> ReadInto(platform::FileSystem& files, StringView full, WorkspaceFile& file,
                          StringBuilderBase& problem, bool& ioFailure) noexcept;

    Allocator* allocator_;
    Array<WorkspaceFile> files_;
    Array<String> roots_;        // 읽은 패키지 뿌리(입력 경로 그대로 — 중복 방지)
    Array<String> rootDisplays_; // 패키지마다 진단 경로 뿌리
};

} // namespace rocks::sparks::tools::detail
