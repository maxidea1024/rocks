#pragma once

#include <AssetCooker/CookArtifacts.h>
#include <AssetCooker/CookPublish.h>
#include <Meta/Annotations.h>

// 쿠킹 전체 흐름과 보고서다(Docs/Design/AssetSystem 10.2절 결정 11, "A4d 구현 세부" — A4d2).
// 기존 매니페스트 읽기 → 대상(A4b) → 키(A4c1) → 재사용 판정(A4d1) → 실행(A4c2, 출력 = 게시기) →
// 비에셋 산출물 단계(10.3.1절 A4f3) → 오류 진단이 없으면 게시 → 보고서. 색인은 호출자가 쿠킹 직전에
// 갱신해 넘긴다(콘텐츠 해시의 Stat 한계 — A4b 상태).
//
// 사용 예(asset-cook 명령 — A4e):
//   CookProjectRequest request;
//   request.files = &files; request.layout = &layout; request.lock = &lock;
//   request.cookLock = &cookLock;   // 쿠킹 루트 잠금(에셋 DB 잠금 뒤에 잡는다)
//   request.index = &index; request.packages = &set; request.types = &types;
//   request.processors = &processors; request.profile = &profile; request.settings = &settings;
//   request.profileSettings = profileHash; request.cookSettings = settingsHash;
//   CookProjectResult result(allocator);
//   if (auto cooked = CookProject(allocator, request, result); !cooked) { ... }
//   if (!result.published) { /* result.diagnostics 보고 */ }
namespace rocks {

inline constexpr StringView CookReportFileName = "CookReport.json";

// 보고서의 진단 한 줄이다(이름은 코드·오류의 텍스트, AssetId는 표준 문자열 — null이면 빈 값).
ROCKS_STRUCT()
struct CookReportDiagnostic {
    ROCKS_GENERATED_BODY()

    explicit CookReportDiagnostic(Allocator& allocator) noexcept
        : code(allocator), level(allocator), asset(allocator), related(allocator),
          error(allocator) {
    }

    ROCKS_PROPERTY() String code;
    // "error" 또는 "warning"이다.
    ROCKS_PROPERTY() String level;
    ROCKS_PROPERTY() String asset;
    ROCKS_PROPERTY() String related;
    ROCKS_PROPERTY() uint32 setting = 0;
    ROCKS_PROPERTY() String error;
};

// 보고서의 artifact 한 줄이다(이번 쿠킹이 만들거나 재사용한 것 — 10.3.1절 A4f3 5번).
ROCKS_STRUCT()
struct CookReportArtifact {
    ROCKS_GENERATED_BODY()

    explicit CookReportArtifact(Allocator& allocator) noexcept
        : kind(allocator), name(allocator), format(allocator), sha256(allocator) {
    }

    // "ShaderLibrary"다.
    ROCKS_PROPERTY() String kind;
    ROCKS_PROPERTY() String name;
    ROCKS_PROPERTY() String format;
    ROCKS_PROPERTY() uint64 bytes = 0;
    // 소문자 hex다.
    ROCKS_PROPERTY() String sha256;
    ROCKS_PROPERTY() bool reused = false;
};

// 보고서의 단계 값 한 줄이다(stage = 단계 정체성의 processor Guid 문자열).
ROCKS_STRUCT()
struct CookReportStat {
    ROCKS_GENERATED_BODY()

    explicit CookReportStat(Allocator& allocator) noexcept : stage(allocator), name(allocator) {
    }

    ROCKS_PROPERTY() String stage;
    ROCKS_PROPERTY() String name;
    ROCKS_PROPERTY() uint64 value = 0;
};

// 쿠킹 보고서(`<쿠킹 루트>/CookReport.json`)다. 도구 빌드 ID·엔진 버전은 진단용이다(10절 — 병합·
// 재사용 판정에 쓰지 않는다).
ROCKS_STRUCT()
struct CookReport {
    ROCKS_GENERATED_BODY()

    explicit CookReport(Allocator& allocator) noexcept
        : toolBuildId(allocator), engineVersion(allocator), profile(allocator),
          diagnostics(allocator), artifacts(allocator), stageStats(allocator) {
    }

