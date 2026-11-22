#include <UnrealExchange/ExchangeImportRun.h>

// Unreal 교환 임포트 실행이다(Docs/Design/UnrealExchange.ko.md 10.5절 7·8번): 단위 순서, 임포터
// 없는 클래스 건너뜀, 실행 중 실패 전파, 임포터 계약 확인.

namespace rocks {

struct ExchangeRunAccess {
    static Array<ExchangeAssetOutcome>& Outcomes(ExchangeImportResult& result) noexcept {
        return result.outcomes_;
    }

    static Array<uint32>& Order(ExchangeImportResult& result) noexcept {
        return result.order_;
    }
};

namespace {

// 계획 실패 진단 코드의 원본 오류다(10.4절 3번 표).
ErrorCode PlanFailureError(AssetDiagnosticCode code) noexcept {
    switch (code) {
    case AssetDiagnosticCode::UnrealPathTooLong:
        return ErrorCode::CapacityExceeded;
    case AssetDiagnosticCode::UnrealPathCollision:
        return ErrorCode::AlreadyExists;
    case AssetDiagnosticCode::UnsupportedCycle:
        return ErrorCode::CycleDetected;
    case AssetDiagnosticCode::MissingDependency:
        return ErrorCode::NotFound;
    default:
        return ErrorCode::InvalidArgument;
    }
}

Result<void> AddRunDiagnostic(ExchangeDiagnostics& diagnostics, AssetDiagnosticCode code,
                              ErrorCode error, ExchangeIssue issue, uint32 asset, uint32 related,
                              StringView text) noexcept {
    ExchangeDiagnosticInput input;
    input.code = code;
    input.error = error;
    input.issue = issue;
    input.asset = asset;
    input.related = related;
    input.text = text;
    return diagnostics.TryAdd(input);
}

void FailOutcome(ExchangeAssetOutcome& outcome, AssetDiagnosticCode code,
                 ErrorCode error) noexcept {
    outcome.state = ExchangeOutcomeState::Failed;
    outcome.failure = code;
    outcome.error = error;
}

// 실패한 계획 단위의 구성원 결과다: 순환은 모두 UnsupportedCycle, 원인 구성원은 그 코드, 나머지는
// MissingDependency(10.4절 15번).
void FailPlannedMember(const ExchangeImportUnit& unit, uint32 member,
                       ExchangeAssetOutcome& outcome) noexcept {
    AssetDiagnosticCode code = AssetDiagnosticCode::MissingDependency;
    if (unit.failure == AssetDiagnosticCode::UnsupportedCycle || member == unit.failureAsset) {
        code = unit.failure;
    }

    FailOutcome(outcome, code, PlanFailureError(code));
}

// 묶음 안 Hard 대상을 본다(10.5절 8번). 이번 실행에서 실패한 대상은 9.3절대로 실패를 전파한다.
// 건너뛴 대상(임포터 없음)은 기존 에셋이 있으면 그것에 잇고, 없으면 텍스처·머티리얼은 경고 뒤
// 비우고 그 밖은 실패다. blocked면 그 에셋은 실패다.
Result<void> CheckRunDependencies(const ExchangeAsset& asset, uint32 member,
                                  Span<const AssetId> resolved,
                                  Span<const ExchangeAssetOutcome> outcomes,
                                  const ExchangeBundle& bundle, ExchangeDiagnostics& diagnostics,
                                  bool& blocked) noexcept {
    blocked = false;
    for (const ExchangeDependency& dependency : asset.dependencies) {
        if (dependency.kind != ExchangeDependencyKind::Hard ||
            dependency.targetClass == ExchangeClass::Unsupported ||
            dependency.target == ExchangeNoIndex || dependency.target == member) {
            continue;
        }

        const bool failed = outcomes[dependency.target].state == ExchangeOutcomeState::Failed;
        if (!failed && !resolved[dependency.target].IsNull()) {
            continue;
        }

        const bool replaceable = !failed && IsExchangeReplaceableTarget(dependency.targetClass);
        if (auto added =
                AddRunDiagnostic(diagnostics,
                                 replaceable ? AssetDiagnosticCode::MissingUnrealDependency
                                             : AssetDiagnosticCode::MissingDependency,
                                 replaceable ? ErrorCode::None : ErrorCode::NotFound,
                                 ExchangeIssue::FailedDependency, member, dependency.target,
                                 bundle.Assets()[dependency.target].objectPath);
            !added) {
            return added;
        }

        if (!replaceable) {
            blocked = true;
            return Result<void>::Success();
        }
    }

    return Result<void>::Success();
}

// 진단 목록 from 뒤에서 asset의 첫 오류 진단 코드다(없으면 None).
AssetDiagnosticCode FirstErrorSince(const ExchangeDiagnostics& diagnostics, usize from,
                                    uint32 asset) noexcept {
    Span<const ExchangeDiagnostic> items = diagnostics.Items();
    for (usize index = from; index < items.Size(); ++index) {
        if (items[index].asset == asset && items[index].severity == ExchangeSeverity::Error) {
            return items[index].code;
        }
    }

    return AssetDiagnosticCode::None;
}

// Ready 단위의 구성원 하나를 실행한다(Hard 순환은 계획이 실패시키므로 Ready 단위는 구성원 하나다).
Result<void> RunMember(const ExchangeRunInput& input, uint32 member, Array<AssetId>& resolved,
                       Array<ExchangeAssetOutcome>& outcomes,
                       ExchangeDiagnostics& diagnostics) noexcept {
    const ExchangeBundle& bundle = *input.bundle;
    const ExchangeAsset& asset = bundle.Assets()[member];
    ExchangeAssetOutcome& outcome = outcomes[member];
    ExchangeKindImporter* importer = input.kinds->Find(asset.exchangeClass);
    if (importer == nullptr) {
        outcome.state = ExchangeOutcomeState::Skipped;
        return AddRunDiagnostic(diagnostics, AssetDiagnosticCode::UnsupportedAsset, ErrorCode::None,
                                ExchangeIssue::NoKindImporter, member, ExchangeNoIndex,
                                ExchangeClassName(asset.exchangeClass));
    }

    bool blocked = false;
    if (auto checked = CheckRunDependencies(asset, member, resolved.View(), outcomes.View(), bundle,
                                            diagnostics, blocked);
        !checked) {
        return checked;
    }

    if (blocked) {
        FailOutcome(outcome, AssetDiagnosticCode::MissingDependency, ErrorCode::NotFound);
        return Result<void>::Success();
    }

    const ExchangeExistingMatch existing = input.plan->Existing(member);
    ExchangeKindRequest request;
    request.bundle = &bundle;
    request.plan = input.plan;
    request.asset = member;
    request.path = input.plan->TargetPath(member);
    request.existing = existing.state == ExchangeExistingState::Found ? existing.id : AssetId{};
    request.options = input.options;
    request.resolved = resolved.View();
    request.service = input.service;
    request.lock = input.lock;
    request.warnings = input.warnings;
    request.diagnostics = &diagnostics;
    const usize before = diagnostics.Items().Size();
    auto imported = importer->Import(request);
    if (!imported) {
        if (imported.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(imported.Error());
        }

        FailOutcome(outcome, FirstErrorSince(diagnostics, before, member), imported.Error());
        return Result<void>::Success();
    }

    // 계약: 새 에셋은 null이 아닌 id, 재임포트는 기존 id 그대로다(어기면 그 에셋의 실패).
    const AssetId id = imported.Value();
    if (id.IsNull() || (!request.existing.IsNull() && id != request.existing)) {
        FailOutcome(outcome, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
        return Result<void>::Success();
    }

    outcome.id = id;
    outcome.state = request.existing.IsNull() ? ExchangeOutcomeState::Created
                                              : ExchangeOutcomeState::Reimported;
    resolved[member] = id;
    return Result<void>::Success();
}

// Ready 단위를 실행한다(dry run이면 계획만).
Result<void> RunUnit(const ExchangeRunInput& input, bool dryRun, const ExchangeImportUnit& unit,
                     Array<AssetId>& resolved, Array<ExchangeAssetOutcome>& outcomes,
                     ExchangeDiagnostics& diagnostics) noexcept {
    const ExchangeImportPlan& plan = *input.plan;
    for (const uint32 member : plan.Members(unit)) {
        if (!dryRun) {
            if (auto ran = RunMember(input, member, resolved, outcomes, diagnostics); !ran) {
                return ran;
            }

            continue;
        }

        const ExchangeExistingMatch existing = plan.Existing(member);
        ExchangeAssetOutcome& outcome = outcomes[member];
        outcome.state = existing.state == ExchangeExistingState::Found
                            ? ExchangeOutcomeState::PlannedReimport
                            : ExchangeOutcomeState::PlannedCreate;
        outcome.id = existing.id;
    }

    return Result<void>::Success();
}

} // namespace

Result<void> ExchangeKindTable::Register(ExchangeClass type,
                                         ExchangeKindImporter& importer) noexcept {
    const usize index = static_cast<usize>(type);
    if (index >= ClassCount) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (importers_[index] != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    importers_[index] = &importer;
    return Result<void>::Success();
}

ExchangeKindImporter* ExchangeKindTable::Find(ExchangeClass type) const noexcept {
    const usize index = static_cast<usize>(type);
    return index < ClassCount ? importers_[index] : nullptr;
}

usize ExchangeImportResult::Count(ExchangeOutcomeState state) const noexcept {
    usize count = 0;
    for (const uint32 asset : order_) {
        count += outcomes_[asset].state == state ? 1 : 0;
    }

    return count;
}

Result<void> RunExchangeImport(const ExchangeRunInput& input, bool dryRun,
                               ExchangeImportResult& out,
                               ExchangeDiagnostics& diagnostics) noexcept {
    const ExchangeBundle& bundle = *input.bundle;
    const ExchangeImportPlan& plan = *input.plan;
    const usize count = bundle.Assets().Size();
    out.Clear();
    Array<ExchangeAssetOutcome>& outcomes = ExchangeRunAccess::Outcomes(out);
    Array<uint32>& order = ExchangeRunAccess::Order(out);
    if (auto resized = outcomes.TryResize(count); !resized) {
        return resized;
    }

    // 참조로 걸 id: 처음에는 기존 에셋, 임포트하면 그 id로 바꾼다.
    Array<AssetId> resolved(outcomes.GetAllocator());
    if (auto resized = resolved.TryResize(count); !resized) {
        return resized;
    }

    for (usize index = 0; index < count; ++index) {
        const ExchangeExistingMatch existing = plan.Existing(static_cast<uint32>(index));
        if (existing.state == ExchangeExistingState::Found) {
            resolved[index] = existing.id;
        }
    }

    // 보고 순서: 단위 구성원 전부, 그 뒤 고른 범위 밖 에셋(크기를 먼저 세어 한 번에 잡는다).
    usize reported = 0;
    for (const ExchangeImportUnit& unit : plan.Units()) {
        reported += unit.memberCount;
    }

    for (usize index = 0; index < count; ++index) {
        const bool unsupported = bundle.Assets()[index].state == ExchangeAssetState::Unsupported;
        reported += unsupported && plan.Selected(static_cast<uint32>(index)) ? 1 : 0;
    }

    if (auto resized = order.TryResize(reported); !resized) {
        return resized;
    }

    usize at = 0;
    for (const ExchangeImportUnit& unit : plan.Units()) {
        for (const uint32 member : plan.Members(unit)) {
            order[at++] = member;
            if (unit.state == ExchangeUnitState::Failed) {
                FailPlannedMember(unit, member, outcomes[member]);
            }
        }

        if (unit.state == ExchangeUnitState::Failed) {
            continue;
        }

        if (auto ran = RunUnit(input, dryRun, unit, resolved, outcomes, diagnostics); !ran) {
            return ran;
        }
    }

    for (usize index = 0; index < count; ++index) {
        if (!plan.Selected(static_cast<uint32>(index)) ||
            bundle.Assets()[index].state != ExchangeAssetState::Unsupported) {
            continue;
        }

        outcomes[index].state = ExchangeOutcomeState::Unsupported;
        order[at++] = static_cast<uint32>(index);
    }

    return Result<void>::Success();
}

} // namespace rocks
