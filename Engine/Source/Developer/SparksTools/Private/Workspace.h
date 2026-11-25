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
    bool selected = false; // 인자로 고른 모듈(파일 인자 그 자신, 폴더 인자 아래 — dump·fmt의 대상)
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

    // 열린 문서 내용으로 덮는다(언어 서버 — Docs/Design/SparksTools.ko.md 10.4절). key는 경로 비교
    // key(LspPathKey — `/` 구분·드라이브 소문자)다. 같은 경로의 파일이 있으면 그 내용을 바꾸고,
    // 없고
    // `.spark`가 어떤 패키지 뿌리 아래면 그 패키지 모듈로 더한다(저장 전 새 파일). 값은 덮었는지다.
    // fileKey는 Files()의 파일마다 비교 key를 만드는 함수다. 실패: 할당 실패.
    template <typename KeyOf>
    Result<bool> Overlay(StringView key, StringView text, KeyOf&& fileKey) noexcept;

    // 패키지의 ID manifest 파일 번호다(Files() 색인). 없으면 NoPackage.
    uint32 IdsFile(uint32 package) const noexcept;

private:
    Result<bool> AddOverlayModule(uint32 package, StringView relative, StringView full,
                                  StringView text) noexcept;
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

template <typename KeyOf>
Result<bool> Workspace::Overlay(StringView key, StringView text, KeyOf&& fileKey) noexcept {
    String other(*allocator_);
    for (WorkspaceFile& file : files_) {
        if (auto made = fileKey(file.full.View(), other); !made) {
            return Result<bool>::Failure(made.Error());
        }

        if (other.View() == key) {
            file.text.Clear();
            if (auto appended =
                    file.text.TryAppend(Span<const char>(text.Data(), text.ByteCount()));
                !appended) {
                return Result<bool>::Failure(appended.Error());
            }

            return Result<bool>::Success(true);
        }
    }

    if (!key.EndsWith(SourceExtension, StringComparison::CaseSensitive)) {
        return Result<bool>::Success(false);
    }

    // 가장 안쪽(뿌리 key가 가장 긴) 패키지다 — 중첩 패키지의 파일은 안쪽 패키지 모듈이다.
    uint32 owner = NoPackage;
    usize ownerBytes = 0;
    for (uint32 package = 0; package < roots_.Size(); ++package) {
        if (auto made = fileKey(roots_[package].View(), other); !made) {
            return Result<bool>::Failure(made.Error());
        }

        if (key.ByteCount() > other.ByteCount() + 1 &&
            key.StartsWith(other.View(), StringComparison::CaseSensitive) &&
            key.Data()[other.ByteCount()] == '/' &&
            (owner == NoPackage || other.ByteCount() > ownerBytes)) {
            owner = package;
            ownerBytes = other.ByteCount();
        }
    }

    if (owner == NoPackage) {
        return Result<bool>::Success(false);
    }

    // 작업 공간 읽기가 건너뛰는 `.` 폴더·파일 아래는 모듈이 아니다.
    const StringView relative(key.Data() + ownerBytes + 1, key.ByteCount() - ownerBytes - 1);
    for (usize at = 0; at < relative.ByteCount(); ++at) {
        if (relative.Data()[at] == '.' && (at == 0 || relative.Data()[at - 1] == '/')) {
            return Result<bool>::Success(false);
        }
    }

    return AddOverlayModule(owner, relative, key, text);
}

} // namespace rocks::sparks::tools::detail