    ROCKS_PROPERTY() String toolBuildId;
    ROCKS_PROPERTY() String engineVersion;
    ROCKS_PROPERTY() String profile;
    ROCKS_PROPERTY() bool partial = false;
    ROCKS_PROPERTY() bool published = false;
    ROCKS_PROPERTY() uint32 targets = 0;
    ROCKS_PROPERTY() uint32 cooked = 0;
    ROCKS_PROPERTY() uint32 reused = 0;
    // 이번 실행이 새로 쓴 패키지·artifact byte다(게시 여부와 무관 — 게시하지 않은 실행의 파일은
    // 다음 전체 쿠킹이 지운다).
    ROCKS_PROPERTY() uint64 bytesWritten = 0;
    ROCKS_PROPERTY() Array<CookReportDiagnostic> diagnostics;
    ROCKS_PROPERTY() Array<CookReportArtifact> artifacts;
    ROCKS_PROPERTY() Array<CookReportStat> stageStats;
};

struct CookProjectRequest {
    platform::FileSystem* files = nullptr;
    const ProjectLayout* layout = nullptr;
    // 이 프로젝트의 잡은 쓰기 잠금과 그 뒤에 잡은 쿠킹 루트 잠금이다(10.3.1절 A4f4 2번 — 둘 다
    // 쥐었는지 확인한다).
    const AssetDatabaseLock* lock = nullptr;
    const CookRootLock* cookLock = nullptr;
    // 쿠킹 직전에 갱신한 색인과 Initialize한 에디터 패키지 집합(dirty 없음)이다. 보고서 codec은
    // 집합의 Registry를 쓴다.
    AssetIndex* index = nullptr;
    EditorPackageSet* packages = nullptr;
    TypeRegistry* types = nullptr;
    const CookProcessorTable* processors = nullptr;
    const TargetProfile* profile = nullptr;
    Sha256Digest profileSettings{};
    const CookSettings* settings = nullptr;
    Sha256Digest cookSettings{};
    // 부분 쿠킹의 인자 경로다(비면 전체 쿠킹). 기존 매니페스트가 없으면 부분 쿠킹은 전체 쿠킹과
    // 같다 (결정 10 — 인자를 무시한다).
    Span<const StringView> paths{};
    Guid projectGeneration{};
    uint32 cookerVersion = CookerVersion;
    EngineVersion cookedBy{};
    usize maxOpenPackages = CookMaxOpenPackages;
    NuggetCache* nuggets = nullptr;
    // 참이면 처리기가 너겟 적중을 다시 만들어 대조한다(CookContext::verifyNuggets). 재사용하지
    // 않는다 (재사용 대상은 처리기를 거치지 않아 대조가 빈다 — 10.2절 "A4e 구현 세부").
    bool verifyNuggets = false;
    // 처리기 안 병렬 실행이다(CookRunRequest로 넘긴다)(AssetSystem 12.17.9 4 — 텍스처 블록 압축).
    // scheduler가 있으면 scratch는 동시 Allocate·Deallocate가 안전한 allocator여야 한다. 결과
    // byte와 무관하다(너겟 키에 넣지 않는다).
    WorkerScheduler* scheduler = nullptr;
    Allocator* scratch = nullptr;
    // 기존 매니페스트 크기 상한이다(넘으면 깨진 것으로 본다 — 시험이 낮춘다).
    uint64 manifestLimitBytes = CookedManifestMaxFileBytes;
    // 비에셋 산출물 단계 표다(null이면 빈 표 — asset-cook은 CookArtifactStages()). 셰이더 있는
    // 프로필인데 ShaderLibrary 단계가 없으면 오류 진단 UnsupportedCapability다(10.3.1절 A4f3 3번).
    CookArtifactStageTable* stages = nullptr;
};

// 이번 쿠킹의 artifact 한 줄이다(보고서 artifacts).
struct CookArtifactRow {
    explicit CookArtifactRow(Allocator& allocator) noexcept : name(allocator), formatId(allocator) {
    }

    CookArtifactKind kind = CookArtifactKind::ShaderLibrary;
    String name;
    String formatId;
    uint64 bytes = 0;
    Sha256Digest sha256{};
    bool reused = false;
};

// 단계가 보고한 값 한 줄이다(보고서 stageStats).
struct CookArtifactStatRow {
    explicit CookArtifactStatRow(Allocator& allocator) noexcept : name(allocator) {
    }

    Guid stage{};
    String name;
    uint64 value = 0;
};

struct CookProjectResult {
    explicit CookProjectResult(Allocator& allocator) noexcept
        : diagnostics(allocator), artifacts(allocator), stats(allocator), stageFailure(allocator) {
    }

