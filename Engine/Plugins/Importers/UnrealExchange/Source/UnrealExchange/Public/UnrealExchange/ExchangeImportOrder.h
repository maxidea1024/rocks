#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <Object/AssetId.h>
#include <UnrealExchange/ExchangeBundle.h>
#include <UnrealExchange/ExchangeDiagnostic.h>

// Unreal 교환 묶음의 임포트 순서다(Docs/Design/UnrealExchange.ko.md 9.3절, 10.4절 13–15번, 10.5절).
// 읽은 묶음의 Hard·Build 참조 그래프를 강한 연결 요소(SCC)로 묶어 위상 정렬하고, 에셋마다 Rocks
// 논리 경로(9.1절)와 기존 에셋(재임포트 대상, 9.2절)을 정한다. 저장 단위(SCC 하나 또는 에셋 하나)는
// 실행(ExchangeImportRun.h)이 순서대로 임포트한다.
//
// 사용 예:
//   Array<uint8> selection(allocator);
//   auto selected = SelectExchangeClosure(bundle, only, selection);
//   ExchangeImportPlan plan(allocator);
//   ExchangeOrderOptions options;
//   options.existing = &lookup;
//   options.selection = selection.View();
//   auto planned = PlanExchangeImport(bundle, options, plan, diagnostics);
//   for (const ExchangeImportUnit& unit : plan.Units()) {
//       for (uint32 asset : plan.Members(unit)) { /* plan.TargetPath(asset) */ }
//   }

namespace rocks {

// 기존 에셋 조회 결과의 상태다.
enum class ExchangeExistingState : uint8 {
    None,     // 없음 — 새 에셋
    Found,    // 하나 — 그 에셋에 다시 임포트한다
    Ambiguous // 둘 이상 — 어느 쪽인지 고를 수 없다(UnrealPathCollision, AmbiguousExisting)
};

// 기존 에셋 하나다. path는 조회 구현(또는 그것을 복사한 계획)이 살아 있는 동안 유효하다.
struct ExchangeExistingMatch {
    ExchangeExistingState state = ExchangeExistingState::None;
    AssetId id{};
    StringView path;
    // 둘째 단계(옛 경로)로 찾았으면 참이다. 묶음 에셋 둘이 같은 기존 에셋을 찾으면 첫 단계로 찾은
    // 쪽만 그 에셋에 다시 임포트한다(10.5절 5번).
    bool former = false;
};

// UE objectPath의 Rocks 에셋이 이미 있는지 알려 주는 조회다(10.5절 4번 — ExchangeIndexLookup이
// AssetIndex의 UnrealExchange.ObjectPath·FormerPaths 태그로 구현한다). PlanExchangeImport 동안만
// 빌린다.
class ExchangeExistingAssets {
public:
    virtual ~ExchangeExistingAssets() = default;

    // objectPath(지금 UE 경로)와 formerPaths(그 경로로 이어지는 묶음 redirect의 옛 경로들)로
    // 찾는다. 오류: 조회의 할당 실패 등(계획 전체가 그 오류로 실패한다).
    virtual Result<ExchangeExistingMatch>
    Find(StringView objectPath, Span<const StringView> formerPaths) const noexcept = 0;

    // 논리 경로(대소문자 무시)를 이미 쓰는 Rocks 에셋이 있으면 참이다(새 에셋의 대응 경로 점유 —
    // 10.5절 5번). 오류: 조회의 할당 실패 등.
    virtual Result<bool> PathTaken(StringView logicalPath) const noexcept = 0;
};

struct ExchangeOrderOptions {
    // 대응 경로의 뿌리(10.2절 --root)다. 논리 경로 규칙을 어기면 InvalidPath 오류다.
    StringView root = "/Game/Unreal";
    // 기존 에셋 조회다. null이면 기존 에셋은 없는 것으로 본다(모두 새 에셋, 묶음에 없는 대상은
    // 누락).
    const ExchangeExistingAssets* existing = nullptr;
    // 고른 에셋이다(에셋 번호마다 0·1 — 크기는 묶음 에셋 수, SelectExchangeClosure의 결과처럼 Hard
    // 닫힘이어야 한다). 비면 모두 고른다. 경로 대응·겹침·기존 에셋 조회는 선택과 무관하게 묶음
    // 전체에 하고(10.5절 3번 — 결과가 --only 유무와 같다), 진단·단위는 고른 에셋만이다.
    Span<const uint8> selection{};
};

enum class ExchangeUnitState : uint8 {
    Ready,
    Failed
};

// 저장 단위 하나다.
struct ExchangeImportUnit {
    // ExchangeImportPlan::Members 안의 시작 위치와 구성원 수(objectPath 순)다.
    uint32 firstMember = 0;
    uint32 memberCount = 0;
    // 단위 밖 의존 단위 깊이의 최댓값 + 1이다(의존 없으면 0).
    uint32 depth = 0;
    ExchangeUnitState state = ExchangeUnitState::Ready;
    // 실패 이유(구성원 읽기·경로 오류 코드, UnsupportedCycle, MissingDependency)와 원인 에셋
    // 번호다. Ready면 None·ExchangeNoIndex다.
    AssetDiagnosticCode failure = AssetDiagnosticCode::None;
    uint32 failureAsset = ExchangeNoIndex;
};

// 임포트 순서 결과다. 단위는 (깊이, 가장 작은 구성원 objectPath) 순이고 결정적이다.
class ExchangeImportPlan {
public:
    explicit ExchangeImportPlan(Allocator& allocator) noexcept;

