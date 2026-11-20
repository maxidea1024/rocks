#include <UnrealExchange/ExchangeImportOrder.h>

#include <Core/Algorithms.h>
#include <Core/Text/Unicode.h>
#include <UnrealExchange/ExchangePaths.h>

// Unreal 교환 임포트 순서다(Docs/Design/UnrealExchange.ko.md 9.1·9.3절, 10.4절 13–15번): 경로
// 대응과 겹침, Hard·Build 그래프의 SCC(재귀 없는 Tarjan), 단위 깊이·실패 전파.

namespace rocks {

struct ExchangePlanAccess {
    static Allocator& GetAllocator(ExchangeImportPlan& plan) noexcept {
        return *plan.allocator_;
    }

    static Array<ExchangeImportUnit>& Units(ExchangeImportPlan& plan) noexcept {
        return plan.units_;
    }

    static Array<uint32>& Members(ExchangeImportPlan& plan) noexcept {
        return plan.members_;
    }

    static Array<uint32>& UnitOf(ExchangeImportPlan& plan) noexcept {
        return plan.unitOf_;
    }

    static Array<uint32>& PathStart(ExchangeImportPlan& plan) noexcept {
        return plan.pathStart_;
    }

    static Array<uint32>& PathBytes(ExchangeImportPlan& plan) noexcept {
        return plan.pathBytes_;
    }

    static Array<uint8>& Renamed(ExchangeImportPlan& plan) noexcept {
        return plan.renamed_;
    }

    static Array<char>& PathText(ExchangeImportPlan& plan) noexcept {
        return plan.pathText_;
    }
};

namespace {

// 에셋마다의 순서 계산 상태다.
struct OrderNode {
    // 이 에셋 자체의 실패(읽기 CorruptBundle·UnrealPathTooLong·UnrealPathCollision)다.
    AssetDiagnosticCode failure = AssetDiagnosticCode::None;
    // 묶음·기존 에셋에 없는 Hard 대상이 있어 MissingDependency 오류를 냈으면 참이다.
    bool missingTarget = false;
    // 자기 자신을 Build로 참조하면 참이다.
    bool selfBuild = false;
    // Tarjan 상태다.
    uint32 index = ExchangeNoIndex;
    uint32 low = 0;
    bool onStack = false;
    // 간선 범위(edges 안)다.
    uint32 firstEdge = 0;
    uint32 edgeCount = 0;
};

struct OrderEdge {
    uint32 target;
    bool build;
};

struct OrderFrame {
    uint32 node;
    uint32 edge;
};

// 순서 계산 전체의 작업 상태다.
struct OrderState {
    explicit OrderState(Allocator& allocator) noexcept
        : nodes(allocator), edges(allocator), stack(allocator), frames(allocator),
          depths(allocator) {
    }

