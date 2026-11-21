#pragma once

#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/PackageReader.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/Containers/Array.h>
#include <Platform/File.h>

// 에디터 패키지 색인이다(Docs/Design/AssetSystem.ko.md 7절, 7.1절 11·13 — A2b, UE AssetRegistry
// 대응). 패키지 파일이 유일한 진실이고 색인은 `Content/` 아래 `.rasset`의 헤더·섹션 표·Summary·
// Exports·Dependencies·ImportRecord·Tags만 부분 읽기로 읽은 요약이다. A2b는 전체 검색만 한다(캐시·
// 감시 갱신은 A2d).
//
// 사용 예:
//   AssetIndex index(allocator, layout);
//   if (auto built = index.Rebuild(platform::PhysicalFileSystem(), readOptions); !built) { ... }
//   for (const AssetIndexDiagnostic& d : index.Diagnostics()) { /* 진단 줄 */ }
//   auto hero = index.FindByPath(ParseLogicalPath(allocator, "/Game/Hero").Value());
//
// 스레드: 한 스레드에서 쓴다(색인 갱신과 조회를 동시에 하지 않는다).
namespace rocks {

class StringBuilderBase;
class TypeRegistry;

// 의존 종류 묶음이다(PackageDependencyKind 값 v의 bit 1 << v).
inline constexpr uint32 DependencyKindBit(PackageDependencyKind kind) noexcept {
    return uint32{1} << static_cast<uint32>(kind);
}

inline constexpr uint32 AllDependencyKinds = DependencyKindBit(PackageDependencyKind::Hard) |
                                             DependencyKindBit(PackageDependencyKind::Soft) |
                                             DependencyKindBit(PackageDependencyKind::EditorOnly) |
                                             DependencyKindBit(PackageDependencyKind::Build) |
                                             DependencyKindBit(PackageDependencyKind::ImportOwner);

// 색인 항목 하나다(패키지 하나).
struct AssetIndexEntry {
    explicit AssetIndexEntry(Allocator& allocator) noexcept
        : path(allocator), key(allocator), fileName(allocator), dependencies(allocator),
          tags(allocator) {
    }

    AssetIndexEntry(AssetIndexEntry&&) noexcept = default;
    AssetIndexEntry& operator=(AssetIndexEntry&&) noexcept = default;

    AssetId id{};
    // 논리 경로("/Game/Dir/Name")와 비교 키("game/dir/name").
    String path;
    String key;
    // Content 아래 실제 OS 상대 이름(열거한 byte 그대로, 구분자 '/', 확장자 포함 — 7.1절 10).
    String fileName;
    // 대표 에셋의 타입(export 1)이다.
    TypeId type{};
    PackageFingerprint fingerprint{};
    uint64 fileBytes = 0;
    int64 modifiedTicks = 0;
    Array<PackageDependencyEntry> dependencies;
    Array<PackageTag> tags;
    bool readOnly = false;
    // 같은 AssetId·같은 경로 키가 다른 파일에도 있다(조회에서 AlreadyExists — 7.1절 13).
    bool duplicateId = false;
    bool duplicatePath = false;
};

// 색인 진단 하나다(4.6절 코드). fileName은 Content 상대 이름(경로 진단)이다.
struct AssetIndexDiagnostic {
    explicit AssetIndexDiagnostic(Allocator& allocator) noexcept
        : fileName(allocator), related(allocator) {
    }

    AssetIndexDiagnostic(AssetIndexDiagnostic&&) noexcept = default;
    AssetIndexDiagnostic& operator=(AssetIndexDiagnostic&&) noexcept = default;

    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    // 원본 오류(패키지 판정·IO의 ErrorCode)다.
    ErrorCode error = ErrorCode::None;
    // Content 상대 OS 이름의 표시 이름이다(related도 같다): 한 줄·유효한 UTF-8이고 서로 다른 OS
    // 이름이 겹치지 않게 역슬래시·제어 문자(0x00–0x1f·0x7f)와, 이름이 유효한 UTF-8이 아니면(POSIX)
    // 0x80 이상 byte를 역슬래시 x 16진 두 자리로 적는다. Windows 이름은 대개 그대로다.
    String fileName;
    AssetId asset{};
    // InvalidPath의 사유다.
    LogicalPathError pathError = LogicalPathError::None;
    // 패키지 판정 진단의 칸·위치(AssetDiagnostic::section·detail)다.
    uint32 section = 0;
    uint64 detail = 0;
    // 겹친 상대(DuplicateAssetId·DuplicatePath의 다른 파일, MissingDependency·TypeMismatch의 의존
    // 대상 문자열 없음 — asset·related로 본다)다.
    String related;
    AssetId relatedAsset{};
};

class AssetIndex {
public:
    AssetIndex(Allocator& allocator, const ProjectLayout& layout) noexcept;

    AssetIndex(const AssetIndex&) = delete;
    AssetIndex& operator=(const AssetIndex&) = delete;

