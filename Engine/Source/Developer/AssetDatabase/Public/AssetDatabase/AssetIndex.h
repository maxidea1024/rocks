#pragma once

#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/PackageReader.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/Containers/Array.h>
#include <Platform/File.h>

// 에디터 패키지 색인이다(Docs/Design/AssetSystem.ko.md 7절, 7.1절 11·13 — A2b, UE AssetRegistry
// 대응). 패키지 파일이 유일한 진실이고 색인은 `Content/` 아래 `.rasset`의 헤더·섹션 표·Summary·
// Exports·Dependencies·ImportRecord·Tags만 부분 읽기로 읽은 요약이다. 전체 검색(Rebuild — 캐시
// 재사용 A2d1)과 부분 갱신(Refresh·RefreshPaths — 저장·감시 A2d3)이 바꾼다.
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
class WorkerScheduler;

// 의존 종류 묶음이다(PackageDependencyKind 값 v의 bit 1 << v).
inline constexpr uint32 DependencyKindBit(PackageDependencyKind kind) noexcept {
    return uint32{1} << static_cast<uint32>(kind);
}

inline constexpr uint32 AllDependencyKinds = DependencyKindBit(PackageDependencyKind::Hard) |
                                             DependencyKindBit(PackageDependencyKind::Soft) |
                                             DependencyKindBit(PackageDependencyKind::EditorOnly) |
                                             DependencyKindBit(PackageDependencyKind::Build) |
                                             DependencyKindBit(PackageDependencyKind::ImportOwner);

// 색인 항목의 ImportRecord 요약이다(7.1절 15 — A2c2a). present는 ImportRecord를 읽었는지다(섹션이
// 없거나 다른 스키마·더 새 버전·형식 위반이면 거짓 — 뒤 둘은 진단이 남는다). 참조 기록은 owner만
// 의미가 있다(owner ≠ 항목 id). outputCount는 소유 기록의 출력 수다.
struct AssetIndexImportSummary {
    explicit AssetIndexImportSummary(Allocator& allocator) noexcept : importerName(allocator) {
    }

    bool present = false;
    AssetId owner{};
    Guid importerId{};
    String importerName;
    uint32 importerVersion = 0;
    usize outputCount = 0;
};