    ExchangeImportPlan(const ExchangeImportPlan&) = delete;
    ExchangeImportPlan& operator=(const ExchangeImportPlan&) = delete;

    Span<const ExchangeImportUnit> Units() const noexcept {
        return units_.View();
    }

    // 단위의 구성원 에셋 번호(objectPath 순)다.
    Span<const uint32> Members(const ExchangeImportUnit& unit) const noexcept;

    // 에셋이 속한 단위 번호(Units 안)다. Unsupported 에셋이나 고르지 않은 에셋이면
    // ExchangeNoIndex다.
    uint32 UnitOf(uint32 asset) const noexcept;

    // 고른 에셋이면 참이다(선택이 비었으면 모두).
    bool Selected(uint32 asset) const noexcept;

    // 에셋의 대응 논리 경로다. Unsupported 에셋이나 대응 실패(UnrealPathTooLong)면 빈 view다.
    StringView MappedPath(uint32 asset) const noexcept;

    // 경로 정리로 이름이 바뀐 에셋이면 참이다(UnrealPathRenamed).
    bool Renamed(uint32 asset) const noexcept;

    // 에셋의 기존 에셋 조회 결과다(path는 이 계획이 가진 사본). 조회가 없었거나 Unsupported
    // 에셋이면 None이다.
    ExchangeExistingMatch Existing(uint32 asset) const noexcept;

    // 임포트할 논리 경로다: 기존 에셋이 있으면 그 경로(9.1절 — 기존 위치 우선), 아니면 대응 경로.
    StringView TargetPath(uint32 asset) const noexcept;

    void Clear() noexcept;

private:
    friend struct ExchangePlanAccess;

    Allocator* allocator_;
    Array<ExchangeImportUnit> units_;
    Array<uint32> members_;
    Array<uint32> unitOf_;
    // 에셋마다 (시작, 길이) — pathText_ 안 대응 경로·기존 에셋 경로다.
    Array<uint32> pathStart_;
    Array<uint32> pathBytes_;
    Array<uint32> existingStart_;
    Array<uint32> existingBytes_;
    Array<AssetId> existingId_;
    Array<ExchangeExistingState> existingState_;
    Array<uint8> renamed_;
    Array<uint8> selected_;
    Array<char> pathText_;
};

// --only 값들(objectPath 또는 묶음 redirect의 옛 경로)에서 묶음 안 Hard 대상을 끝까지 따라간
// 선택을 out(에셋마다 0·1)에 쓴다(10.5절 2번). only가 비면 모두 고른다. Soft 의존·범위 밖 대상은
// 따라가지 않는다. 범위 밖 클래스 에셋도 직접 고르면 고른다.
//
// 반환: only 값이 묶음에 없으면 NotFound이고 missing(있으면)에 그 값의 번호를 둔다. 할당 실패.
Result<void> SelectExchangeClosure(const ExchangeBundle& bundle, Span<const StringView> only,
                                   Array<uint8>& out, uint32* missing = nullptr) noexcept;

// bundle의 임포트 순서와 대응 경로를 out에 채운다(out은 먼저 비운다). 에셋·단위 실패는 진단과
// 단위 state로 알리고 성공한다(9.3절 부분 결과).
//
// 반환: root가 논리 경로 규칙을 어기면 InvalidArgument(InvalidPath 진단, out 빈 상태), selection
// 크기가 에셋 수와 다르거나 Hard 닫힘이 아니면 InvalidArgument(진단 없음, out 빈 상태), 기존 에셋
// 조회의 오류와 할당 실패는 진단 없이 그 오류다.
Result<void> PlanExchangeImport(const ExchangeBundle& bundle, const ExchangeOrderOptions& options,
                                ExchangeImportPlan& out, ExchangeDiagnostics& diagnostics) noexcept;

} // namespace rocks
