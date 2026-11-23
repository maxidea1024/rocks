#pragma once

#include <AssetCooker/CookConfig.h>
#include <AssetDatabase/AssetIndex.h>
#include <AssetRuntime/CookedManifest.h>

// 쿠킹 대상 계산이다(Docs/Design/AssetSystem.ko.md 10.2절 결정 4, "A4b 구현 세부"). 색인만 쓰고
// 패키지를 열지 않는다(UE가 AssetRegistry로 쿠킹 대상을 세는 것과 같다). 규칙 위반은 진단으로
// 모으고 함수 오류는 색인 조회 오류·할당 실패뿐이다(쿠킹은 오류 진단이 하나라도 있으면 게시하지
// 않는다).
//
// 사용 예:
//   CookTargetRequest request;
//   request.index = &index;
//   request.types = &types;
//   request.settings = &settings;
//   CookTargetSet targets(allocator);
//   if (auto computed = ComputeCookTargets(allocator, request, targets); !computed) { ... }
//   if (targets.HasErrors()) { /* 진단 보고 — 게시하지 않음 */ }
namespace rocks {

// PrimaryAsset bundle 구성원 태그의 키 접두다(`Bundle.<이름>` = 구성원 AssetId 표준 문자열,
// 구성원마다 태그 하나 — 저장 때 A4e가 쓴다).
inline constexpr StringView CookBundleTagPrefix = "Bundle.";

// 쿠킹 진단이다. 같은 코드가 경고·오류로 쓰이므로(NeverCookReferenced·MissingDependency·
// InvalidPath) 수준을 따로 둔다.
struct CookDiagnostic {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    bool warning = false;
    // 대상 에셋(참조자)과 원인 에셋(의존)이다. 모르면 null이다.
    AssetId asset{};
    AssetId related{};
    // 설정·인자 항목의 순번이다(InvalidPath·NeverCookReferenced의 alwaysCook·인자 경로, 그 밖 0).
    uint32 setting = 0;
    // 원인 오류다(콘텐츠 해시의 파일 오류 등 — 없으면 None, A4c).
    ErrorCode error = ErrorCode::None;
};

// 진단 순서다((asset, code, related, setting, 오류 먼저, 원인 오류) — 보고서·시험이 결정적으로
// 읽는다).
constexpr bool CookDiagnosticBefore(const CookDiagnostic& left,
                                    const CookDiagnostic& right) noexcept {
    if (!(left.asset == right.asset)) {
        return GuidLess(left.asset.value, right.asset.value);
    }

    if (left.code != right.code) {
        return left.code < right.code;
    }

    if (!(left.related == right.related)) {
        return GuidLess(left.related.value, right.related.value);
    }

    if (left.setting != right.setting) {
        return left.setting < right.setting;
    }

    // 같은 칸에 수준만 다른 진단(같은 의존의 Hard·Soft 누락)은 오류가 먼저다.
    if (left.warning != right.warning) {
        return !left.warning;
    }

    return static_cast<uint32>(left.error) < static_cast<uint32>(right.error);
}

struct CookTargetBundle {
    explicit CookTargetBundle(Allocator& allocator) noexcept : name(allocator), members(allocator) {
    }

    String name;
    // 구성원(AssetId 순)이다.
    Array<AssetId> members;
};

struct CookTarget {
    explicit CookTarget(Allocator& allocator) noexcept : bundles(allocator) {
    }

    AssetId id{};
    bool primary = false;
    // PrimaryAsset의 bundle(이름 순)이다.
    Array<CookTargetBundle> bundles;
};

struct CookTargetRequest {
    const AssetIndex* index = nullptr;
    TypeRegistry* types = nullptr;
    const CookSettings* settings = nullptr;
    // 부분 쿠킹의 인자 경로(에셋·디렉터리 — ClassifyCookPath 형식)다. 비면 전체 쿠킹이다.
    Span<const StringView> paths{};
    // 부분 쿠킹의 기존 매니페스트다(없으면 null).
    const CookedManifest* existing = nullptr;
};

struct CookTargetSet {
    explicit CookTargetSet(Allocator& allocator) noexcept
        : targets(allocator), excluded(allocator), removed(allocator), diagnostics(allocator) {
    }

    bool HasErrors() const noexcept {
        for (const CookDiagnostic& diagnostic : diagnostics) {
            if (!diagnostic.warning) {
                return true;
            }
        }

        return false;
    }

    // 대상(AssetId 순)이다.
    Array<CookTarget> targets;
    // Soft로만 닿은 NeverCook 에셋(매니페스트 의존에서 뺄 것 — AssetId 순)이다.
    Array<AssetId> excluded;
    // 기존 매니페스트에 있고 색인에 없는 항목(AssetId 순)이다.
    Array<AssetId> removed;
    // 진단(CookDiagnosticBefore 순)이다.
    Array<CookDiagnostic> diagnostics;
};

// 대상을 계산한다(out을 비우고 채운다). 오류: 요청 인자 null InvalidArgument, 색인·TypeRegistry
// 조회 오류(ListByType의 ShuttingDown 등), 할당 실패. 설정 경로는 ReadCookSettings가 검증했다고
// 본다(형식이 틀린 항목은 InvalidArgument).
ROCKS_ASSETCOOKER_API Result<void> ComputeCookTargets(Allocator& allocator,
                                                      const CookTargetRequest& request,
                                                      CookTargetSet& out) noexcept;

} // namespace rocks
