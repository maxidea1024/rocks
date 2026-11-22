#pragma once

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/AssetAliasTable.h>
#include <AssetDatabase/AssetClock.h>
#include <AssetDatabase/AssetImporter.h>
#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/AssetTextResolver.h>
#include <AssetDatabase/EditorPackageSet.h>
#include <AssetDatabase/ProjectLayout.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextDiagnostics.h>

// 에셋 서비스 API다(Docs/Design/AssetSystem.ko.md 13절·7.1절 15 A2c3). 명령행 asset-new·asset-set·
// asset-info·asset-list와 1:1 함수이고 에디터가 나중에 같은 함수를 쓴다. 프로젝트 하나의 색인과
// 에디터 패키지 집합을 소유한다(명령 하나가 서비스 하나를 만들고 끝나면 버린다).
//
// 사용 예(asset-set):
//   AssetServiceConfig config;
//   config.packages.types = &types;
//   config.packages.files = &platform::PhysicalFileSystem();
//   config.packages.hooks = &hooks;
//   config.packages.registryOptions.names = &names;
//   AssetService service(allocator, layout, config);
//   if (auto ready = service.Initialize(); !ready) { ... }
//   AssetDatabaseLock lock;
//   if (auto held = lock.Acquire(layout, waitMilliseconds); !held) { ... }
//   reflection_text::TextDiagnostics diagnostics(allocator);
//   Array<EditorWarning> warnings(allocator);
//   AssetDiagnostic failure;
//   auto set = service.Set(lock, "/Game/Hero", assignments, false, diagnostics, &warnings,
//                          &failure);
namespace rocks {

struct AssetServiceConfig {
    // 에디터 패키지 집합 설정이다. layout·index는 서비스가 채운다(넘긴 값은 쓰지 않는다).
    // registryOptions.names는 Name 필드 대입·출력의 pool이다(없으면 Name 값 대입은 진단
    // Unsupported). read는 색인 Rebuild에도 쓴다.
    EditorPackageSetConfig packages;
    // 색인 캐시(Intermediate/AssetIndex.cache — 7.1절 16 A2d1)의 파일 시스템이다(null이면
    // packages.files). useCache가 거짓이면 캐시를 읽지 않는다(점검·재구축 명령).
    platform::FileSystem* cacheFiles = nullptr;
    bool useCache = true;
    // 색인 검색의 파일 읽기를 나눌 scheduler다(7.1절 16 A2d2 — null이면 직렬). 있으면
    // packages.files와 서비스 allocator가 동시 호출을 받아야 한다(AssetIndex 생성자 계약).
    WorkerScheduler* scheduler = nullptr;
    // 별칭의 만든 시각·prune 판정 시계(null이면 시스템 시계 — 시험은 수동 시계), 새 별칭의 만료
    // 일수, prune의 텍스트 검색 상한이다(7.1절 17 A2e1).
    AssetClock* clock = nullptr;
    uint32 aliasDays = AssetAliasDefaultDays;
    AssetAliasSearchLimits aliasSearch{};
    // 임포터 표다(7.1절 18 A2f — null이면 임포트는 UnsupportedSourceFormat / Unsupported). 표는
    // 서비스보다 오래 살아야 한다.
    AssetImporterTable* importers = nullptr;
};

// 임포트 원본 입력이다: 역할(원본 하나면 DefaultImportSourceRole)과 절대 OS 경로.
struct AssetImportSourceInput {
    StringView role{};
    StringView path{};
};

// 임포트 출력 하나다(주 출력 말고 — 임포터가 Output을 부른 순서).
struct AssetImportOutput {
    explicit AssetImportOutput(Allocator& allocator) noexcept : key(allocator), path(allocator) {
    }

    AssetImportOutput(AssetImportOutput&&) noexcept = default;
    AssetImportOutput& operator=(AssetImportOutput&&) noexcept = default;

    String key;
    String path;
    AssetId id{};
};

// asset-import 결과다(7.1절 18 A2f1b): 주 출력 id·경로와 다른 출력들.
struct AssetImportResult {
    explicit AssetImportResult(Allocator& allocator) noexcept
        : path(allocator), outputs(allocator) {
    }

