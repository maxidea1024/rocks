#pragma once

#include <AssetDatabase/LogicalPath.h>
#include <Base/Compiler.h>

// 프로젝트 디렉터리 배치와 논리 경로 ↔ 파일 경로
// 대응이다(Docs/Design/AssetSystem.ko.md 4.2절, 7.1절 10 — A2a). 디렉터리 이름은 UE와 같다:
// `<루트>/Content`(마운트 `/Game`), `Config`, `Intermediate` (잠금·색인 캐시), `Saved`.
//
// 사용 예:
//   ProjectLayout layout(allocator);
//   if (!layout.Initialize("D:/Games/MyGame")) { ... }
//   auto path = ParseLogicalPath(allocator, "/Game/Characters/Hero").Value();
//   String file(allocator);
//   layout.TryMakeFilePath(path, file);   // "D:/Games/MyGame/Content/Characters/Hero.rasset"
namespace rocks {

// 에디터 패키지 파일 확장자다(4.2절).
inline constexpr StringView EditorPackageExtension = ".rasset";
// 저장 임시 파일 접미사 `.<guid 36자>.tmp`의 byte 수다(5.5절 2 — 경로 상한 검사에 넣는다).
inline constexpr usize TemporaryFileSuffixBytes = 1 + 36 + 4;
// 파일 시스템이 이름을 정규화해 돌려주는지다(macOS는 NFD로 저장하고 어느 형태로 열어도 같은 파일 —
// 그 밖은 byte로 비교한다). 열거한 비 NFC 이름을 받을지 정한다(7.1절 10).
inline constexpr bool FileSystemNormalizesNames = IsMac;

// 절대 OS 경로의 byte 상한이다(UTF-8 byte로 센다 — Windows 긴 경로 32767 UTF-16 단위보다 작거나
// 같은 보수적 판정, macOS PATH_MAX 1024, 그 밖 4096).
constexpr usize MaxNativePathBytes() noexcept {
    if constexpr (IsWindows) {
        return 32767;
    } else if constexpr (IsMac) {
        return 1024;
    } else {
        return 4096;
    }
}

class ProjectLayout {
public:
    explicit ProjectLayout(Allocator& allocator) noexcept;

    ProjectLayout(const ProjectLayout&) = delete;
    ProjectLayout& operator=(const ProjectLayout&) = delete;

    // root(프로젝트 디렉터리 — `*.rocksproject`가 있는 곳, HostConfig FindProjectFrom이 찾는다)로
    // 디렉터리를 정한다. '\'는 '/'로 바꾸고 끝 '/'는 뺀다. 디스크는 보지 않는다. 다시 부르면
    // 바꾼다. 오류: 빈 root InvalidArgument, 할당 실패(실패하면 이전 값 그대로).
    Result<void> Initialize(StringView root) noexcept;

    // 경로 문자열의 저장소다(이 배치에서 만드는 경로가 쓴다).
    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    StringView Root() const noexcept {
        return root_.View();
    }

    StringView ContentDirectory() const noexcept {
        return content_.View();
    }

    StringView ConfigDirectory() const noexcept {
        return config_.View();
    }

    StringView IntermediateDirectory() const noexcept {
        return intermediate_.View();
    }

    StringView SavedDirectory() const noexcept {
        return saved_.View();
    }

    // path의 새 에디터 패키지 파일 경로("<Content>/<Relative>.rasset")를 out에 쓴다(out을 바꾼다).
    // 이름은 path의 NFC 철자다 — 이미 있는 파일의 실제 이름은 색인이 가진다(7.1절 10). 오류: 임시
    // 접미사를 붙인 절대 경로가 MaxNativePathBytes를 넘으면 CapacityExceeded(호출자는 InvalidPath로
    // 보고한다), 할당 실패.
    Result<void> TryMakeFilePath(const LogicalPath& path, String& out) const noexcept;

    // Content 아래 상대 이름(열거·감시가 준 byte 그대로, 구분자 '/', 확장자 포함)을 논리 경로로
    // 바꾼다. 확장자가 `.rasset`(byte 일치)이 아니면 InvalidArgument(failure None — 패키지 파일이
    // 아니다), 이름이 NFC가 아니고 FileSystemNormalizesNames가 거짓이면
    // InvalidArgument(NotNfcOnDisk — 그 NFC 이름으로는 같은 파일을 열 수 없다), 그 밖은
    // ParseLogicalPath와 같다.
    Result<LogicalPath> TryMakeLogicalPath(StringView contentRelative,
                                           LogicalPathFailure* failure = nullptr) const noexcept;

private:
    Allocator* allocator_;
    String root_;
    String content_;
    String config_;
    String intermediate_;
    String saved_;
};

} // namespace rocks