    const ExchangeBundle* bundle = nullptr;
    ExchangeDiagnostics* diagnostics = nullptr;
    Array<OrderNode> nodes;
    Array<OrderEdge> edges;
    Array<uint32> stack;
    Array<OrderFrame> frames;
    // 내보낸 순서(Tarjan 완료 순)의 단위 깊이다.
    Array<uint32> depths;
    uint32 counter = 0;
};

Result<void> AddOrderDiagnostic(OrderState& state, AssetDiagnosticCode code, ErrorCode error,
                                ExchangeIssue issue, uint32 asset, uint32 related,
                                StringView text) noexcept {
    ExchangeDiagnosticInput input;
    input.code = code;
    input.error = error;
    input.issue = issue;
    input.asset = asset;
    input.related = related;
    input.text = text;
    return state.diagnostics->TryAdd(input);
}

// Build 간선인지 본다(10.4절 14번 — MaterialSystem 4.4절의 부모·함수 호출·레이어·컬렉션).
bool IsBuildEdge(ExchangeClass source, ExchangeClass target) noexcept {
    return IsExchangeMaterialFamily(source) &&
           (IsExchangeMaterialFamily(target) ||
            target == ExchangeClass::MaterialParameterCollection);
}

// 묶음에 없는 Hard 대상 가운데 엔진 기본값으로 비워 둘 수 있는 것(텍스처·머티리얼 참조)이면
// 참이다(9.3절).
bool IsReplaceableTarget(ExchangeClass target) noexcept {
    return IsExchangeTextureClass(target) || target == ExchangeClass::Material ||
           target == ExchangeClass::MaterialInstanceConstant;
}

// 에셋마다 대응 경로를 만들고 길이 초과를 표시한다.
Result<void> MapPaths(OrderState& state, const ExchangeOrderOptions& options,
                      ExchangeImportPlan& out) noexcept {
    Allocator& allocator = ExchangePlanAccess::GetAllocator(out);
    Span<const ExchangeAsset> assets = state.bundle->Assets();
    Array<char>& text = ExchangePlanAccess::PathText(out);
    for (usize index = 0; index < assets.Size(); ++index) {
        const ExchangeAsset& asset = assets[index];
        if (asset.state == ExchangeAssetState::Unsupported) {
            continue;
        }

        if (asset.state == ExchangeAssetState::Failed) {
            state.nodes[index].failure = AssetDiagnosticCode::CorruptBundle;
        }

        StringBuilder<UnrealPathMaxBytes> mapped(allocator);
        ExchangePathOutcome outcome;
        auto made = MapUnrealObjectPath(asset.objectPath, options.root, mapped, outcome);
        if (!made) {
            if (outcome.failure != ExchangePathOutcome::Failure::TooLong) {
                return Result<void>::Failure(made.Error());
            }

            if (state.nodes[index].failure == AssetDiagnosticCode::None) {
                state.nodes[index].failure = AssetDiagnosticCode::UnrealPathTooLong;
            }

            if (auto added = AddOrderDiagnostic(
                    state, AssetDiagnosticCode::UnrealPathTooLong, ErrorCode::CapacityExceeded,
                    ExchangeIssue::PathTooLong, static_cast<uint32>(index), ExchangeNoIndex,
                    asset.objectPath);
                !added) {
                return added;
            }

            continue;
        }

        ExchangePlanAccess::PathStart(out)[index] = static_cast<uint32>(text.Size());
        ExchangePlanAccess::PathBytes(out)[index] = static_cast<uint32>(mapped.ByteCount());
        if (auto appended =
                text.TryAppend(Span<const char>(mapped.View().Data(), mapped.View().ByteCount()));
            !appended) {
            return appended;
        }

        if (outcome.renamed) {
            ExchangePlanAccess::Renamed(out)[index] = 1;
            if (auto added =
                    AddOrderDiagnostic(state, AssetDiagnosticCode::UnrealPathRenamed,
                                       ErrorCode::None, ExchangeIssue::Renamed,
                                       static_cast<uint32>(index), ExchangeNoIndex, mapped.View());
                !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

// 대응 경로의 대소문자 무시 키가 겹치는 에셋 모두를 UnrealPathCollision으로 표시한다.
Result<void> FindCollisions(OrderState& state, ExchangeImportPlan& out) noexcept {
    Allocator& allocator = ExchangePlanAccess::GetAllocator(out);
    const usize count = state.bundle->Assets().Size();
    Array<char> keys(allocator);
    Array<uint32> keyStart(allocator);
    Array<uint32> keyBytes(allocator);
    Array<uint32> order(allocator);
    if (auto resized = keyStart.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = keyBytes.TryResize(count); !resized) {
        return resized;
    }

    for (usize index = 0; index < count; ++index) {
        const StringView mapped = out.MappedPath(static_cast<uint32>(index));
        if (mapped.IsEmpty()) {
            continue;
        }

        StringBuilder<UnrealPathMaxBytes> key(allocator);
        if (auto made = TryMakeCaselessKey(mapped, key); !made) {
            return made;
        }

        keyStart[index] = static_cast<uint32>(keys.Size());
        keyBytes[index] = static_cast<uint32>(key.ByteCount());
        if (auto appended =
                keys.TryAppend(Span<const char>(key.View().Data(), key.View().ByteCount()));
            !appended) {
            return appended;
        }

        if (auto pushed = order.TryPushBack(static_cast<uint32>(index)); !pushed) {
            return pushed;
        }
    }

    auto keyOf = [&](uint32 index) noexcept {
        return StringView(keys.Data() + keyStart[index], keyBytes[index]);
    };
    algo::Sort(order.View(), [&](uint32 left, uint32 right) noexcept {
        const int compared = StringView::CompareBytes(keyOf(left), keyOf(right));
        return compared != 0 ? compared < 0 : left < right;
    });

    Span<const ExchangeAsset> assets = state.bundle->Assets();
    for (usize start = 0; start < order.Size();) {
        usize end = start + 1;
        while (end < order.Size() && keyOf(order[end]) == keyOf(order[start])) {
            ++end;
        }

        for (usize at = start; end - start > 1 && at < end; ++at) {
            const uint32 asset = order[at];
            const uint32 other = order[at == start ? start + 1 : start];
            if (state.nodes[asset].failure == AssetDiagnosticCode::None) {
                state.nodes[asset].failure = AssetDiagnosticCode::UnrealPathCollision;
            }

            if (auto added = AddOrderDiagnostic(
                    state, AssetDiagnosticCode::UnrealPathCollision, ErrorCode::AlreadyExists,
                    ExchangeIssue::PathCollision, asset, other, assets[asset].objectPath);
                !added) {
                return added;
            }
        }

        start = end;
    }

    return Result<void>::Success();
}

// 간선(묶음 안 Hard 대상)을 만들고 묶음에 없는 대상을 분류한다.
Result<void> BuildEdges(OrderState& state, const ExchangeOrderOptions& options) noexcept {
    Span<const ExchangeAsset> assets = state.bundle->Assets();
    for (usize index = 0; index < assets.Size(); ++index) {
        const ExchangeAsset& asset = assets[index];
        OrderNode& node = state.nodes[index];
        node.firstEdge = static_cast<uint32>(state.edges.Size());
        if (asset.state != ExchangeAssetState::Ready) {
            continue;
        }

        for (const ExchangeDependency& dependency : asset.dependencies) {
            if (dependency.kind != ExchangeDependencyKind::Hard ||
                dependency.targetClass == ExchangeClass::Unsupported) {
                continue;
            }

            if (dependency.target != ExchangeNoIndex) {
                const ExchangeAsset& target = assets[dependency.target];
                const bool build = IsBuildEdge(asset.exchangeClass, target.exchangeClass);
                if (dependency.target == index) {
                    node.selfBuild = node.selfBuild || build;
                    continue;
                }

                if (auto pushed = state.edges.TryPushBack(OrderEdge{dependency.target, build});
                    !pushed) {
                    return pushed;
                }

                ++node.edgeCount;
                continue;
            }

            const StringView resolved = state.bundle->ResolveRedirect(dependency.objectPath);
            if (options.existing != nullptr && options.existing->Contains(resolved)) {
                continue;
            }

            const bool replaceable = IsReplaceableTarget(dependency.targetClass);
            node.missingTarget = node.missingTarget || !replaceable;
            if (auto added =
                    AddOrderDiagnostic(state,
                                       replaceable ? AssetDiagnosticCode::MissingUnrealDependency
                                                   : AssetDiagnosticCode::MissingDependency,
                                       replaceable ? ErrorCode::None : ErrorCode::NotFound,
                                       ExchangeIssue::MissingTarget, static_cast<uint32>(index),
                                       ExchangeNoIndex, resolved);
                !added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

// 구성원 모두에 같은 진단을 낸다.
Result<void> ReportMembers(OrderState& state, Span<const uint32> members, AssetDiagnosticCode code,
                           ErrorCode error, ExchangeIssue issue, uint32 skip,
                           uint32 related) noexcept {
    for (const uint32 member : members) {
        if (member == skip) {
            continue;
        }

        if (auto added = AddOrderDiagnostic(state, code, error, issue, member, related,
                                            state.bundle->Assets()[member].objectPath);
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

// 끝난 SCC 하나를 단위로 만든다(깊이·Build 순환·실패 전파, 10.4절 14·15번).
Result<void> EmitUnit(OrderState& state, ExchangeImportPlan& out, usize stackStart) noexcept {
    Array<ExchangeImportUnit>& units = ExchangePlanAccess::Units(out);
    Array<uint32>& members = ExchangePlanAccess::Members(out);
    Array<uint32>& unitOf = ExchangePlanAccess::UnitOf(out);
    const uint32 unitIndex = static_cast<uint32>(units.Size());
    const usize firstMember = members.Size();
    for (usize at = stackStart; at < state.stack.Size(); ++at) {
        const uint32 member = state.stack[at];
        state.nodes[member].onStack = false;
        unitOf[member] = unitIndex;
        if (auto pushed = members.TryPushBack(member); !pushed) {
            return pushed;
        }
    }

    if (auto resized = state.stack.TryResize(stackStart); !resized) {
        return resized;
    }

    Span<uint32> unitMembers(members.Data() + firstMember, members.Size() - firstMember);
    algo::Sort(unitMembers);
    ExchangeImportUnit unit;
    unit.firstMember = static_cast<uint32>(firstMember);
    unit.memberCount = static_cast<uint32>(unitMembers.Size());
    bool buildCycle = false;
    uint32 failedMember = ExchangeNoIndex;
    uint32 blockedMember = ExchangeNoIndex;
    uint32 blockedBy = ExchangeNoIndex;
    for (const uint32 member : unitMembers) {
        const OrderNode& node = state.nodes[member];
        buildCycle = buildCycle || node.selfBuild;
        if (failedMember == ExchangeNoIndex && node.failure != AssetDiagnosticCode::None) {
            failedMember = member;
        }

        if (blockedMember == ExchangeNoIndex && node.missingTarget) {
            blockedMember = member;
        }

        for (uint32 edge = node.firstEdge; edge < node.firstEdge + node.edgeCount; ++edge) {
            const OrderEdge& item = state.edges[edge];
            const uint32 targetUnit = unitOf[item.target];
            if (targetUnit == unitIndex) {
                buildCycle = buildCycle || item.build;
                continue;
            }

            const uint32 below = state.depths[targetUnit] + 1;
            unit.depth = below > unit.depth ? below : unit.depth;
            if (units[targetUnit].state == ExchangeUnitState::Failed &&
                blockedBy == ExchangeNoIndex) {
                blockedMember = member;
                blockedBy = item.target;
            }
        }
    }

    if (failedMember != ExchangeNoIndex) {
        unit.state = ExchangeUnitState::Failed;
        unit.failure = state.nodes[failedMember].failure;
        unit.failureAsset = failedMember;
        if (auto reported = ReportMembers(
                state, unitMembers, AssetDiagnosticCode::MissingDependency, ErrorCode::NotFound,
                ExchangeIssue::FailedMember, failedMember, failedMember);
            !reported) {
            return reported;
        }
    } else if (buildCycle || unit.depth > UnrealImportMaxDepth) {
        unit.state = ExchangeUnitState::Failed;
        unit.failure = AssetDiagnosticCode::UnsupportedCycle;
        unit.failureAsset = unitMembers[0];
        if (auto reported = ReportMembers(
                state, unitMembers, AssetDiagnosticCode::UnsupportedCycle, ErrorCode::CycleDetected,
                buildCycle ? ExchangeIssue::BuildCycle : ExchangeIssue::DepthLimit, ExchangeNoIndex,
                ExchangeNoIndex);
            !reported) {
            return reported;
        }
    } else if (blockedMember != ExchangeNoIndex) {
        unit.state = ExchangeUnitState::Failed;
        unit.failure = AssetDiagnosticCode::MissingDependency;
        unit.failureAsset = blockedBy;
        if (blockedBy != ExchangeNoIndex) {
            if (auto added = AddOrderDiagnostic(
                    state, AssetDiagnosticCode::MissingDependency, ErrorCode::NotFound,
                    ExchangeIssue::FailedDependency, blockedMember, blockedBy,
                    state.bundle->Assets()[blockedBy].objectPath);
                !added) {
                return added;
            }
        }

        if (auto reported = ReportMembers(
                state, unitMembers, AssetDiagnosticCode::MissingDependency, ErrorCode::NotFound,
                ExchangeIssue::FailedMember, blockedMember, blockedMember);
            !reported) {
            return reported;
        }
    }

    if (auto pushed = state.depths.TryPushBack(unit.depth); !pushed) {
        return pushed;
    }

    return units.TryPushBack(unit);
}

// root에서 시작하는 재귀 없는 Tarjan이다.
Result<void> VisitFrom(OrderState& state, ExchangeImportPlan& out, uint32 root) noexcept {
    auto enter = [&](uint32 node) noexcept -> Result<void> {
        state.nodes[node].index = state.counter;
        state.nodes[node].low = state.counter;
        ++state.counter;
        state.nodes[node].onStack = true;
        if (auto pushed = state.stack.TryPushBack(node); !pushed) {
            return pushed;
        }

        return state.frames.TryPushBack(OrderFrame{node, state.nodes[node].firstEdge});
    };

    if (auto entered = enter(root); !entered) {
        return entered;
    }

    while (!state.frames.IsEmpty()) {
        const usize top = state.frames.Size() - 1;
        const uint32 node = state.frames[top].node;
        const OrderNode& current = state.nodes[node];
        if (state.frames[top].edge < current.firstEdge + current.edgeCount) {
            const uint32 target = state.edges[state.frames[top].edge].target;
            ++state.frames[top].edge;
            if (state.nodes[target].index == ExchangeNoIndex) {
                if (auto entered = enter(target); !entered) {
                    return entered;
                }
            } else if (state.nodes[target].onStack) {
                const uint32 reached = state.nodes[target].index;
                state.nodes[node].low = reached < current.low ? reached : current.low;
            }

            continue;
        }

        if (auto popped = state.frames.TryResize(top); !popped) {
            return popped;
        }

        if (!state.frames.IsEmpty()) {
            OrderNode& parent = state.nodes[state.frames[state.frames.Size() - 1].node];
            parent.low = current.low < parent.low ? current.low : parent.low;
        }

        if (current.low != current.index) {
            continue;
        }

        usize stackStart = state.stack.Size();
        while (state.stack[stackStart - 1] != node) {
            --stackStart;
        }

        if (auto emitted = EmitUnit(state, out, stackStart - 1); !emitted) {
            return emitted;
        }
    }

    return Result<void>::Success();
}

// 단위를 (깊이, 가장 작은 구성원) 순으로 정렬하고 unitOf를 다시 맞춘다.
void SortUnits(ExchangeImportPlan& out) noexcept {
    Array<ExchangeImportUnit>& units = ExchangePlanAccess::Units(out);
    Array<uint32>& members = ExchangePlanAccess::Members(out);
    algo::Sort(units.View(),
               [&](const ExchangeImportUnit& left, const ExchangeImportUnit& right) noexcept {
                   if (left.depth != right.depth) {
                       return left.depth < right.depth;
                   }

                   return members[left.firstMember] < members[right.firstMember];
               });
    Array<uint32>& unitOf = ExchangePlanAccess::UnitOf(out);
    for (usize index = 0; index < units.Size(); ++index) {
        for (const uint32 member : out.Members(units[index])) {
            unitOf[member] = static_cast<uint32>(index);
        }
    }
}

Result<void> PlanInto(const ExchangeBundle& bundle, const ExchangeOrderOptions& options,
                      ExchangeImportPlan& out, ExchangeDiagnostics& diagnostics) noexcept {
    Allocator& allocator = ExchangePlanAccess::GetAllocator(out);
    const usize count = bundle.Assets().Size();
    OrderState state(allocator);
    state.bundle = &bundle;
    state.diagnostics = &diagnostics;
    if (auto resized = state.nodes.TryResize(count); !resized) {
        return resized;
    }

    Array<uint32>* const indexArrays[] = {&ExchangePlanAccess::UnitOf(out),
                                          &ExchangePlanAccess::PathStart(out),
                                          &ExchangePlanAccess::PathBytes(out)};
    for (Array<uint32>* array : indexArrays) {
        if (auto resized = array->TryResize(count); !resized) {
            return resized;
        }
    }

    for (uint32& unit : ExchangePlanAccess::UnitOf(out)) {
        unit = ExchangeNoIndex;
    }

    if (auto resized = ExchangePlanAccess::Renamed(out).TryResize(count); !resized) {
        return resized;
    }

    if (auto mapped = MapPaths(state, options, out); !mapped) {
        return mapped;
    }

    if (auto found = FindCollisions(state, out); !found) {
        return found;
    }

    if (auto built = BuildEdges(state, options); !built) {
        return built;
    }

    for (usize index = 0; index < count; ++index) {
        if (bundle.Assets()[index].state == ExchangeAssetState::Unsupported ||
            state.nodes[index].index != ExchangeNoIndex) {
            continue;
        }

        if (auto visited = VisitFrom(state, out, static_cast<uint32>(index)); !visited) {
            return visited;
        }
    }

    SortUnits(out);
    return Result<void>::Success();
}

} // namespace

ExchangeImportPlan::ExchangeImportPlan(Allocator& allocator) noexcept
    : allocator_(&allocator), units_(allocator), members_(allocator), unitOf_(allocator),
      pathStart_(allocator), pathBytes_(allocator), renamed_(allocator), pathText_(allocator) {
}

Span<const uint32> ExchangeImportPlan::Members(const ExchangeImportUnit& unit) const noexcept {
    return Span<const uint32>(members_.Data() + unit.firstMember, unit.memberCount);
}

uint32 ExchangeImportPlan::UnitOf(uint32 asset) const noexcept {
    return asset < unitOf_.Size() ? unitOf_[asset] : ExchangeNoIndex;
}

StringView ExchangeImportPlan::MappedPath(uint32 asset) const noexcept {
    if (asset >= pathBytes_.Size()) {
        return StringView();
    }

    return StringView(pathText_.Data() + pathStart_[asset], pathBytes_[asset]);
}

bool ExchangeImportPlan::Renamed(uint32 asset) const noexcept {
    return asset < renamed_.Size() && renamed_[asset] != 0;
}

void ExchangeImportPlan::Clear() noexcept {
    units_.Clear();
    members_.Clear();
    unitOf_.Clear();
    pathStart_.Clear();
    pathBytes_.Clear();
    renamed_.Clear();
    pathText_.Clear();
}

Result<void> PlanExchangeImport(const ExchangeBundle& bundle, const ExchangeOrderOptions& options,
                                ExchangeImportPlan& out,
                                ExchangeDiagnostics& diagnostics) noexcept {
    out.Clear();
    if (auto valid = ValidateUnrealImportRoot(options.root, ExchangePlanAccess::GetAllocator(out));
        !valid) {
        if (valid.Error() != ErrorCode::InvalidArgument) {
            return valid;
        }

        ExchangeDiagnosticInput input;
        input.code = AssetDiagnosticCode::InvalidPath;
        input.error = ErrorCode::InvalidArgument;
        input.issue = ExchangeIssue::InvalidRoot;
        input.text = options.root;
        if (auto added = diagnostics.TryAdd(input); !added) {
            return added;
        }

        return valid;
    }

    auto planned = PlanInto(bundle, options, out, diagnostics);
    if (!planned) {
        out.Clear();
    }

    return planned;
}

} // namespace rocks