    AssetId id{};
    String path;
    Array<AssetImportOutput> outputs;
};

// asset-info 출력 선택이다(13절).
struct AssetInfoOptions {
    // JSON 출력(ExportJson)이다(기본은 `<경로>=<값>` 줄 — ExportLines).
    bool json = false;
    // 보고용 전체 출력(Report 용도 — Transient만 뺀다)이다(기본은 Edit 용도 — asset-set에 그대로
    // 넣을 수 있다).
    bool all = false;
};

// asset-move 결과다(7.1절 17 A2e2).
struct AssetMoveResult {
    // 옮긴 패키지 수(파일이 있는 것과 저장하지 않은 새 패키지)와 더한 별칭 수다.
    usize packages = 0;
    usize aliases = 0;
};

// asset-delete 결과다(7.1절 17 A2e3).
struct AssetDeleteResult {
    explicit AssetDeleteResult(Allocator& allocator) noexcept
        : path(allocator), referencerPaths(allocator), referencerIds(allocator) {
    }

    // 지운(거부면 고른) 패키지 id와 논리 경로, 지운 별칭 수다.
    AssetId id{};
    String path;
    usize aliases = 0;
    // 디스크 참조자(색인 — 경로 키 순, 자신 제외)의 경로·id 사본이다(거부·force 모두 — 색인이
    // 바뀌어도 유효).
    Array<String> referencerPaths;
    Array<AssetId> referencerIds;
};

// asset-fix-duplicate-id 결과다(7.1절 17 A2e4b): 고친 경로·옛 id·새 id, 옛 id를 쓰는 남은 항목의
// 경로들(다른 패키지의 옛 id 참조는 그쪽을 가리킨다 — 실패해도 고른 대상은 채운다).
struct AssetFixIdResult {
    explicit AssetFixIdResult(Allocator& allocator) noexcept
        : path(allocator), remaining(allocator) {
    }

    String path;
    AssetId oldId{};
    AssetId newId{};
    Array<String> remaining;
};

// asset-duplicate 결과다(7.1절 17 A2e3b): 사본 id와 정규화한 논리 경로.
struct AssetDuplicateResult {
    explicit AssetDuplicateResult(Allocator& allocator) noexcept : path(allocator) {
    }

    AssetId id{};
    String path;
};

class AssetService {
public:
    // layout은 초기화한 프로젝트 배치다(빌림 — 서비스보다 오래 산다).
    AssetService(Allocator& allocator, const ProjectLayout& layout,
                 const AssetServiceConfig& config) noexcept;
    AssetService(const AssetService&) = delete;
    AssetService& operator=(const AssetService&) = delete;

    // Content를 훑어 색인을 만들고(useCache면 캐시를 읽어 지문이 같은 파일은 다시 읽지 않는다 —
    // 없거나 맞지 않는 캐시는 없는 것과 같다) 에디터 패키지 집합을 연다. 다시 부르면
    // InvalidArgument다.
    // 오류: 설정 오류(타입·파일 시스템 없음 등) InvalidArgument, 색인 Rebuild의 오류(디렉터리
    // 열거 실패), 집합 Initialize의 오류, 할당 실패.
    Result<void> Initialize() noexcept;

    // 색인과 집합이다(Initialize 뒤).
    AssetIndex& Index() noexcept {
        return index_;
    }

    EditorPackageSet& Packages() noexcept {
        return set_;
    }

