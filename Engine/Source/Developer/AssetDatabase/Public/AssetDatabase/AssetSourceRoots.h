#pragma once

#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/AssetDiagnostic.h>
#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>

// 임포트 원본 루트와 원본 읽기다(Docs/Design/AssetSystem.ko.md 4.5절, 7.1절 18 — A2f1a). 원본은
// 이름 있는 루트 기준 상대 경로로 기록해(ImportRecord sources[].root·relativePath) 다른 기기·
// 체크아웃 위치에서도 같은 원본을 찾는다. 루트 표는 `Config/AssetSources.json`(사용자가 손으로 쓰는
// 버전 관리 데이터 — 별칭 표와 같다)이고 이 모듈은 읽기만 한다.
namespace rocks {

// 원본 루트 표 파일 이름(Config 아래)과 기본 루트 이름이다.
inline constexpr StringView AssetSourceRootsFileName = "AssetSources.json";
inline constexpr StringView ProjectSourceRootName = "Project";
// 표 파일 크기·루트 수 상한이다.
inline constexpr uint64 MaxAssetSourceRootsFileBytes = 1024 * 1024;
inline constexpr usize MaxAssetSourceRoots = 256;
// 원본 하나와 한 임포트의 원본 합 byte 상한이다(원본은 통째로 메모리에 읽는다 — UE와 같다).
inline constexpr uint64 MaxImportSourceBytes = uint64{1} << 30;
inline constexpr uint64 MaxImportTotalSourceBytes = uint64{2} << 30;

// 루트 하나다: 이름과 정규화한 절대 경로(`/` 구분, 루트뿐인 경로 말고는 끝 `/` 없음).
struct AssetSourceRoot {
    explicit AssetSourceRoot(Allocator& allocator) noexcept
        : name(allocator), path(allocator), key(allocator) {
    }

    AssetSourceRoot(AssetSourceRoot&&) noexcept = default;
    AssetSourceRoot& operator=(AssetSourceRoot&&) noexcept = default;

    String name;
    String path;
    // path의 대소문자 무시 비교 키(TryMakeCaselessKey — 경로 정책과 같은 규칙)와 성분 수다.
    String key;
    usize components = 0;
};

// 원본 하나의 기록 위치다(ImportRecord sources[]의 root·relativePath). root가 비면 relative는
// 절대 경로다(absolute 참 — 경고 AbsoluteSourcePath).
struct AssetSourceLocation {
    explicit AssetSourceLocation(Allocator& allocator) noexcept
        : root(allocator), relative(allocator) {
    }

    String root;
    String relative;
    bool absolute = false;
};

class AssetSourceRoots {
public:
    AssetSourceRoots(Allocator& allocator, const ProjectLayout& layout) noexcept;

    AssetSourceRoots(const AssetSourceRoots&) = delete;
    AssetSourceRoots& operator=(const AssetSourceRoots&) = delete;

    // `Config/AssetSources.json` = `{"roots": {"<이름>": "<경로>"}}`를 읽는다. 이름은 1–
    // MaxImportRootBytes byte의 `[A-Za-z0-9_]`, 경로는 프로젝트 루트 기준 상대 또는 절대(`/`·OS
    // 구분자, `.`·`..` 정규화). 파일이 없거나 `Project`가 없으면 `Project` = 프로젝트 루트를
    // 더한다. 표는 이름 byte 순이다. 다시 부르면 바꾼다(실패하면 빈 표).
    //
    // 오류(failure 진단 InvalidSource — detail = JSON 오류면 (줄 << 32) | 열, 항목 오류면 항목 번호
    // + 1, 그 밖은 0): 읽기 IO 오류(진단 None — 그 오류), 크기 상한 CapacityExceeded, JSON 문법·
    // 키(정확히 "roots")·값 형식·이름 규칙·대소문자만 다른 두 이름(기본 Project 포함 — 이름은 byte
    // 그대로 비교한다)·빈 경로·Windows 드라이브 상대 경로 InvalidArgument, 장치 이름공간 경로
    // (`\\?\`·`\\.\`) Unsupported, 루트 수 상한 CapacityExceeded, 할당 실패(진단 None).
    //
    // 사용 예:
    //   AssetSourceRoots roots(allocator, layout);
    //   if (auto loaded = roots.Load(files, &diagnostic); !loaded) { /* 명령 실패 */ }
    Result<void> Load(platform::FileSystem& files, AssetDiagnostic* failure = nullptr) noexcept;

