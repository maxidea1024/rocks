#include <UnrealExchange/ExchangeImportOrder.h>

#include <Core/Algorithms.h>
#include <Core/Graph/StrongComponents.h>
#include <Core/Text/Unicode.h>
#include <UnrealExchange/ExchangePaths.h>

// Unreal 교환 임포트 순서다(Docs/Design/UnrealExchange 9.1·9.3절, 10.4절 13–15번, 10.5절):
// --only 닫힘, 기존 에셋 조회, 경로 대응과 겹침, Hard·Build 그래프의 SCC(Core 공용 반복 Tarjan),
// 단위 깊이·실패 전파.

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

    static Array<uint8>& Selected(ExchangeImportPlan& plan) noexcept {
        return plan.selected_;
    }

    static Array<uint32>& ExistingStart(ExchangeImportPlan& plan) noexcept {
        return plan.existingStart_;
    }

    static Array<uint32>& ExistingBytes(ExchangeImportPlan& plan) noexcept {
        return plan.existingBytes_;
    }

    static Array<AssetId>& ExistingId(ExchangeImportPlan& plan) noexcept {
        return plan.existingId_;
    }

    static Array<ExchangeExistingState>& ExistingState(ExchangeImportPlan& plan) noexcept {
        return plan.existingState_;
    }

    static Array<char>& PathText(ExchangeImportPlan& plan) noexcept {
        return plan.pathText_;
    }

    static Array<uint32>& ExternalStart(ExchangeImportPlan& plan) noexcept {
        return plan.externalStart_;
    }

    static Array<AssetId>& ExternalId(ExchangeImportPlan& plan) noexcept {
        return plan.externalId_;
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
};

// 순서 계산 전체의 작업 상태다.
struct OrderState {
    explicit OrderState(Allocator& allocator) noexcept
        : nodes(allocator), edgeStart(allocator), edgeTargets(allocator), edgeBuild(allocator),
          depths(allocator), redirectsByTarget(allocator), formerPaths(allocator),
          foundByFormer(allocator) {
    }