// 색인 항목 하나다(패키지 하나).
struct AssetIndexEntry {
    explicit AssetIndexEntry(Allocator& allocator) noexcept
        : path(allocator), key(allocator), fileName(allocator), dependencies(allocator),
          tags(allocator), importRecord(allocator) {
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
    AssetIndexImportSummary importRecord;
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

class AssetIndex;
class AssetIndexCache;

// 이름 바꾸기 하나다(RefreshPaths — 7.1절 17 A2e2). from(Content 상대 — 패키지 파일 또는
// 디렉터리)의 지금 내용을 to 이름으로 색인한다(OS 이동 전에 준비하고 이동 뒤에 확정한다).
struct AssetIndexMove {
    StringView from;
    StringView to;
};

// 여러 파일·하위 트리를 한 번에 다시 읽는 입력이다(AssetIndex::RefreshPaths — 7.1절 16 A2d3).
// 이름은 모두 Content 상대 이름 규칙(유효한 UTF-8, 구분자 '/', 앞뒤 '/'·빈 구성 요소·"."·".."
// 없음)을 따른다. 같은 이름이 여러 번 나와도 되고 하위 트리 안의 파일·하위 트리는 그 트리가 덮는다.
//
// 사용 예(감시가 모은 이름):
//   const StringView changed[] = {"Hero.rasset"};
//   const StringView moved[] = {"Characters"};
//   AssetIndexRefreshPaths paths;
//   paths.files = changed;
//   paths.subtrees = moved;
//   if (auto refreshed = index.RefreshPaths(files, paths, readOptions); !refreshed) { ... }
struct AssetIndexRefreshPaths {
    // 다시 읽을 패키지 파일(`.rasset`)이다. 없어졌으면 빼고 새 파일이면 더한다(Refresh와 같다).
    Span<const StringView> files;
    // 다시 검색할 하위 트리다(빈 문자열은 Content 전체). 그 이름 자신과 그 아래의 항목·진단을 지금
    // 파일 상태로 바꾼다(없어진 것은 뺀다). 이름이 디렉터리가 아니면 그 아래는 비어 있고, 이름이
    // 패키지 파일이면 그 파일을 읽는다.
    Span<const StringView> subtrees;
    // 비어 있지 않으면 files와 같은 길이다. 참인 파일은 쓰는 중으로 보이면(패키지를 읽지 못해 항목
    // 없이 CorruptPackage이거나 열기·읽기가 Busy) 그 파일의 항목·진단을 바꾸지 않고 deferred에 그
    // 위치(files 첨자 — 같은 이름이 여럿이면 모두)를 붙인다. 같은 이름 중 하나라도 거짓이면 미루지
    // 않는다. 하위 트리가 덮는 파일은 미루지 않는다.
    Span<const bool> mayDefer;
    Array<usize>* deferred = nullptr;
    // 이름 바꾸기다: from과 to 이름 자신과 그 아래의 항목·진단을 빼고, 지금 from에 있는 것(파일이면
    // 그 파일, 디렉터리면 그 아래 — 하위 트리 검색과 같은 규칙)을 to 아래 이름으로 읽어 더한다.
    // 이름 규칙은 같고, from과 to가 같은 키가 아니면 서로의 아래일 수 없다(InvalidArgument).
    // 대소문자만 다른 이름(같은 키)도 받는다.
    Span<const AssetIndexMove> moves;
    // 이름 정규화 파일 시스템인지다(참이면 이름 대조를 NFC로 한다 — macOS 이벤트 이름이 열거
    // 이름과 다른 정규형이어도 같은 파일로 본다).
    bool normalizesNames = FileSystemNormalizesNames;
};

// 색인 게시 준비다(AssetIndex::PrepareRefresh가 채우고 CommitRefresh가 쓴다 — 7.1절 15 A2c2b). 새
// 항목·진단, 정렬 순서, 같은 id·경로 진단과 결과 배열 자리를 모두 할당해 두고 지금 색인의 항목·
// 진단은 자리 포인터로 가리킨다(옮기지 않는다). 그래서 준비와 확정 사이에 그 색인을 바꾸면 안
// 된다(확정이 색인 변경 세대로 단언한다). 복사·이동하지 않는다(가리키는 자리를 지킨다).
class AssetIndexRefresh {
public:
    explicit AssetIndexRefresh(Allocator& allocator) noexcept
        : added_(allocator), fileDiagnostics_(allocator), neighbors_(allocator),
          entryOrder_(allocator), flags_(allocator), byId_(allocator), diagnosticOrder_(allocator),
          entries_(allocator), diagnostics_(allocator) {
    }

    AssetIndexRefresh(const AssetIndexRefresh&) = delete;
    AssetIndexRefresh& operator=(const AssetIndexRefresh&) = delete;

    // 준비를 마쳐 확정할 수 있는지다.
    bool IsReady() const noexcept {
        return ready_;
    }

private:
    friend class AssetIndex;

    // 준비 전 상태로 비운다(용량은 남긴다).
    void Reset() noexcept;

    // 다시 읽은 파일의 항목(0·1개)과 그 파일 진단, 같은 id·경로 진단이다.
    Array<AssetIndexEntry> added_;
    Array<AssetIndexDiagnostic> fileDiagnostics_;
    Array<AssetIndexDiagnostic> neighbors_;
    // 확정 순서의 항목·진단 자리와 항목별 같은 경로(1)·같은 id(2) 표시다.
    Array<AssetIndexEntry*> entryOrder_;
    Array<uint8> flags_;
    Array<usize> byId_;
    Array<AssetIndexDiagnostic*> diagnosticOrder_;
    // 자리를 잡아 둔 결과 배열이다(확정은 할당 없이 옮긴다).
    Array<AssetIndexEntry> entries_;
    Array<AssetIndexDiagnostic> diagnostics_;
    uint64 revision_ = 0;
    bool ready_ = false;
};

class AssetIndex {
public:
    // scheduler가 있으면 검색(Rebuild·RefreshPaths의 하위 트리)의 파일 읽기를 그 worker와 나눠
    // 한다(7.1절 16 A2d2 — 결과는 직렬과 같다). 그때 allocator·검색에 넘기는 FileSystem·layout의
    // allocator는 여러 스레드의 동시 호출을 받아야 한다(SystemAllocator·PhysicalFileSystem은
    // 받는다). scheduler는 색인보다 오래 살고, 색인을 그 scheduler의 worker 안에서 갱신하면 읽기는
    // 호출 스레드 혼자 한다(ParallelFor 규칙).
    AssetIndex(Allocator& allocator, const ProjectLayout& layout,
               WorkerScheduler* scheduler = nullptr) noexcept;

    AssetIndex(const AssetIndex&) = delete;
    AssetIndex& operator=(const AssetIndex&) = delete;

    // Content 아래를 처음부터 다시 검색한다(링크는 따라가지 않는다). `.rasset`(대소문자 무시)만
    // 읽고 `*.tmp`와 그 밖의 파일(장치 등 일반 파일이 아닌 것 포함)은 무시한다. 파일 하나의
    // 실패는 진단으로 남기고 그 파일을 빼고 계속한다: 이름 규칙 위반 InvalidPath, 패키지 판정은 그
    // 코드, 열거 뒤 사라진 파일 MissingFile, 그 밖의 열기·읽기 IO 실패는 코드 None과 그 오류다.
    // 없는 디렉터리(Content가 없거나 검색 중 지워진 하위 디렉터리)는 비어 있는 것으로 본다.
    // 오류(색인은 비운다): 디렉터리 열거 실패(Content가 파일이면 WrongType 대응 오류)는 그 파일
    // 오류, 할당 실패.
    //
    // 색인 읽기는 TypeRegistry를 쓰지 않는다(options.types를 비운다 — 색인은 형식 판정만 하고
    // 타입 축은 열기가 판정한다, 7.1절 16). cache가 있으면 파일마다 범위 읽기 지문
    // (ReadPackageFingerprint)이 캐시의 같은 이름·같은 지문 항목과 같을 때 그 항목을 쓴다(크기·
    // 시각은 지금 값). 지문 읽기가 실패하면 부분 읽기가 진단을 만든다. 캐시 재사용의 민감도는 섹션
    // CRC32 수준이다 — 점검(Verify 명령)은 캐시 없이 부른다.
    Result<void> Rebuild(platform::FileSystem& files, const PackageReadOptions& options,
                         const AssetIndexCache* cache = nullptr) noexcept;

    // Content 상대 이름 fileName(구분자 '/', `.rasset` 확장자 — 색인 항목의 fileName과 같은 철자)
    // 한 파일만 다시 읽는다(저장 뒤 — 7.1절 15 A2c2b). 그 파일의 항목·진단을 바꾸고(없어졌으면
    // 빼고, 새 파일이면 더한다) 같은 id·경로 진단과 표시는 전체에서 다시 정한다. 파일 IO·판정
    // 실패는 Rebuild처럼 진단이다(링크·일반 파일이 아닌 것은 색인하지 않는다). 오류: fileName이
    // `.rasset`이 아니거나 비었으면 InvalidArgument, 할당 실패 — 둘 다 색인
    // 그대로다(PrepareRefresh + CommitRefresh).
    Result<void> Refresh(platform::FileSystem& files, StringView fileName,
                         const PackageReadOptions& options) noexcept;

    // Refresh를 두 단계로 한다: PrepareRefresh는 fileName 한 파일을 다시 읽어(readPath가 비지
    // 않으면 그 경로의 내용을 fileName 이름으로 — 저장이 교체 전에 임시 파일을 대상 이름으로
    // 색인한다) 새 색인을 모두 준비하고 지금 색인을 바꾸지 않는다. 오류는 Refresh와 같고(할당
    // 실패도 색인 그대로), 실패한 plan은 준비되지 않는다. CommitRefresh는 할당 없이
    // 확정한다(실패하지 않는다). 준비와 확정 사이에 이 색인을 바꾸면(Rebuild·다른 확정) 안 되고,
    // 준비되지 않은 plan의 확정은 계약 위반이다(단언).
    Result<void> PrepareRefresh(platform::FileSystem& files, StringView fileName,
                                StringView readPath, const PackageReadOptions& options,
                                AssetIndexRefresh& plan) noexcept;
    void CommitRefresh(AssetIndexRefresh& plan) noexcept;

    // paths의 파일·하위 트리를 다시 읽어 그 이름들의 항목·진단을 지금 상태로 바꾸고 같은 id·경로
    // 진단과 표시는 전체에서 다시 정한다(7.1절 16 A2d3 — 감시 갱신이 쓴다). 다시 읽기는 Rebuild와
    // 같은 규칙이고(캐시는 쓰지 않는다) 하위 트리의 열거 실패는 진단이다 — 단 빈 하위 트리(Content
    // 전체)의 열거 실패는 Rebuild처럼 오류다. 오류: 이름 규칙 위반·mayDefer 길이 불일치·mayDefer가
    // 있는데 deferred가 없으면 InvalidArgument, Content 열거 실패, 할당 실패 — 모두 색인
    // 그대로다(deferred는 붙인 것이 남을 수 있다). 두 단계(PrepareRefreshPaths + CommitRefresh)로도
    // 할 수 있다(규칙은 PrepareRefresh와 같다).
    Result<void> RefreshPaths(platform::FileSystem& files, const AssetIndexRefreshPaths& paths,
                              const PackageReadOptions& options) noexcept;
    Result<void> PrepareRefreshPaths(platform::FileSystem& files,
                                     const AssetIndexRefreshPaths& paths,
                                     const PackageReadOptions& options,
                                     AssetIndexRefresh& plan) noexcept;

    // 감시 루트(Content)가 사라졌다: 항목을 모두 빼고 진단을 RootLost(이름은 빈 문자열 — Content
    // 자신, 원본 오류 NotFound) 하나로 둔다(7.1절 16 A2d3 — 파일이 진실이다, 열린 패키지는
    // 그대로). 오류: 할당 실패(색인 그대로).
    Result<void> MarkRootLost() noexcept;

    // 색인이 쓰는 배치다.
    const ProjectLayout& Layout() const noexcept {
        return *layout_;
    }

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
    Result<void> IndexFile(platform::FileSystem& files, const PackageReadOptions& editor,
                           String&& relative, StringView readPath, int64 modifiedTicks,
                           Array<AssetIndexEntry>& found,
                           Array<AssetIndexDiagnostic>& diagnostics) const noexcept;
    // cache의 같은 지문 항목으로 relative를 색인한다(found에 더하면 true, 캐시를 쓸 수 없으면
    // false — 호출자가 IndexFile로 읽는다).
    Result<bool> IndexCached(platform::FileSystem& files, const AssetIndexCache& cache,
                             const String& relative, int64 modifiedTicks,
                             Array<AssetIndexEntry>& found) const noexcept;
    // 걷기가 모은 패키지 후보다(Content 상대 이름과 열거한 수정 시각).
    struct ScanCandidate {
        explicit ScanCandidate(Allocator& allocator) noexcept
            : relative(allocator), readPath(allocator) {
        }

        String relative;
        // 이름 바꾸기면 내용을 읽을 경로(Content의 지금 이름)다 — 비면 Content/relative.
        String readPath;
        int64 modifiedTicks = 0;
    };

    // 후보 하나의 읽기 결과 칸이다(항목 0·1개와 그 파일 진단).
    struct ScanSlot {
        explicit ScanSlot(Allocator& allocator) noexcept
            : found(allocator), diagnostics(allocator) {
        }

        Array<AssetIndexEntry> found;
        Array<AssetIndexDiagnostic> diagnostics;
    };

    // start(Content 상대 — 빈 문자열은 Content)부터 깊이 우선으로 걸어 found·diagnostics에 더한다
    // (Rebuild·RefreshPaths 공용). Content 자신의 열거 실패만 오류다.
    // renameTo가 비지 않으면 start 아래 이름을 renameTo 아래 이름으로 색인한다(내용은 지금 이름에서
    // 읽는다 — 이동 전 준비, 캐시는 쓰지 않는다).
    Result<void> ScanTree(platform::FileSystem& files, const PackageReadOptions& editor,
                          const AssetIndexCache* cache, StringView start, StringView renameTo,
                          Array<AssetIndexEntry>& found,
                          Array<AssetIndexDiagnostic>& diagnostics) const noexcept;
    // 후보를 읽어(캐시 재사용 또는 부분 읽기 — scheduler가 있으면 병렬) 후보 순서대로 더한다.
    Result<void> ReadCandidates(platform::FileSystem& files, const PackageReadOptions& editor,
                                const AssetIndexCache* cache, Array<ScanCandidate>& candidates,
                                Array<AssetIndexEntry>& found,
                                Array<AssetIndexDiagnostic>& diagnostics) const noexcept;
    // fileName 한 파일을 Stat해 다시 읽는다(Refresh·RefreshPaths 공용 — 없어졌으면 아무것도 더하지
    // 않는다).
    Result<void> RereadFile(platform::FileSystem& files, const PackageReadOptions& editor,
                            StringView fileName, StringView readPath, Array<AssetIndexEntry>& found,
                            Array<AssetIndexDiagnostic>& diagnostics) const noexcept;
    // 항목·진단 자리로 게시를 준비한다(정렬·같은 id·경로 진단·결과 자리 — 할당은 모두 여기서).
    Result<void> PlanPublish(Span<AssetIndexEntry* const> entries,
                             Span<AssetIndexDiagnostic* const> diagnostics,
                             AssetIndexRefresh& plan) const noexcept;
    // 준비한 게시를 할당 없이 확정한다.
    void CommitPublish(AssetIndexRefresh& plan) noexcept;

    Allocator* allocator_;
    const ProjectLayout* layout_;
    WorkerScheduler* scheduler_;
    Array<AssetIndexEntry> entries_;
    // 항목 index를 id 순으로 둔 표다(FindById 이진 검색).
    Array<usize> byId_;
    Array<AssetIndexDiagnostic> diagnostics_;
    // 게시마다 오르는 세대다(AssetIndexRefresh 대조).
    uint64 revision_ = 0;
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
