#pragma once

#include <AssetCooker/CookPublish.h>
#include <Meta/Annotations.h>

// 쿠킹 전체 흐름과 보고서다(Docs/Design/AssetSystem.ko.md 10.2절 결정 11, "A4d 구현 세부" — A4d2).
// 기존 매니페스트 읽기 → 대상(A4b) → 키(A4c1) → 재사용 판정(A4d1) → 실행(A4c2, 출력 = 게시기) →
// 오류 진단이 없으면 게시 → 보고서. 색인은 호출자가 쿠킹 직전에 갱신해 넘긴다(콘텐츠 해시의 Stat
// 한계 — A4b 상태).
//
// 사용 예(asset-cook 명령 — A4e):
//   CookProjectRequest request;
//   request.files = &files; request.layout = &layout; request.lock = &lock;
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

// 쿠킹 보고서(`<쿠킹 루트>/CookReport.json`)다. 도구 빌드 ID·엔진 버전은 진단용이다(10절 — 병합·
// 재사용 판정에 쓰지 않는다).
ROCKS_STRUCT()
struct CookReport {
    ROCKS_GENERATED_BODY()

    explicit CookReport(Allocator& allocator) noexcept
        : toolBuildId(allocator), engineVersion(allocator), profile(allocator),
          diagnostics(allocator) {
    }

    ROCKS_PROPERTY() String toolBuildId;
    ROCKS_PROPERTY() String engineVersion;
    ROCKS_PROPERTY() String profile;
    ROCKS_PROPERTY() bool partial = false;
    ROCKS_PROPERTY() bool published = false;
    ROCKS_PROPERTY() uint32 targets = 0;
    ROCKS_PROPERTY() uint32 cooked = 0;
    ROCKS_PROPERTY() uint32 reused = 0;
    ROCKS_PROPERTY() uint64 bytesWritten = 0;
    ROCKS_PROPERTY() Array<CookReportDiagnostic> diagnostics;
};

struct CookProjectRequest {
    platform::FileSystem* files = nullptr;
    const ProjectLayout* layout = nullptr;
    // 이 프로젝트의 잡은 쓰기 잠금이다(게시기가 확인한다).
    const AssetDatabaseLock* lock = nullptr;
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
};

struct CookProjectResult {
    explicit CookProjectResult(Allocator& allocator) noexcept : diagnostics(allocator) {
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
};

// 쿠킹한다(result를 비우고 채운다). 오류 진단이 하나라도 있으면(대상·키·실행·게시) 게시하지 않고,
// 끝까지 오면 성공·실패와 상관없이 보고서를 쓴다. 기존 매니페스트가 깨졌으면 경고(그 진단 코드)를
// 남기고 없는 것으로 본다(전체 쿠킹). 오류(멈춤 — 보고서 없음): 요청 인자 null InvalidArgument, 각
// 단계의 함수 오류(색인·TypeRegistry·IO·잠금 아님 등), 기존 매니페스트 읽기 IO 오류·크기 상한
// CapacityExceeded, 보고서 쓰기 오류, 할당 실패.
ROCKS_ASSETCOOKER_API Result<void> CookProject(Allocator& allocator,
                                               const CookProjectRequest& request,
                                               CookProjectResult& result) noexcept;

} // namespace rocks
