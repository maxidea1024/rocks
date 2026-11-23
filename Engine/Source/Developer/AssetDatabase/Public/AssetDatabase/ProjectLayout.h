#pragma once

#include <AssetDatabase/LogicalPath.h>
#include <Base/Compiler.h>

// 프로젝트 디렉터리 배치와 논리 경로 ↔ 파일 경로
// 대응이다(Docs/Design/AssetSystem 4.2절, 7.1절 10 — A2a). 디렉터리 이름은 UE와 같다:
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

// 절대 OS 경로의 byte 상한이다(UTF-8 byte로 센다 — 끝 NUL은 넣지 않는다). Windows는 긴 경로 32767
// UTF-16 단위에서 NUL과 PlatformBase가 붙이는 UNC 긴 경로 접두의 실질 증가(6)를 뺀 값이고(UTF-8
// byte 수 ≥ UTF-16 단위 수라 보수적이다), POSIX PATH_MAX(macOS 1024, 그 밖 4096)는 NUL을 포함하므로
// 1을 뺀다.
inline constexpr usize WindowsLongPathUnits = 32767;
inline constexpr usize WindowsUncPrefixGrowth = 6;
inline constexpr usize MacPathMax = 1024;
inline constexpr usize PosixPathMax = 4096;

constexpr usize MaxNativePathBytes() noexcept {
    if constexpr (IsWindows) {
        return WindowsLongPathUnits - 1 - WindowsUncPrefixGrowth;
    } else if constexpr (IsMac) {
        return MacPathMax - 1;
    } else {
        return PosixPathMax - 1;
    }
}

class ROCKS_ASSETDATABASE_API ProjectLayout {
public:
    explicit ProjectLayout(Allocator& allocator) noexcept;

    ProjectLayout(const ProjectLayout&) = delete;
    ProjectLayout& operator=(const ProjectLayout&) = delete;

    // root(프로젝트 디렉터리 — `*.rocksproject`가 있는 곳, HostConfig FindProjectFrom이 찾는다)로
    // 디렉터리를 정한다. 상대 경로는 현재 작업 디렉터리 기준 절대 경로로 바꾸고(UE
    // FPaths::ConvertRelativePathToFull), 정규화한 뒤 구분자를 '/'로 쓴다. 루트뿐인
    // 경로("C:/"·"/")는 그대로 두고 자식을 그 뒤에 붙인다. 디스크는 보지 않는다. 다시 부르면
    // 바꾼다. 오류: 빈 root, Windows 현재 드라이브 루트("/x")·드라이브 상대("C:x")는
    // InvalidArgument, 작업 디렉터리 조회 실패는 그 파일 오류, 할당 실패(실패하면 이전 값 그대로).
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
    // 보고한다), Initialize 전이면 InvalidArgument, 할당 실패.
    Result<void> TryMakeFilePath(const LogicalPath& path, String& out) const noexcept;

    // Content 아래 상대 이름(열거·감시가 준 byte 그대로, 구분자 '/', 확장자 포함)을 논리 경로로
    // 바꾼다. 확장자가 `.rasset`(ASCII 대소문자 무시 — UE와 같다)이 아니면 InvalidArgument(failure
    // None — 패키지 파일이 아니다), 이름이 NFC가 아니고 FileSystemNormalizesNames가 거짓이면
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
