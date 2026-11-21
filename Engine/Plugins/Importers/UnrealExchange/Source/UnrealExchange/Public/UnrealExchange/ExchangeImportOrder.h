#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <UnrealExchange/ExchangeBundle.h>
#include <UnrealExchange/ExchangeDiagnostic.h>

// Unreal 교환 묶음의 임포트 순서다(Docs/Design/UnrealExchange.ko.md 9.3절, 10.4절 13–15번). 읽은
// 묶음의 Hard·Build 참조 그래프를 강한 연결 요소(SCC)로 묶어 위상 정렬하고, 에셋마다 Rocks 논리
// 경로(9.1절)를 정한다. 저장 단위(SCC 하나 또는 에셋 하나)는 임포트(UX1b)가 원자적으로 저장한다.
//
// 사용 예:
//   ExchangeImportPlan plan(allocator);
//   ExchangeOrderOptions options;
//   options.root = "/Game/Unreal";
//   if (auto planned = PlanExchangeImport(bundle, options, plan, diagnostics); !planned) { ... }
//   for (const ExchangeImportUnit& unit : plan.Units()) {
//       if (unit.state != ExchangeUnitState::Ready) { continue; }
//       for (uint32 asset : plan.Members(unit)) { /* plan.MappedPath(asset) */ }
//   }

namespace rocks {

// 묶음에 없는 objectPath의 Rocks 에셋이 이미 있는지 알려 주는 조회다(9.3절 — UX1b가 AssetIndex의
// UnrealExchange.ObjectPath 태그로 구현한다). PlanExchangeImport 동안만 빌린다.
class ExchangeExistingAssets {
public:
    virtual ~ExchangeExistingAssets() = default;

    virtual bool Contains(StringView objectPath) const noexcept = 0;
};

struct ExchangeOrderOptions {
    // 대응 경로의 뿌리(10.2절 --root)다. 논리 경로 규칙을 어기면 InvalidPath 오류다.
    StringView root = "/Game/Unreal";
    // 기존 에셋 조회다. null이면 묶음에 없는 대상은 모두 없는 것으로 본다.
    const ExchangeExistingAssets* existing = nullptr;
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

    // 에셋이 속한 단위 번호(Units 안)다. Unsupported 에셋이면 ExchangeNoIndex다.
    uint32 UnitOf(uint32 asset) const noexcept;

    // 에셋의 대응 논리 경로다. Unsupported 에셋이나 대응 실패(UnrealPathTooLong)면 빈 view다.
    StringView MappedPath(uint32 asset) const noexcept;

    // 경로 정리로 이름이 바뀐 에셋이면 참이다(UnrealPathRenamed).
    bool Renamed(uint32 asset) const noexcept;

    void Clear() noexcept;

private:
    friend struct ExchangePlanAccess;

    Allocator* allocator_;
    Array<ExchangeImportUnit> units_;
    Array<uint32> members_;
    Array<uint32> unitOf_;
    // 에셋마다 (시작, 길이) — pathText_ 안 대응 경로다.
    Array<uint32> pathStart_;
    Array<uint32> pathBytes_;
    Array<uint8> renamed_;
    Array<char> pathText_;
};

// bundle의 임포트 순서와 대응 경로를 out에 채운다(out은 먼저 비운다). 에셋·단위 실패는 진단과
// 단위 state로 알리고 성공한다(9.3절 부분 결과).
//
// 반환: root가 논리 경로 규칙을 어기면 InvalidArgument(InvalidPath 진단, out 빈 상태), 할당 실패는
// 진단 없이 그 오류다.
Result<void> PlanExchangeImport(const ExchangeBundle& bundle, const ExchangeOrderOptions& options,
                                ExchangeImportPlan& out, ExchangeDiagnostics& diagnostics) noexcept;

} // namespace rocks