    const ExchangeBundle* bundle = nullptr;
    ExchangeDiagnostics* diagnostics = nullptr;
    // 고른 에셋 표다(계획의 selected_ — 에셋마다 0·1).
    const uint8* selected = nullptr;
    Array<OrderNode> nodes;
    // 간선(묶음 안 Hard 대상) CSR 표다: 에셋 n의 간선은 [edgeStart[n], edgeStart[n + 1])이고
    // edgeStart는 에셋 수 + 1개다. edgeBuild는 간선마다 Build 관계인지(0·1)다.
    Array<uint32> edgeStart;
    Array<uint32> edgeTargets;
    Array<uint8> edgeBuild;
    // 내보낸 순서(SCC 알림 순 — 의존이 먼저)의 단위 깊이다.
    Array<uint32> depths;
    // redirect 번호를 (to, from) byte 순으로 정렬한 표다(옛 경로 찾기 — 10.5절 4번).
    Array<uint32> redirectsByTarget;
    // 조회 하나의 옛 경로 목록(재사용 버퍼)이다.
    Array<StringView> formerPaths;
    // 에셋마다 기존 에셋을 둘째 단계(옛 경로)로 찾았는지다.
    Array<uint8> foundByFormer;
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

bool IsSelected(const OrderState& state, usize asset) noexcept {
    return state.selected[asset] != 0;
}

// 순서 단위가 될 에셋인지다(고른 지원 에셋). 아닌 에셋은 간선이 없고 간선 대상도 아니다(선택은
// Hard 닫힘이고, 의존 className은 대상 클래스와 같다 — ManifestMismatch).
bool IsOrdered(const OrderState& state, usize asset) noexcept {
    return state.bundle->Assets()[asset].state != ExchangeAssetState::Unsupported &&
           IsSelected(state, asset);
}

// redirect 번호를 (to, from) 순으로 정렬한다.
Result<void> IndexRedirects(OrderState& state) noexcept {
    Span<const ExchangeRedirect> redirects = state.bundle->Redirects();
    if (auto resized = state.redirectsByTarget.TryResize(redirects.Size()); !resized) {
        return resized;
    }

    for (usize index = 0; index < redirects.Size(); ++index) {
        state.redirectsByTarget[index] = static_cast<uint32>(index);
    }

    algo::Sort(state.redirectsByTarget.View(), [&](uint32 left, uint32 right) noexcept {
        const int compared = StringView::CompareBytes(redirects[left].to, redirects[right].to);
        return compared != 0
                   ? compared < 0
                   : StringView::CompareBytes(redirects[left].from, redirects[right].from) < 0;
    });
    return Result<void>::Success();
}

// objectPath로 이어지는 redirect 옛 경로들로 기존 에셋을 찾는다(10.5절 4번).
Result<ExchangeExistingMatch> FindExisting(OrderState& state,
                                           const ExchangeExistingAssets& existing,
                                           StringView objectPath) noexcept {
    Span<const ExchangeRedirect> redirects = state.bundle->Redirects();
    Span<const uint32> byTarget = state.redirectsByTarget.View();
    usize at =
        algo::LowerBound(byTarget, objectPath, [&](uint32 redirect, StringView key) noexcept {
            return StringView::CompareBytes(redirects[redirect].to, key) < 0;
        });
    state.formerPaths.Clear();
    for (; at < byTarget.Size() && redirects[byTarget[at]].to == objectPath; ++at) {
        if (auto pushed = state.formerPaths.TryPushBack(redirects[byTarget[at]].from); !pushed) {
            return Result<ExchangeExistingMatch>::Failure(pushed.Error());
        }
    }

    return existing.Find(objectPath, state.formerPaths.View());
}

// 기존 에셋을 찾아 계획에 적는다(Ready 에셋만 — 읽기에 실패한 에셋은 어차피 실패한다, 10.5절 4번).
Result<void> LookupExisting(OrderState& state, const ExchangeOrderOptions& options,
                            ExchangeImportPlan& out) noexcept {
    Span<const ExchangeAsset> assets = state.bundle->Assets();
    if (auto resized = state.foundByFormer.TryResize(assets.Size()); !resized) {
        return resized;
    }

    if (options.existing == nullptr) {
        return Result<void>::Success();
    }

    Array<char>& text = ExchangePlanAccess::PathText(out);
    for (usize index = 0; index < assets.Size(); ++index) {
        if (assets[index].state != ExchangeAssetState::Ready) {
            continue;
        }

        auto found = FindExisting(state, *options.existing, assets[index].objectPath);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        const ExchangeExistingMatch match = found.Value();
        ExchangePlanAccess::ExistingState(out)[index] = match.state;
        if (match.state != ExchangeExistingState::Found) {
            continue;
        }

        state.foundByFormer[index] = match.former ? 1 : 0;
        ExchangePlanAccess::ExistingId(out)[index] = match.id;
        ExchangePlanAccess::ExistingStart(out)[index] = static_cast<uint32>(text.Size());
        ExchangePlanAccess::ExistingBytes(out)[index] = static_cast<uint32>(match.path.ByteCount());
        if (auto appended =
                text.TryAppend(Span<const char>(match.path.Data(), match.path.ByteCount()));
            !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

// 묶음 에셋 둘 이상이 같은 기존 에셋을 찾으면 첫 단계로 찾은 하나만 그 에셋에 다시 임포트하고
// 둘째 단계로 찾은 나머지는 새 에셋으로 본다(redirector를 지운 뒤 같은 옛 이름으로 새로 만든 UE
// 에셋 — 9.2절). 첫 단계로 찾은 것이 없거나 둘 이상이면 모두 모호하다(10.5절 5번). 그다음 모호한
// 에셋을 UnrealPathCollision(AmbiguousExisting)으로 표시한다.
Result<void> ResolveExistingConflicts(OrderState& state, ExchangeImportPlan& out) noexcept {
    Allocator& allocator = ExchangePlanAccess::GetAllocator(out);
    Span<const ExchangeAsset> assets = state.bundle->Assets();
    Array<ExchangeExistingState>& states = ExchangePlanAccess::ExistingState(out);
    const Array<AssetId>& ids = ExchangePlanAccess::ExistingId(out);
    Array<uint32> found(allocator);
    for (usize index = 0; index < assets.Size(); ++index) {
        if (states[index] != ExchangeExistingState::Found) {
            continue;
        }

        if (auto pushed = found.TryPushBack(static_cast<uint32>(index)); !pushed) {
            return pushed;
        }
    }

    auto idLess = [&](uint32 left, uint32 right) noexcept {
        const Guid& a = ids[left].value;
        const Guid& b = ids[right].value;
        if (a.high != b.high) {
            return a.high < b.high;
        }

        return a.low != b.low ? a.low < b.low : left < right;
    };
    algo::Sort(found.View(), idLess);
    for (usize start = 0; start < found.Size();) {
        usize end = start + 1;
        while (end < found.Size() && ids[found[end]] == ids[found[start]]) {
            ++end;
        }

        uint32 direct = 0;
        for (usize at = start; at < end; ++at) {
            direct += state.foundByFormer[found[at]] == 0 ? 1 : 0;
        }

        for (usize at = start; end - start > 1 && at < end; ++at) {
            const uint32 asset = found[at];
            if (direct == 1 && state.foundByFormer[asset] == 0) {
                continue;
            }

            states[asset] =
                direct == 1 ? ExchangeExistingState::None : ExchangeExistingState::Ambiguous;
            ExchangePlanAccess::ExistingBytes(out)[asset] = 0;
        }

        start = end;
    }

    for (usize index = 0; index < assets.Size(); ++index) {
        if (states[index] != ExchangeExistingState::Ambiguous) {
            continue;
        }

        state.nodes[index].failure = AssetDiagnosticCode::UnrealPathCollision;
        if (!IsSelected(state, index)) {
            continue;
        }

        if (auto added = AddOrderDiagnostic(
                state, AssetDiagnosticCode::UnrealPathCollision, ErrorCode::AlreadyExists,
                ExchangeIssue::AmbiguousExisting, static_cast<uint32>(index), ExchangeNoIndex,
                assets[index].objectPath);
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

// 기존 에셋에 다시 임포트하는 에셋이면 참이다(대응 경로를 쓰지 않는다 — 10.5절 5번).
bool UsesExisting(const ExchangeImportPlan& plan, usize asset) noexcept {
    return plan.Existing(static_cast<uint32>(asset)).state == ExchangeExistingState::Found;
}

// Build 간선인지 본다(10.4절 14번 — MaterialSystem 4.4절의 부모·함수 호출·레이어·컬렉션).
bool IsBuildEdge(ExchangeClass source, ExchangeClass target) noexcept {
    return IsExchangeMaterialFamily(source) &&
           (IsExchangeMaterialFamily(target) ||
            target == ExchangeClass::MaterialParameterCollection);
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

        // 기존 에셋에 다시 임포트하면 대응 경로는 보여 주기만 한다(실패·진단 없음).
        const bool report = IsSelected(state, index) && !UsesExisting(out, index);
        StringBuilder<UnrealPathMaxBytes> mapped(allocator);
        ExchangePathOutcome outcome;
        auto made = MapUnrealObjectPath(asset.objectPath, options.root, mapped, outcome);
        if (!made) {
            if (outcome.failure != ExchangePathOutcome::Failure::TooLong) {
                return Result<void>::Failure(made.Error());
            }

            if (UsesExisting(out, index)) {
                continue;
            }

            if (state.nodes[index].failure == AssetDiagnosticCode::None) {
                state.nodes[index].failure = AssetDiagnosticCode::UnrealPathTooLong;
            }

            if (!report) {
                continue;
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
            if (!report) {
                continue;
            }

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

// 새 에셋의 대응 경로 키(대소문자 무시)가 다른 새 에셋이나 기존 에셋에 다시 임포트하는 에셋의
// 경로와 겹치면 그 새 에셋들을 UnrealPathCollision으로 표시한다(기존 에셋 쪽은 실패하지 않는다 —
// 10.5절 5번).
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
        const StringView path = out.TargetPath(static_cast<uint32>(index));
        if (path.IsEmpty()) {
            continue;
        }

        StringBuilder<UnrealPathMaxBytes> key(allocator);
        if (auto made = TryMakeCaselessKey(path, key); !made) {
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

        // 무리 안의 첫 기존 에셋(있으면)이 새 에셋들의 관련 에셋이다.
        uint32 occupant = ExchangeNoIndex;
        for (usize at = start; at < end && occupant == ExchangeNoIndex; ++at) {
            occupant = UsesExisting(out, order[at]) ? order[at] : ExchangeNoIndex;
        }

        for (usize at = start; end - start > 1 && at < end; ++at) {
            const uint32 asset = order[at];
            if (UsesExisting(out, asset)) {
                continue;
            }

            const uint32 other =
                occupant != ExchangeNoIndex ? occupant : order[at == start ? start + 1 : start];
            if (state.nodes[asset].failure == AssetDiagnosticCode::None) {
                state.nodes[asset].failure = AssetDiagnosticCode::UnrealPathCollision;
            }

            if (!IsSelected(state, asset)) {
                continue;
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

// 아직 실패하지 않은 새 에셋의 대응 경로를 다른 Rocks 에셋이 쓰면
// UnrealPathCollision(PathTaken)이다 (10.5절 5번 — 덮어쓰지 않는다).
Result<void> CheckTakenPaths(OrderState& state, const ExchangeOrderOptions& options,
                             const ExchangeImportPlan& out) noexcept {
    if (options.existing == nullptr) {
        return Result<void>::Success();
    }

    Span<const ExchangeAsset> assets = state.bundle->Assets();
    for (usize index = 0; index < assets.Size(); ++index) {
        const StringView mapped = out.MappedPath(static_cast<uint32>(index));
        if (mapped.IsEmpty() || UsesExisting(out, index) ||
            state.nodes[index].failure != AssetDiagnosticCode::None) {
            continue;
        }

        auto taken = options.existing->PathTaken(mapped);
        if (!taken) {
            return Result<void>::Failure(taken.Error());
        }

        if (!taken.Value()) {
            continue;
        }

        state.nodes[index].failure = AssetDiagnosticCode::UnrealPathCollision;
        if (!IsSelected(state, index)) {
            continue;
        }

        if (auto added = AddOrderDiagnostic(state, AssetDiagnosticCode::UnrealPathCollision,
                                            ErrorCode::AlreadyExists, ExchangeIssue::PathTaken,
                                            static_cast<uint32>(index), ExchangeNoIndex, mapped);
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

// 간선(묶음 안 Hard 대상)을 만들고 묶음에 없는 대상을 분류한다. 묶음 밖 대상의 기존 에셋 id는
// 의존마다 계획에 남긴다(10.6절 4번 — ExternalTarget).
Result<void> BuildEdges(OrderState& state, const ExchangeOrderOptions& options,
                        ExchangeImportPlan& out) noexcept {
    Span<const ExchangeAsset> assets = state.bundle->Assets();
    if (auto resized = state.edgeStart.TryResize(assets.Size() + 1); !resized) {
        return resized;
    }

    Array<uint32>& externalStart = ExchangePlanAccess::ExternalStart(out);
    Array<AssetId>& externalId = ExchangePlanAccess::ExternalId(out);
    if (auto resized = externalStart.TryResize(assets.Size() + 1); !resized) {
        return resized;
    }

    usize dependencies = 0;
    for (usize index = 0; index < assets.Size(); ++index) {
        externalStart[index] = static_cast<uint32>(dependencies);
        dependencies += assets[index].dependencies.Size();
    }

    externalStart[assets.Size()] = static_cast<uint32>(dependencies);
    if (auto resized = externalId.TryResize(dependencies); !resized) {
        return resized;
    }

    for (usize index = 0; index < assets.Size(); ++index) {
        const ExchangeAsset& asset = assets[index];
        OrderNode& node = state.nodes[index];
        state.edgeStart[index] = static_cast<uint32>(state.edgeTargets.Size());
        if (asset.state != ExchangeAssetState::Ready || !IsSelected(state, index)) {
            continue;
        }

        for (usize at = 0; at < asset.dependencies.Size(); ++at) {
            const ExchangeDependency& dependency = asset.dependencies[at];
            if (dependency.kind != ExchangeDependencyKind::Hard ||
                dependency.targetClass == ExchangeClass::Unsupported) {
                continue;
            }

            if (dependency.target != ExchangeNoIndex) {
                // 선택은 Hard 닫힘이어야 한다(SelectExchangeClosure — 아니면 호출 오류).
                if (!IsSelected(state, dependency.target)) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                const ExchangeAsset& target = assets[dependency.target];
                const bool build = IsBuildEdge(asset.exchangeClass, target.exchangeClass);
                if (dependency.target == index) {
                    if (build) {
                        node.selfBuild = true;
                    }

                    continue;
                }

                if (auto pushed = state.edgeTargets.TryPushBack(dependency.target); !pushed) {
                    return pushed;
                }

                if (auto pushed = state.edgeBuild.TryPushBack(build ? 1 : 0); !pushed) {
                    return pushed;
                }

                continue;
            }

            const StringView resolved = state.bundle->ResolveRedirect(dependency.objectPath);
            ExchangeExistingState found = ExchangeExistingState::None;
            AssetId foundId{};
            if (options.existing != nullptr) {
                auto match = FindExisting(state, *options.existing, resolved);
                if (!match) {
                    return Result<void>::Failure(match.Error());
                }

                found = match.Value().state;
                foundId = match.Value().id;
            }

            if (found == ExchangeExistingState::Found) {
                externalId[externalStart[index] + at] = foundId;
                continue;
            }

            if (found == ExchangeExistingState::Ambiguous) {
                // 어느 에셋에 이을지 고를 수 없으므로 대상 클래스와 무관하게 누락이다.
                node.missingTarget = true;
                if (auto added = AddOrderDiagnostic(
                        state, AssetDiagnosticCode::UnrealPathCollision, ErrorCode::AlreadyExists,
                        ExchangeIssue::AmbiguousExisting, static_cast<uint32>(index),
                        ExchangeNoIndex, resolved);
                    !added) {
                    return added;
                }

                continue;
            }

            const bool replaceable = IsExchangeReplaceableTarget(dependency.targetClass);
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

    state.edgeStart[assets.Size()] = static_cast<uint32>(state.edgeTargets.Size());
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

// SCC 하나를 단위로 만든다(깊이·Build 순환·실패 전파, 10.4절 14·15번). SCC는 의존이 먼저
// 오므로 단위 밖 간선 대상의 단위는 이미 있다.
Result<void> EmitUnit(OrderState& state, ExchangeImportPlan& out,
                      Span<const uint32> component) noexcept {
    Array<ExchangeImportUnit>& units = ExchangePlanAccess::Units(out);
    Array<uint32>& members = ExchangePlanAccess::Members(out);
    Array<uint32>& unitOf = ExchangePlanAccess::UnitOf(out);
    const uint32 unitIndex = static_cast<uint32>(units.Size());
    const usize firstMember = members.Size();
    for (const uint32 member : component) {
        unitOf[member] = unitIndex;
        if (auto pushed = members.TryPushBack(member); !pushed) {
            return pushed;
        }
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

        for (uint32 edge = state.edgeStart[member]; edge < state.edgeStart[member + 1]; ++edge) {
            const uint32 target = state.edgeTargets[edge];
            const uint32 targetUnit = unitOf[target];
            if (targetUnit == unitIndex) {
                buildCycle = buildCycle || state.edgeBuild[edge] != 0;
                continue;
            }

            const uint32 below = state.depths[targetUnit] + 1;
            unit.depth = below > unit.depth ? below : unit.depth;
            if (units[targetUnit].state == ExchangeUnitState::Failed &&
                blockedBy == ExchangeNoIndex) {
                blockedMember = member;
                blockedBy = target;
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
    } else if (unitMembers.Size() > 1) {
        // Hard만의 순환은 9.3절이 허용하지만 다중 에셋 원자 저장(UXR11) 전까지 실패다(10.5절 8번 —
        // 의존 실패가 있으면 그쪽이 원인이므로 먼저 본다).
        unit.state = ExchangeUnitState::Failed;
        unit.failure = AssetDiagnosticCode::UnsupportedCycle;
        unit.failureAsset = unitMembers[0];
        if (auto reported = ReportMembers(state, unitMembers, AssetDiagnosticCode::UnsupportedCycle,
                                          ErrorCode::CycleDetected, ExchangeIssue::HardCycle,
                                          ExchangeNoIndex, ExchangeNoIndex);
            !reported) {
            return reported;
        }
    }

    if (auto pushed = state.depths.TryPushBack(unit.depth); !pushed) {
        return pushed;
    }

    return units.TryPushBack(unit);
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

    Array<uint32>* const indexArrays[] = {
        &ExchangePlanAccess::UnitOf(out), &ExchangePlanAccess::PathStart(out),
        &ExchangePlanAccess::PathBytes(out), &ExchangePlanAccess::ExistingStart(out),
        &ExchangePlanAccess::ExistingBytes(out)};
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

    if (auto resized = ExchangePlanAccess::ExistingId(out).TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = ExchangePlanAccess::ExistingState(out).TryResize(count); !resized) {
        return resized;
    }

    Array<uint8>& selected = ExchangePlanAccess::Selected(out);
    if (auto resized = selected.TryResize(count); !resized) {
        return resized;
    }

    for (usize index = 0; index < count; ++index) {
        selected[index] = options.selection.IsEmpty() ? 1 : (options.selection[index] != 0 ? 1 : 0);
    }

    state.selected = selected.Data();
    if (auto indexed = IndexRedirects(state); !indexed) {
        return indexed;
    }

    if (auto looked = LookupExisting(state, options, out); !looked) {
        return looked;
    }

    if (auto resolved = ResolveExistingConflicts(state, out); !resolved) {
        return resolved;
    }

    if (auto mapped = MapPaths(state, options, out); !mapped) {
        return mapped;
    }

    if (auto found = FindCollisions(state, out); !found) {
        return found;
    }

    if (auto checked = CheckTakenPaths(state, options, out); !checked) {
        return checked;
    }

    if (auto built = BuildEdges(state, options, out); !built) {
        return built;
    }

    // 단위가 아닌 에셋은 간선이 없어 혼자 SCC가 되므로 건너뛴다(IsOrdered).
    auto visited = algo::ForEachStrongComponent(
        allocator, static_cast<uint32>(count), state.edgeStart.View(), state.edgeTargets.View(),
        [&](Span<const uint32> component, bool) noexcept -> Result<void> {
            if (component.Size() == 1 && !IsOrdered(state, component[0])) {
                return Result<void>::Success();
            }

            return EmitUnit(state, out, component);
        });
    if (!visited) {
        return visited;
    }

    SortUnits(out);
    return Result<void>::Success();
}

} // namespace

ExchangeImportPlan::ExchangeImportPlan(Allocator& allocator) noexcept
    : allocator_(&allocator), units_(allocator), members_(allocator), unitOf_(allocator),
      pathStart_(allocator), pathBytes_(allocator), existingStart_(allocator),
      existingBytes_(allocator), existingId_(allocator), existingState_(allocator),
      externalStart_(allocator), externalId_(allocator), renamed_(allocator), selected_(allocator),
      pathText_(allocator) {
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

bool ExchangeImportPlan::Selected(uint32 asset) const noexcept {
    return asset < selected_.Size() && selected_[asset] != 0;
}

ExchangeExistingMatch ExchangeImportPlan::Existing(uint32 asset) const noexcept {
    ExchangeExistingMatch match;
    if (asset >= existingState_.Size()) {
        return match;
    }

    match.state = existingState_[asset];
    if (match.state == ExchangeExistingState::Found) {
        match.id = existingId_[asset];
        match.path = StringView(pathText_.Data() + existingStart_[asset], existingBytes_[asset]);
    }

    return match;
}

StringView ExchangeImportPlan::TargetPath(uint32 asset) const noexcept {
    const ExchangeExistingMatch existing = Existing(asset);
    return existing.state == ExchangeExistingState::Found ? existing.path : MappedPath(asset);
}

AssetId ExchangeImportPlan::ExternalTarget(uint32 asset, uint32 dependency) const noexcept {
    if (externalStart_.IsEmpty() || asset >= externalStart_.Size() - 1) {
        return AssetId{};
    }

    const uint32 first = externalStart_[asset];
    if (dependency >= externalStart_[asset + 1] - first) {
        return AssetId{};
    }

    return externalId_[first + dependency];
}

void ExchangeImportPlan::Clear() noexcept {
    units_.Clear();
    members_.Clear();
    unitOf_.Clear();
    pathStart_.Clear();
    pathBytes_.Clear();
    existingStart_.Clear();
    existingBytes_.Clear();
    existingId_.Clear();
    existingState_.Clear();
    externalStart_.Clear();
    externalId_.Clear();
    renamed_.Clear();
    selected_.Clear();
    pathText_.Clear();
}

Result<void> SelectExchangeClosure(const ExchangeBundle& bundle, Span<const StringView> only,
                                   Array<uint8>& out, uint32* missing) noexcept {
    Span<const ExchangeAsset> assets = bundle.Assets();
    out.Clear();
    if (auto resized = out.TryResize(assets.Size()); !resized) {
        return resized;
    }

    for (uint8& selected : out) {
        selected = only.IsEmpty() ? 1 : 0;
    }

    if (only.IsEmpty()) {
        return Result<void>::Success();
    }

    // 고를 때 표시하므로 에셋마다 stack에 한 번만 들어간다(stack 크기 = 에셋 수).
    Array<uint32> pending(out.GetAllocator());
    if (auto resized = pending.TryResize(assets.Size()); !resized) {
        out.Clear();
        return resized;
    }

    usize top = 0;
    auto select = [&](uint32 asset) noexcept {
        if (out[asset] == 0) {
            out[asset] = 1;
            pending[top++] = asset;
        }
    };
    for (usize index = 0; index < only.Size(); ++index) {
        const uint32 asset = bundle.FindAsset(bundle.ResolveRedirect(only[index]));
        if (asset == ExchangeNoIndex) {
            if (missing != nullptr) {
                *missing = static_cast<uint32>(index);
            }

            out.Clear();
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        select(asset);
    }

    while (top != 0) {
        const uint32 asset = pending[--top];
        if (assets[asset].state != ExchangeAssetState::Ready) {
            continue;
        }

        for (const ExchangeDependency& dependency : assets[asset].dependencies) {
            if (dependency.kind == ExchangeDependencyKind::Hard &&
                dependency.targetClass != ExchangeClass::Unsupported &&
                dependency.target != ExchangeNoIndex) {
                select(dependency.target);
            }
        }
    }

    return Result<void>::Success();
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

    if (!options.selection.IsEmpty() && options.selection.Size() != bundle.Assets().Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto planned = PlanInto(bundle, options, out, diagnostics);
    if (!planned) {
        out.Clear();
    }

    return planned;
}

} // namespace rocks