    // asset-new <Type> <path> [<속성>=<값>…]: path에 type(타입 도구 이름 — 에셋 클래스)의 새
    // 패키지를 만들고, 대입을 공개 전 대표 에셋에 모두 검증·게시한 뒤 함께 공개해 저장한다. 반환은
    // 새 AssetId다. lock은 이 프로젝트의 잡은 쓰기 잠금이다. 새 패키지의 객체는 공개 전이라 Weak
    // 참조와 새 패키지 자신을 가리키는 참조는 풀 수 없다(진단 Unresolved — 만든 뒤 Set으로 넣는다).
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, path 규칙 위반 InvalidPath /
    // InvalidArgument, 모르는 타입 이름 NotFound(진단 None), 에셋 클래스가 아니거나 추상 클래스
    // TypeMismatch / InvalidArgument, EditorPackageSet::Create의 오류(같은 경로 DuplicatePath 등),
    // 대입 오류 InvalidArgument(진단 None — diagnostics에 모두 모인다, 아무것도 만들지 않는다),
    // EditorPackageSet::Save의 오류(만든 패키지는 저장하지 않은 채 집합에 남는다), 할당 실패.
    Result<AssetId> New(const AssetDatabaseLock& lock, StringView type, StringView path,
                        Span<const reflection_text::TextAssignment> assignments,
                        reflection_text::TextDiagnostics& diagnostics,
                        Array<EditorWarning>* warnings = nullptr,
                        AssetDiagnostic* failure = nullptr) noexcept;