    // Content 아래를 처음부터 다시 검색한다(링크는 따라가지 않는다). `.rasset`(대소문자 무시)만
    // 읽고 `*.tmp`와 그 밖의 파일(장치 등 일반 파일이 아닌 것 포함)은 무시한다. 파일 하나의
    // 실패는 진단으로 남기고 그 파일을 빼고 계속한다: 이름 규칙 위반 InvalidPath, 패키지 판정은 그
    // 코드, 열거 뒤 사라진 파일 MissingFile, 그 밖의 열기·읽기 IO 실패는 코드 None과 그 오류다.
    // 없는 디렉터리(Content가 없거나 검색 중 지워진 하위 디렉터리)는 비어 있는 것으로 본다.
    // 오류(색인은 비운다): 디렉터리 열거 실패(Content가 파일이면 WrongType 대응 오류)는 그 파일
    // 오류, 할당 실패.
    Result<void> Rebuild(platform::FileSystem& files, const PackageReadOptions& options) noexcept;

    // 항목 수와 index번째 항목(경로 키 → 파일 이름 순)이다.
    usize Count() const noexcept {
        return entries_.Size();
    }

    const AssetIndexEntry& At(usize index) const noexcept {
        return entries_[index];
    }

    // 마지막 Rebuild의 진단(파일 이름 → 코드 순)이다.
    Span<const AssetIndexDiagnostic> Diagnostics() const noexcept {
        return diagnostics_.View();
    }

    // 오류: 없으면 NotFound, 같은 id·경로가 둘 이상이면 AlreadyExists(어느 쪽인지 정할 수 없다).
    Result<const AssetIndexEntry*> FindById(AssetId id) const noexcept;
    Result<const AssetIndexEntry*> FindByPath(const LogicalPath& path) const noexcept;

    // directory(마운트 "/Game" 또는 논리 경로) 아래 항목을 out에 붙인다(경로 키 순). recursive가
    // 거짓이면 바로 아래만이다. 오류: directory가 마운트도 논리 경로도 아니면 InvalidArgument, 할당
    // 실패.
    Result<void> ListUnder(StringView directory, bool recursive,
                           Array<const AssetIndexEntry*>& out) const noexcept;

    // 대표 타입이 type인 항목을 붙인다. types가 있으면 등록된 타입의 파생도 포함한다(등록되지 않은
    // 타입은 정확 일치만 — 7.1절 11). 오류: type이 0이면 InvalidArgument, 할당 실패, TypeRegistry
    // 판정의 그 밖의 오류(종료 뒤 ShuttingDown 등).
    Result<void> ListByType(TypeId type, TypeRegistry* types,
                            Array<const AssetIndexEntry*>& out) const noexcept;

    // 태그 (key, value)를 가진 항목을 붙인다(목록 값은 원소마다 — 같은 키 항목 하나라도 맞으면).
    Result<void> FindByTag(StringView key, StringView value,
                           Array<const AssetIndexEntry*>& out) const noexcept;

    // id 패키지가 의존하는 항목(종류 묶음 kinds)을 붙인다(저장 순서). 오류: FindById와 같다, 할당
    // 실패.
    Result<void> Dependencies(AssetId id, uint32 kinds,
                              Array<const PackageDependencyEntry*>& out) const noexcept;

    // id를 kinds 종류로 참조하는 항목을 붙인다(경로 키 순). id가 색인에 없어도 된다(누락 대상의
    // 참조자 찾기). 오류: 할당 실패.
    Result<void> Referencers(AssetId id, uint32 kinds,
                             Array<const AssetIndexEntry*>& out) const noexcept;

    // 의존 대조 진단을 out에 붙인다(7.1절 13·14): 색인에 없는 Hard·Soft 의존 MissingDependency.
    // types가 있으면 Hard 의존의 대상 대표 타입이 expectedType도 그 파생도 아닐 때 TypeMismatch다
    // (로드와 같은 판정 — 미등록 타입·클래스 아닌 id도 TypeMismatch). types가 없으면 파생 여부를 알
    // 수 없어 형식은 대조하지 않는다. 오류: 할당 실패, TypeRegistry 판정의 그 밖의 오류
    // (ShuttingDown 등).
    Result<void> Verify(TypeRegistry* types, Array<AssetIndexDiagnostic>& out) const noexcept;

private:
    Allocator* allocator_;
    const ProjectLayout* layout_;
    Array<AssetIndexEntry> entries_;
    // 항목 index를 id 순으로 둔 표다(FindById 이진 검색).
    Array<usize> byId_;
    Array<AssetIndexDiagnostic> diagnostics_;
};

// 진단 한 줄 `<진단 코드>: Content/<파일>: <설명>\n`을 line에 붙인다(13절 명령 출력 —
// asset-index-verify, 영어). 코드 None(파일 IO 실패)은 코드 자리에 원본 오류 이름(ErrorCodeName)을
// 쓴다. 설명: InvalidPath는 LogicalPathError 이름, 같은 id·경로는 상대 파일, 의존 진단은 대상
// AssetId, 패키지 판정은 칸·위치·원본 오류다. 이름은 진단의 표시 이름 그대로 쓴다. 오류: line의
// 할당 실패(그때 line에 붙인 일부가 남을 수 있다).
//   StringBuilder<512> line(allocator);
//   if (auto appended = AppendAssetIndexDiagnosticLine(line, diagnostic); !appended) { ... }
Result<void> AppendAssetIndexDiagnosticLine(StringBuilderBase& line,
                                            const AssetIndexDiagnostic& diagnostic) noexcept;

} // namespace rocks
