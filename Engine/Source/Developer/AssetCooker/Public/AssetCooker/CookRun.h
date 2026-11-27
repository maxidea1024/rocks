#pragma once

#include <AssetCooker/CookKeys.h>
#include <AssetDatabase/EditorPackageSet.h>
#include <AssetRuntime/CookedManifest.h>

// 쿠킹 실행이다(Docs/Design/AssetSystem 10.2절 결정 7, "A4c 구현 세부" — A4c2). 키가 있는
// 대상을 Hard SCC 역위상(의존 먼저, SCC 안은 AssetId 순)으로 열고 처리기로 쿠킹해 출력 sink에
// 하나씩 넘긴다. 열린 집합은 이번 쿠킹이 연 패키지 수가 상한을 넘으면 한꺼번에 닫는다(UE가 메모리
// 압박에서 GC로 내리는 것과 같은 방식 — 결정적인 수 기준). 에셋별 오류는 진단으로 모으고 끝까지
// 진행한다.
//
// 사용 예:
//   CookRunRequest request;
//   request.index = &index;
//   request.packages = &set;          // Initialize한 EditorPackageSet(dirty 없음)
//   request.processors = &processors;
//   request.targets = &targets;
//   request.keys = &keys;
//   request.profile = &profile;
//   request.profileSettings = profileHash;
//   request.output = &publisher;      // A4d 게시가 staging에 쓴다
//   CookRunResult result(allocator);
//   if (auto cooked = CookPackages(allocator, request, result); !cooked) { ... }
namespace rocks {

// 이번 쿠킹이 열어 둘 패키지 수의 기본 상한이다(넘으면 시작 때 열린 것만 남기고 닫는다).
inline constexpr usize CookMaxOpenPackages = 4096;

// 쿠킹한 패키지 하나다(포인터·span은 Put 동안만 유효하다). 매니페스트 항목의 재료다(A4d).
struct CookedPackage {
    AssetId asset{};
    // 대표 에셋 타입이다.
    TypeId type{};
    // 논리 경로(매니페스트 경로 힌트)다.
    StringView pathHint{};
    // CookedManifestFlags 비트 합이다(PrimaryAsset).
    uint32 flags = 0;
    // PrimaryAsset bundle이다(이름 순, 구성원 AssetId 순).
    Span<const CookTargetBundle> bundles{};
    // 매니페스트 의존이다: 쿠킹 패키지 Dependencies 그대로((asset, kind) 순 — 로더가 같은지
    // 대조한다, AS 11.6절 3). NeverCook·색인에 없는 Soft 대상도 남는다(매니페스트 항목이 없어
    // 로드가 NotFound).
    Span<const CookedManifestDependency> dependencies{};
    // 쿠킹 패키지 byte와 SHA-256, BulkIndex rawBytes 합이다.
    Span<const uint8> bytes{};
    Sha256Digest sha256{};
    uint64 bulkRawBytes = 0;
    Sha256Digest cookKey{};
};

// 쿠킹 결과를 받는 곳이다(A4d 게시 — 시험은 메모리). Put의 오류는 실행을 멈추고 그대로 돌아온다.
class CookOutputSink {
public:
    virtual Result<void> Put(const CookedPackage& package) noexcept = 0;

protected:
    ~CookOutputSink() = default;
};

struct CookRunRequest {
    // 색인이다(대상·키를 만든 같은 색인).
    AssetIndex* index = nullptr;
    // Initialize한 에디터 패키지 집합이다(dirty 패키지가 있으면 Busy — 쿠킹은 디스크 상태).
    EditorPackageSet* packages = nullptr;
    const CookProcessorTable* processors = nullptr;
    const CookTargetSet* targets = nullptr;
    // 대상과 같은 순서의 키다(ComputeCookKeys).
    const CookKeySet* keys = nullptr;
    const TargetProfile* profile = nullptr;
    Sha256Digest profileSettings{};
    uint32 cookerVersion = CookerVersion;
    // 이번 쿠킹이 열어 둘 패키지 수 상한이다(0이면 InvalidArgument).
    usize maxOpenPackages = CookMaxOpenPackages;
    // 재사용 표지다(대상과 같은 수 — 0이 아니면 그 대상은 열지 않고 건너뛴다, PlanCookReuse). 비면
    // 재사용 없음이다.
    Span<const uint8> reuse{};
    // 처리기에 넘길 너겟 캐시와 verify 표지다(CookContext로 그대로 넘긴다).
    NuggetCache* nuggets = nullptr;
    bool verifyNuggets = false;
    // 처리기 안 병렬 실행이다(AssetSystem 12.17.9 4 — 텍스처 블록 압축). scheduler가 있으면
    // scratch는 동시 Allocate·Deallocate가 안전한 allocator여야 한다. 결과 byte와 무관하다(너겟
    // 키에 넣지 않는다).
    WorkerScheduler* scheduler = nullptr;
    Allocator* scratch = nullptr;
    CookOutputSink* output = nullptr;
};

struct CookRunResult {
    explicit CookRunResult(Allocator& allocator) noexcept : diagnostics(allocator) {
    }

    // 경고가 아닌 진단이 있는지다(처리기 경고는 warning = true — 12.14절 10번).
    bool HasErrors() const noexcept {
        for (const CookDiagnostic& diagnostic : diagnostics) {
            if (!diagnostic.warning) {
                return true;
            }
        }

        return false;
    }

    // 진단(CookDiagnosticBefore 순)이다.
    Array<CookDiagnostic> diagnostics;
    // 쿠킹해 넘긴 패키지 수, 대상을 열려고 부른 Open 수(이미 열린 대상은 세지 않는다), 상한으로
    // 한꺼번에 닫은 횟수다.
    uint64 cooked = 0;
    uint64 opens = 0;
    uint64 releases = 0;
};

// 쿠킹한다(result를 비우고 채운다). 키가 없는 대상은 건너뛴다(원인 진단은 키 단계에 있다).
//
// 진단(에셋별 — 계속 진행): 읽기 전용 패키지(더 새 버전 등) UnsupportedVersion, 열기 실패(Open의
// 진단 코드·오류), 미해결 import MissingDependency(related = 대상), 처리기 실패(그 진단 코드·오류),
// 쿠킹 byte 검증 실패 CorruptPackage(error = 읽기 오류), 로더 대조(CheckCookedPackage — 프로파일
// 칸·대표 타입 등) 실패 CorruptPackage(error = 대조 오류).
//
// 오류(멈춤): 요청 인자 null·상한 0·키 수나 재사용 표지 수가 대상 수와 다름·대상이 색인에 없음
// InvalidArgument, 시작
// 때 dirty 패키지 Busy, 처리기 조회 오류, 출력 sink 오류, 닫기 오류, 할당 실패. 멈춰도 이번 쿠킹이
// 연 패키지는 닫는다(닫기 오류는 원래 오류 뒤에 묻힌다). 닫기 자체가 할당에 실패하면 패키지가 열린
// 채 남고 OutOfMemory다 — 호출자가 시작 때 열린 목록으로 EditorPackageSet::CloseOpened를 다시
// 부른다.
ROCKS_ASSETCOOKER_API Result<void> CookPackages(Allocator& allocator, const CookRunRequest& request,
                                                CookRunResult& result) noexcept;

} // namespace rocks