    // asset-set <path|AssetId> <속성>=<값>…: 대상을 열고(Hard·EditorOnly 닫힘째) 대입을 모두
    // 검증한 뒤 한꺼번에 게시하고 저장한다. Weak 참조는 대상 패키지 안의 객체만 받는다(5.1절 —
    // 밖은 진단 Unresolved). force는 미해결 import를 null로 저장한다(--force —
    // 경고 DroppedUnresolvedImport). 빈 대입 목록도 저장한다.
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, 대상 텍스트 형식(논리 경로 규칙·36자
    // 소문자 AssetId) InvalidPath / InvalidArgument, 열기의 오류(EditorPackageSet::Open), 대입 오류
    // InvalidArgument(진단 None — diagnostics에 모두 모인다, 아무것도 바꾸지 않는다), 게시의 오류,
    // EditorPackageSet::Save의 오류(바뀐 값은 집합에 남는다), 할당 실패.
    Result<void> Set(const AssetDatabaseLock& lock, StringView target,
                     Span<const reflection_text::TextAssignment> assignments, bool force,
                     reflection_text::TextDiagnostics& diagnostics,
                     Array<EditorWarning>* warnings = nullptr,
                     AssetDiagnostic* failure = nullptr) noexcept;

    // asset-info <path|AssetId>: 대상을 열어 대표 에셋의 값을 out 뒤에 쓴다(options — 줄·JSON,
    // Edit·Report). 참조는 논리 경로로 쓴다. 잠금을 잡지 않는다.
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, 대상 텍스트 형식 InvalidPath /
    // InvalidArgument, 열기의 오류, 출력의 오류(진단 None — 줄로 쓸 수 없는 값은 Unsupported,
    // 그때는 json을 쓴다), 할당 실패.
    Result<void> Info(StringView target, const AssetInfoOptions& options, String& out,
                      Array<EditorWarning>* warnings = nullptr,
                      AssetDiagnostic* failure = nullptr) noexcept;

    // asset-list [--type=<Type>] [--under=<path>]: 색인 항목을 경로 키 순으로 out에 붙인다. type이
    // 있으면 그 클래스와 파생의 대표 에셋만(등록되지 않은 타입의 항목은 맞지 않는다), under가
    // 있으면 그 논리 디렉터리 아래만(없으면 /Game 전체)이다. 항목 포인터는 색인이 바뀌기 전까지
    // 유효하다.
    //
    // 오류(failure 진단): Initialize 전 InvalidArgument, 모르는 타입 이름 NotFound(진단 None),
    // 클래스가 아닌 타입 TypeMismatch / InvalidArgument, under가 논리 디렉터리가 아님 InvalidPath /
    // InvalidArgument, 파생 판정의 그 밖 오류(TypeRegistry 종료 뒤 ShuttingDown 등 — 진단 None),
    // 할당 실패. 실패하면 out은 호출 전과 같다.
    Result<void> List(StringView type, StringView under, Array<const AssetIndexEntry*>& out,
                      AssetDiagnostic* failure = nullptr) noexcept;

    // 지금 색인을 캐시로 쓴다(7.1절 16 — 쓰기 명령이 끝날 때 부른다, 저장마다 쓰지 않는다). lock은
    // 이 프로젝트의 잡은 쓰기 잠금이다. 실패는 캐시만의 일이다(호출자는 경고 CacheNotWritten으로
    // 보고 한다). 오류: Initialize 전 InvalidArgument, WriteAssetIndexCache의 오류.
    Result<void> FlushCache(const AssetDatabaseLock& lock) noexcept;

    // asset-move <path|디렉터리> <새 path>: 패키지 하나·저장하지 않은 새 패키지·논리 디렉터리를 새
    // 경로로 옮긴다(7.1절 17 A2e2 — ID·참조는 그대로, 옮긴 패키지마다 옛 경로 별칭,
    // 대소문자·정규화만 바꾸는 이름 변경은 별칭 없음). 순서: 검사 → 집합 경로 표·색인 갱신 준비 →
    // 열린 패키지 Detach → 별칭 쓰기 → 대상 부모 디렉터리 → OS 이동 한 번(디렉터리도 — 디스크에
    // 없으면 건너뜀) → 확정. OS 이동이 실패하면 별칭 표를 되돌린다(되돌리지 못하면 경고
    // AliasNotRestored). 별칭 쓰기의 NotDurable은 경고 NotDurable이다.
    //
    // 오류(failure 진단): Initialize 전·잠금 아님 InvalidArgument, 경로 규칙 InvalidPath /
    // InvalidArgument, 원본이 없음 NotFound(진단 None), 원본 경로 키 둘 DuplicatePath /
    // AlreadyExists, 대상이 원본 아래 InvalidArgument, 대상이 차 있음(색인·파일·디렉터리·새 패키지·
    // 다른 에셋의 별칭) DuplicatePath / AlreadyExists, 새 경로 규칙 위반(길이) InvalidPath, 별칭
    // 표의 오류(Aliases·Write), Detach·디렉터리 만들기·OS 이동의 IO(진단 None과 그 오류), 할당 실패
    // — 모두 패키지 파일·색인·집합 그대로(Detach는 되돌리지 않는다 — 메모리 bulk는 동작이 같다).
    Result<void> Move(const AssetDatabaseLock& lock, StringView source, StringView target,
                      AssetMoveResult& result, Array<EditorWarning>* warnings = nullptr,
                      AssetDiagnostic* failure = nullptr) noexcept;

    // asset-delete <path|AssetId> [--force]: 패키지 하나를 지운다(7.1절 17 A2e3). 대상은 지금
    // 이름이다(별칭으로 풀지 않는다). 색인 항목이면 파일을 지우고, 저장하지 않은 새 패키지면
    // 집합에서 닫기만 한다. 순서: 검사 → 색인 갱신 준비(removed) → 대상을 가리키는 별칭 지우기
    // 쓰기 → 집합에서 닫기(Discard — 저장하지 않은 편집을 버린다) → 파일 지우기 → 색인 확정.
    // 닫기·지우기가 실패하면 별칭 표를 되돌린다(되돌리지 못하면 경고 AliasNotRestored — 지우기
    // 실패면 패키지는 이미 닫혔고 파일·색인은 그대로다). 지운 별칭마다 경고 AliasRemoved, 별칭
    // 쓰기의 NotDurable은 경고 NotDurable이다. result는 실패해도 고른 대상과 디스크 참조자를
    // 담는다.
    //
    // 오류(failure 진단): Initialize 전·잠금 아님 InvalidArgument, 대상 텍스트 형식 InvalidPath /
    // InvalidArgument, 없음 NotFound(진단 None), 같은 경로 키 둘 DuplicatePath / AlreadyExists,
    // 같은 id 파일 둘 DuplicateAssetId / AlreadyExists, 디스크 참조자가 있고 force가 아님
    // ReferencedBy / InvalidArgument(related = 첫 참조자), 다른 열린 패키지가 메모리에서 참조함
    // ReferencedBy / Busy(force여도), 별칭 표의 오류(Aliases·Write), 색인 준비·닫기·파일 지우기의
    // 오류(진단 None과 그 오류), 할당 실패.
    Result<void> Delete(const AssetDatabaseLock& lock, StringView target, bool force,
                        AssetDeleteResult& result, Array<EditorWarning>* warnings = nullptr,
                        AssetDiagnostic* failure = nullptr) noexcept;

    // asset-orphans-delete <path>: 고아 출력(7.1절 17 A2e4 — AssetIndex::OrphanReason)만 지운다.
    // 경로는 지금 이름이다(별칭으로 풀지 않는다). 고아면 Delete(lock, path, force = false)와 같다
    // (참조자가 있으면 거부 — 강제는 asset-delete --force).
    //
    // 오류(failure 진단): Initialize 전·잠금 아님 InvalidArgument, 경로 규칙 InvalidPath /
    // InvalidArgument, 없음 NotFound(진단 None), 같은 경로 키 둘 DuplicatePath / AlreadyExists,
    // 고아가 아님 InvalidArgument(진단 None), Delete의 오류.
    Result<void> DeleteOrphan(const AssetDatabaseLock& lock, StringView path,
                              AssetDeleteResult& result, Array<EditorWarning>* warnings = nullptr,
                              AssetDiagnostic* failure = nullptr) noexcept;

    // asset-import <path> <source>… [--importer=<이름>] [설정…](7.1절 18 A2f1b): sources를 읽어
    // 임포터(importer 이름 — 비면 첫 원본 확장자의 기본, 원본이 여럿이면 모든 원본 확장자의 기본이
    // 같아야 한다)로 출력을 만들고 ImportRecord와 함께 저장한다. assignments는 첫 이름이 옵션
    // struct 필드면 옵션(ImportRecord options), 아니면 주 출력의 설정(임포트 뒤 대입)이다. 저장은
    // 다른 출력(참조 기록·ImportOwner) 먼저, 주 출력(소유 기록) 마지막이다. 저장 전 실패면 아무것도
    // 쓰지 않고, 중간 저장 실패면 이번에 쓴 출력 파일을 지운다(못 지우면 경고 OrphanNotDeleted).
    // 경고: ImportWarning(AbsoluteSourcePath·임포터 경고)·NotDurable·OrphanNotDeleted. 만든
    // 패키지는 성공하면 열린 채, 실패하면 닫는다.
    //
    // 오류(failure 진단): Initialize 전·잠금 아님·원본 없음 InvalidArgument, 경로 규칙 InvalidPath,
    // 별칭·점유 DuplicatePath / AlreadyExists, 임포터 없음(이름·확장자·표 없음)·원본 확장자의
    // 기본이 다름 UnsupportedSourceFormat / NotFound·Unsupported, 역할이 임포터 roles와 다름·옵션
    // 대입 오류 InvalidImportOption / InvalidArgument, 원본 루트 표·원본 위치(상대 경로 등)·읽기
    // 오류 InvalidSource / 그 오류, 원본 없음 MissingSource / NotFound, 원본 합 상한 InvalidSource
    // / CapacityExceeded, 옵션도 설정도 아닌 이름 UnknownProperty / InvalidArgument(detail = 대입
    // 번호), 임포터 실패(Fail로 남긴 진단 또는 None), 주 출력을 만들지 않음 InvalidArgument, 설정
    // 대입 오류 InvalidArgument(진단 None — diagnostics), 기록 쓰기·저장의 오류, 할당 실패.
    //
    // 사용 예:
    //   const AssetImportSourceInput sources[] = {{DefaultImportSourceRole, "C:/Art/Rock.png"}};
    //   AssetImportResult result(allocator);
    //   auto imported = service.Import(lock, "/Game/Rock", sources, {}, assignments, diagnostics,
    //                                  result, &warnings, &failure);
    Result<void> Import(const AssetDatabaseLock& lock, StringView path,
                        Span<const AssetImportSourceInput> sources, StringView importer,
                        Span<const reflection_text::TextAssignment> assignments,
                        reflection_text::TextDiagnostics& diagnostics, AssetImportResult& result,
                        Array<EditorWarning>* warnings = nullptr,
                        AssetDiagnostic* failure = nullptr) noexcept;

    // asset-reimport <path> [--source=<파일> | --source.<역할>=<파일>…] [설정…](7.1절 18 A2f2a):
    // path(지금 이름 — 별칭으로 풀지 않는다)의 소유 기록대로 원본을 다시 읽어 기록의 임포터로
    // 출력을 제자리 갱신한다. overrides는 원본 바꾸기다(역할이 빈 하나 = `--source=` — 기록 원본이
    // 하나일 때만, 아니면 기록에 있는 역할). 옵션은 기록 Record에 명시 대입만 더한다. 기록 출력은
    // AssetId로 찾는다(이동해도 따라간다 — 색인에 없으면 그 key는 새 출력). 임포터가 청하지 않은
    // 기존 출력은 경고 OrphanedOutput. 저장·되돌리기·경고는 Import와 같다(이미 저장한 기존 출력은
    // 실패해도 새 내용으로 남는다 — 같은 재임포트를 다시 하면 수렴한다).
    //
    // 오류(failure 진단): Initialize 전·잠금 아님 InvalidArgument, 경로 규칙 InvalidPath, 없음
    // NotFound(진단 None), 같은 경로 키 둘 DuplicatePath, 같은 id 둘 DuplicateAssetId(주 출력·출력
    // — asset), 읽기 전용 NewerVersion / Unsupported, 기록 없음·참조 기록(related = owner)·출력의
    // 기록이 이 owner의 참조 기록이 아님(asset·related) InvalidArgument, 주 출력·출력이 열려 있음
    // Busy(asset), 임포터 없음 UnsupportedSourceFormat / Unsupported, 기록 임포터 버전이 더 새
    // NewerVersion / Unsupported, 기록 역할이 임포터 roles와 다름·모르는 바꾸기 역할·`--source=`의
    // 원본 수·옵션 기록 형식 InvalidImportOption, 모르는 원본 루트 MissingSource(detail = 원본
    // 번호), Import의 원본·대입·임포터·출력(타입이 다른 기존 출력 TypeMismatch / InvalidArgument)·
    // 저장 오류, 할당 실패.
    Result<void> Reimport(const AssetDatabaseLock& lock, StringView path,
                          Span<const AssetImportSourceInput> overrides,
                          Span<const reflection_text::TextAssignment> assignments,
                          reflection_text::TextDiagnostics& diagnostics, AssetImportResult& result,
                          Array<EditorWarning>* warnings = nullptr,
                          AssetDiagnostic* failure = nullptr) noexcept;

    // asset-fix-duplicate-id <path>: 같은 id 파일 둘 이상 가운데 path의 파일을 새 id로 다시 쓴다
    // (7.1절 17 A2e4b — EditorPackageSet::FixDuplicateId). 경로는 지금 이름이다(별칭으로 풀지
    // 않는다). 고아가 생기면 경고 OrphanedOutput.
    //
    // 오류(failure 진단): Initialize 전·잠금 아님 InvalidArgument, 경로 규칙 InvalidPath /
    // InvalidArgument, 없음 NotFound(진단 None), 같은 경로 키 둘 DuplicatePath / AlreadyExists,
    // 같은 id가 아님 InvalidArgument(진단 None·asset), id 발급 실패, FixDuplicateId의 오류, 할당
    // 실패.
    Result<void> FixDuplicateId(const AssetDatabaseLock& lock, StringView path,
                                AssetFixIdResult& result, Array<EditorWarning>* warnings = nullptr,
                                AssetDiagnostic* failure = nullptr) noexcept;

    // asset-duplicate <path|AssetId> <새 path>: 원본을 열고(열기와 같다 — 별칭을 따른다) 그 메모리
    // 상태를 새 id로 새 경로에 쓴다(7.1절 17 A2e3b — EditorPackageSet::Duplicate). 대상 경로가
    // 어느 에셋의 별칭이든 점유다(새 에셋에는 별칭이 없다). 사본은 열지 않는다.
    //
    // 오류(failure 진단): Initialize 전·잠금 아님 InvalidArgument, 대상 경로 규칙 InvalidPath /
    // InvalidArgument, 별칭 표의 오류, 대상이 별칭 DuplicatePath / AlreadyExists(related = 별칭
    // 대상), 원본 열기의 오류(OpenTarget), id 발급 실패(진단 None), EditorPackageSet::Duplicate의
    // 오류, 할당 실패.
    Result<void> Duplicate(const AssetDatabaseLock& lock, StringView source, StringView target,
                           AssetDuplicateResult& result, Array<EditorWarning>* warnings = nullptr,
                           AssetDiagnostic* failure = nullptr) noexcept;

    // 별칭 표다(처음 부를 때 packages.files에서 읽는다 — 7.1절 17 A2e1, 실패하면 다음 호출이 다시
    // 읽는다). 오류(failure 진단): Initialize 전 InvalidArgument, AssetAliasTable::Load의 오류.
    Result<AssetAliasTable*> Aliases(AssetDiagnostic* failure = nullptr) noexcept;

    // asset-alias-remove <옛 path>: 그 경로 키의 별칭을 지워 쓴다. notDurable은 교체 뒤 동기화를
    // 확인하지 못했는지다. 오류(failure 진단): Aliases의 오류, 경로 규칙 위반 InvalidPath /
    // InvalidArgument, 별칭이 없으면 NotFound(진단 None), AssetAliasTable::Write의 오류.
    Result<void> AliasRemove(const AssetDatabaseLock& lock, StringView path, bool& notDurable,
                             AssetDiagnostic* failure = nullptr) noexcept;

    // asset-alias-prune: 지금 시각(config.clock)으로 만료 항목 가운데 Config 텍스트에서 쓰이지 않은
    // 것을 지운다(AssetAliasTable::Prune). 오류: Aliases의 오류, Prune의 오류.
    Result<void> AliasPrune(const AssetDatabaseLock& lock, AssetAliasPruneReport& report,
                            AssetDiagnostic* failure = nullptr) noexcept;

private:
    // path의 별칭 대상 id다(경고 AliasResolved — 별칭 표 읽기 실패는 failure 진단). 오류: 별칭이
    // 없으면 NotFound, Aliases의 오류, 할당 실패.
    Result<AssetId> ResolveAliasPath(const LogicalPath& path, Array<EditorWarning>* warnings,
                                     AssetDiagnostic* failure) noexcept;

    // 해석기에 주는 별칭 길이다(서비스의 지연 적재 별칭 표).
    class AliasLookup final : public AssetAliasResolver {
    public:
        explicit AliasLookup(AssetService& service) noexcept : service_(&service) {
        }

        Result<AssetId> ResolveAlias(const LogicalPath& path,
                                     Array<EditorWarning>* warnings) noexcept override;

    private:
        AssetService* service_;
    };

    // `<path|AssetId>`의 패키지를 열고(열린 패키지면 그대로) 대표 에셋을 준다. id에 패키지 id를
    // 둔다.
    Result<ObjectHandle> OpenTarget(StringView target, AssetId& id, Array<EditorWarning>* warnings,
                                    AssetDiagnostic* failure) noexcept;
    // 새 임포트·재임포트가 같이 쓰는 실행이다(AssetServiceImport.cpp — 원본 읽기·옵션·임포터·설정·
    // 저장·되돌리기).
    struct ImportRun;
    Result<void> RunImport(ImportRun& run) noexcept;

    // 타입 도구 이름의 클래스 lease다(에셋 클래스만이면 requireAsset).
    Result<TypeLease> FindClass(StringView name, bool requireAsset,
                                AssetDiagnostic* failure) noexcept;

    Allocator* allocator_;
    const ProjectLayout* layout_;
    AssetIndex index_;
    EditorPackageSet set_;
    PackageReadOptions read_;
    TypeRegistry* types_;
    platform::FileSystem* files_;
    platform::FileSystem* cacheFiles_;
    bool useCache_;
    AssetAliasTable aliases_;
    AliasLookup aliasLookup_;
    SystemAssetClock systemClock_;
    AssetClock* clock_;
    uint32 aliasDays_;
    AssetAliasSearchLimits aliasSearch_;
    AssetImporterTable* importers_;
    bool initialized_ = false;
};

} // namespace rocks