    Span<const AssetSourceRoot> Roots() const noexcept {
        return roots_.View();
    }

    // 절대 OS 경로 osPath의 기록 위치를 out에 쓴다: 정규화한 뒤 성분 단위로 가장 긴 접두 루트(같은
    // 길이면 이름 byte 순 첫째 — 대소문자 무시)를 고르고 그 아래 `/` 구분 상대 경로를 쓴다. 어느
    // 루트 아래도 아니면(루트 자신 포함) root 빈 문자열·relative = 정규화한 절대 경로·absolute 참.
    // 오류: 상대·Windows 드라이브 상대 경로·정규화 오류 InvalidArgument, 기록 경로가
    // MaxImportRelativePathBytes 초과 CapacityExceeded, 할당 실패.
    Result<void> Locate(StringView osPath, AssetSourceLocation& out) const noexcept;

    // 기록 위치를 지금 루트로 풀어 out에 쓴다(`/` 구분 절대 경로). 빈 root는 relative 그대로(기록
    // 규칙의 절대 경로여야 한다). 오류: 모르는 root NotFound, 기록 규칙 위반(ImportRecord와 같다)
    // InvalidArgument, 할당 실패.
    Result<void> Resolve(StringView root, StringView relative, String& out) const noexcept;

private:
    Allocator* allocator_;
    const ProjectLayout* layout_;
    Array<AssetSourceRoot> roots_;
};

// 임포트 원본 파일 path(절대 OS 경로)를 bytes에 통째로 읽고 SHA-256을 digest에 쓴다(4.5절, 7.1절
// 18). limit는 이 원본의 byte 상한이다(기본 MaxImportSourceBytes — 여러 원본은 남은 합계 예산
// MaxImportTotalSourceBytes와의 작은 값을 준다). 오류(failure 진단): 없음 MissingSource / NotFound,
// limit 초과 InvalidSource / CapacityExceeded, 읽기 IO 오류 InvalidSource / 그 오류, 할당 실패(진단
// None).
Result<void> ReadImportSource(platform::FileSystem& files, StringView path, Array<uint8>& bytes,
                              Sha256Digest& digest, uint64 limit = MaxImportSourceBytes,
                              AssetDiagnostic* failure = nullptr) noexcept;

// 임포트 상태의 원본 흘려 읽기 버퍼 크기다(7.1절 18 A2f2b — 원본을 통째로 올리지 않는다).
inline constexpr usize ImportStatusReadBytes = 64 * 1024;

// 임포트 원본 파일 path(절대 OS 경로)가 기록(byte 수 bytes·SHA-256 digest)과 같은지 본다(7.1절 18
// A2f2b). 크기가 다르면 읽지 않는다. 같으면 buffer 크기씩 흘려 읽으며 SHA-256을 계산한다(할당하지
// 않는다). 반환: None(같음), MissingSource(없음), SourceChanged(크기나 내용이 다름),
// InvalidSource(열기·크기·읽기 IO 오류 — error에 그 오류, 크기를 잰 뒤 줄어든 파일 포함). 빈
// buffer는 InvalidSource / InvalidArgument다.
//
// 사용 예:
//   Array<uint8> buffer(allocator);   // TryResize(ImportStatusReadBytes)
//   ErrorCode error = ErrorCode::None;
//   const AssetDiagnosticCode state =
//       CheckImportSource(files, path, source.bytes, source.sha256, buffer.View(), error);
AssetDiagnosticCode CheckImportSource(platform::FileSystem& files, StringView path, uint64 bytes,
                                      const Sha256Digest& digest, Span<uint8> buffer,
                                      ErrorCode& error) noexcept;

} // namespace rocks