    bool HasErrors() const noexcept {
        for (const CookDiagnostic& diagnostic : diagnostics) {
            if (!diagnostic.warning) {
                return true;
            }
        }

        return false;
    }

    // 모든 단계의 진단(CookDiagnosticBefore 순)이다.
    Array<CookDiagnostic> diagnostics;
    uint64 targets = 0;
    uint64 cooked = 0;
    uint64 reused = 0;
    uint64 bytesWritten = 0;
    bool partial = false;
    bool published = false;
    // 비에셋 산출물 단계의 artifact·값이다.
    Array<CookArtifactRow> artifacts;
    Array<CookArtifactStatRow> stats;
    // 단계가 CookProject를 멈춘 원인이다(call None이면 없음 — 계약 위반·단계 오류, 10.3.1절 A4f3
    // 3번).
    CookArtifactFailure stageFailure;
};

// 쿠킹한다(result를 비우고 채운다). 오류 진단이 하나라도 있으면(대상·키·실행·단계·게시) 게시하지
// 않고, 끝까지 오면 성공·실패와 상관없이 보고서를 쓴다. 기존 매니페스트가 깨졌으면(상한 초과 포함)
// 경고(그 진단 코드 — 상한 초과는 CorruptPackage / CapacityExceeded)를 남기고 없는 것으로 본다(전체
// 쿠킹). 부분 쿠킹인데 병합 호환(CheckCookedManifestMerge)이 아니면 쿠킹하지 않고 오류
// IncompatibleCook과 보고서로 끝낸다. verifyNuggets면 재사용 없이 모든 대상을 쿠킹한다. 오류(멈춤 —
// 보고서 없음): 요청 인자 null InvalidArgument, 각 단계의 함수 오류(색인·TypeRegistry·IO·잠금 아님
// 등), 단계 계약 위반·단계 오류(그 오류 — 원인은 result.stageFailure), 기존 매니페스트 읽기 IO
// 오류, 보고서 쓰기 오류, 할당 실패. 잠금(에셋 DB·쿠킹 루트)이 이 프로젝트·쿠킹 루트의 잡은
// 잠금이 아니면 InvalidArgument다.
ROCKS_ASSETCOOKER_API Result<void> CookProject(Allocator& allocator,
                                               const CookProjectRequest& request,
                                               CookProjectResult& result) noexcept;

// 엔진 전용 쿠킹 요청이다(10.3절 7항, 10.3.1절 A4f4 3번 — 에셋 대상·색인·패키지 집합이 없다).
struct CookEngineRequest {
    platform::FileSystem* files = nullptr;
    // `Engine/Saved`다(쿠킹 루트 `Engine/Saved/Cooked/<프로필>`).
    StringView savedDirectory{};
    // 그 쿠킹 루트의 잡은 잠금이다.
    const CookRootLock* cookLock = nullptr;
    // 설정·보고서 struct가 등록된 TypeRegistry와 보고서 내보내기의 Registry다.
    TypeRegistry* types = nullptr;
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
    const TargetProfile* profile = nullptr;
    Sha256Digest profileSettings{};
    const CookSettings* settings = nullptr;
    Sha256Digest cookSettings{};
    uint32 cookerVersion = CookerVersion;
    EngineVersion cookedBy{};
    // 단계가 너겟 적중을 다시 만들어 대조한다(재사용 후보도 없다).
    bool verifyNuggets = false;
    CookArtifactStageTable* stages = nullptr;
    uint64 manifestLimitBytes = CookedManifestMaxFileBytes;
};

// 엔진 전용 쿠킹이다(result를 비우고 채운다): 기존 엔진 매니페스트 읽기(깨졌으면 경고 뒤 없음) →
// 게시기 Begin → 비에셋 산출물 단계(engineOnly — `Engine` artifact만, 너겟은 단계가 연다) → 오류
// 진단이 없으면 전체 쿠킹 게시(entryCount 0 — `Shaders/` 정리 포함) → 보고서. 셰이더 없는 프로필은
// 빈 매니페스트를 게시한다. 오류: 요청 인자 null·빈 savedDirectory·잠금이 그 쿠킹 루트의 잡은
// 잠금이 아님 InvalidArgument, 단계 계약 위반·단계 오류(원인은 result.stageFailure), IO 오류, 할당
// 실패.
ROCKS_ASSETCOOKER_API Result<void> CookEngine(Allocator& allocator,
                                              const CookEngineRequest& request,
                                              CookProjectResult& result) noexcept;

} // namespace rocks
