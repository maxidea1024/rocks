#include <UiCore/HitGrid.h>

namespace rocks::ui {

namespace {

// 격자 칸 키는 위 32 bit가 x, 아래 32 bit가 y다.
constexpr uint32 GridKeyShift = 32;

constexpr float CellSize = 128;
// 전체 화면 배경 하나가 수천 개 셀을 중복 점유하지 않도록 넓은 요소는 별도로 보관한다.
constexpr int64 MaxCellsPerEntry = 64;

bool Cell(Vec2 point, int32& x, int32& y) noexcept {
    return math::TryFloorToInt32(static_cast<double>(point.x) / CellSize, x) &&
           math::TryFloorToInt32(static_cast<double>(point.y) / CellSize, y);
}

uint64 CellKey(int32 x, int32 y) noexcept {
    return static_cast<uint64>(static_cast<uint32>(x)) << GridKeyShift | static_cast<uint32>(y);
}

} // namespace

HitGrid::HitGrid(Allocator& allocator) noexcept
    : entries_(allocator), cells_(allocator), links_(allocator), broad_(allocator),
      candidates_(allocator) {
}

void HitGrid::Reset() noexcept {
    root_ = {};
    entries_.Clear();
    cells_.Clear();
    links_.Clear();
    broad_.Clear();
    candidates_.Clear();
}

bool HitGrid::HasSnapshot() const noexcept {
    return static_cast<bool>(root_.Lock());
}

Result<void> HitGrid::Update(const SharedPtr<Element>& root) noexcept {
    if (!root) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (root_.Lock().Get() == root.Get() && plan_ == root->planRevision_) {
        return Result<void>::Success();
    }

    HitGrid next(entries_.GetAllocator());
    auto result = next.Record(root, IndexNone, nullptr);
    if (!result) {
        return result;
    }

    next.root_ = WeakPtr<Element>(root);
    next.plan_ = root->planRevision_;
    if (auto extent = root->GetGeometry().WindowBounds()) {
        next.extent_ = extent.Value();
    }

    *this = Move(next);
    return Result<void>::Success();
}

Result<void> HitGrid::Record(const SharedPtr<Element>& element, usize parentClip,
                             const Box2<float>* clip) noexcept {
    // 비활성 요소도 등록한다 — 경로 자름이 Find에서 다룬다(20.5절).
    const auto visibility = element->GetVisibility();
    if (visibility == Visibility::HitTestInvisible || visibility == Visibility::Hidden ||
        visibility == Visibility::Collapsed) {
        return Result<void>::Success();
    }

    const auto& geometry = element->GetGeometry();
    auto bounds = geometry.WindowBounds();
    if (!bounds) {
        return Result<void>::Failure(bounds.Error());
    }

    const bool intersects = !clip || clip->Overlaps(bounds.Value());
    if (element->ClipsToBounds() && (!intersects || geometry.size.x <= 0 || geometry.size.y <= 0)) {
        return Result<void>::Success();
    }

    Box2<float> area = bounds.Value();
    if (clip && intersects) {
        area = area.TryIntersection(*clip).Value();
    }

    if (intersects && (visibility == Visibility::Visible || element->ClipsToBounds())) {
        const usize index = entries_.Size();
        auto added = entries_.TryPushBack({WeakPtr<Element>(element), geometry, parentClip});
        if (!added) {
            return added;
        }

        if (visibility == Visibility::Visible && geometry.size.x > 0 && geometry.size.y > 0) {
            auto indexed = Index(index, area);
            if (indexed) {
                indexed = candidates_.TryPushBack({index, area});
            }

            if (!indexed) {
                return indexed;
            }
        }

        if (element->ClipsToBounds()) {
            parentClip = index;
            clip = &area;
        }
    }

    // 그리기 순서로 걷는다(Docs/Design/UiToolkit 20.4절 — layer가 그리기 순서를 따라 줄지 않아
    // DrawBatch의 (layer, 순번) 순서와 같다).
    for (const auto& child : element->HitTestChildren()) {
        auto added = Record(child, parentClip, clip);
        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> HitGrid::Index(usize entry, const Box2<float>& bounds) noexcept {
    int32 left, top, right, bottom;
    if (!Cell(bounds.min, left, top) || !Cell(bounds.max, right, bottom)) {
        return broad_.TryPushBack(entry);
    }

    const int64 width = static_cast<int64>(right) - left + 1;
    const int64 height = static_cast<int64>(bottom) - top + 1;
    if (width > MaxCellsPerEntry || height > MaxCellsPerEntry / width) {
        return broad_.TryPushBack(entry);
    }

    for (int64 y = top; y <= bottom; ++y) {
        for (int64 x = left; x <= right; ++x) {
            const auto key = CellKey(static_cast<int32>(x), static_cast<int32>(y));
            const auto* previous = cells_.Find(key);
            const usize node = links_.Size();
            auto added = links_.TryPushBack({entry, previous ? *previous : IndexNone});
            if (!added) {
                return added;
            }

            auto head = cells_.InsertOrAssign(key, node);
            if (!head) {
                return Result<void>::Failure(head.Error());
            }
        }
    }

    return Result<void>::Success();
}

bool HitGrid::Accepts(usize index, Vec2 point, const Element& scope,
                      SharedPtr<Element>& hit) const noexcept {
    const auto& entry = entries_[index];
    if (!entry.geometry.Contains(point)) {
        return false;
    }

    for (usize clip = entry.parentClip; clip != IndexNone; clip = entries_[clip].parentClip) {
        if (!entries_[clip].geometry.Contains(point)) {
            return false;
        }
    }

    auto target = entry.element.Lock();
    if (!target || target->GetVisibility() != Visibility::Visible ||
        !target->IsInputPathActive(scope, true, false)) {
        return false;
    }

    hit = Move(target);
    return true;
}

namespace {

// scope에서 target으로 내려가는 경로를 첫 비활성 요소 앞에서 자른다(UE GetBubblePath 235–246행).
SharedPtr<Element> CutAtDisabled(SharedPtr<Element> target, const Element& scope,
                                 const SharedPtr<Element>& owner) noexcept {
    const Element* highest = nullptr;
    for (const Element* current = target.Get(); current; current = current->Parent()) {
        if (!current->IsEnabled()) {
            highest = current;
        }

        if (current == &scope) {
            break;
        }
    }

    if (!highest) {
        return target;
    }

    if (highest == &scope) {
        return {};
    }

    const Element* keep = highest->Parent();
    if (keep == owner.Get()) {
        return owner;
    }

    const Element* grand = keep ? keep->Parent() : nullptr;
    return grand ? grand->FindChild(keep) : SharedPtr<Element>{};
}

} // namespace

usize HitGrid::CandidateCount() const noexcept {
    return candidates_.Size();
}

SharedPtr<Element> HitGrid::CandidateFromTop(usize fromTop, Box2<float>& area) const noexcept {
    const auto owner = root_.Lock();
    if (!owner || fromTop >= candidates_.Size()) {
        return {};
    }

    const Candidate& candidate = candidates_[candidates_.Size() - 1 - fromTop];
    // 영역 지도는 구조 판정이라 활성을 보지 않는다(20.5절 — 비활성 최대화 버튼도 버튼 영역).
    auto target = entries_[candidate.entry].element.Lock();
    if (!target || target->GetVisibility() != Visibility::Visible ||
        !target->IsInputPathActive(*owner, true, false)) {
        return {};
    }

    area = candidate.area;
    return target;
}

SharedPtr<Element> HitGrid::Find(Vec2 point, const Element& scope,
                                 bool ignoreEnabled) const noexcept {
    const auto owner = root_.Lock();
    // scope 위의 비활성 조상도 경로를 자른다(UE는 창 루트부터 자른다 — 20.5절).
    if (!point.IsFinite() || !owner || !scope.IsInputPathActive(*owner, true, !ignoreEnabled)) {
        return {};
    }

    int32 x, y;
    const auto* found = Cell(point, x, y) ? cells_.Find(CellKey(x, y)) : nullptr;
    usize node = found ? *found : IndexNone;
    usize broad = broad_.Size();
    while (node != IndexNone || broad) {
        usize entry;
        if (node == IndexNone || (broad && broad_[broad - 1] > links_[node].entry)) {
            entry = broad_[--broad];
        } else {
            entry = links_[node].entry;
            node = links_[node].previous;
        }

        SharedPtr<Element> hit;
        if (Accepts(entry, point, scope, hit)) {
            // 맨 위 후보에서 멈춘다 — 자른 결과(부모)를 버리고 뒤 후보로 가지 않는다.
            return ignoreEnabled ? hit : CutAtDisabled(Move(hit), scope, owner);
        }
    }

    return {};
}

namespace {

// 방향 탐색의 비교 허용오차와 수직 축 줄임(UE FindNextFocusableWidgetDefault 598–640행)이다.
constexpr float FocusCompareTolerance = 0.1f;
constexpr float FocusSweepInset = 0.5f;
// 조상이 비활성인 후보를 빼고 다시 찾는 횟수의 상한이다(트리 깊이를 넘는 반복을 막는다).
constexpr usize MaxFocusSearchRestarts = 64;

struct FocusAxis {
    // 0이면 가로(좌·우), 1이면 세로(상·하)다.
    int32 axis;
    int32 increment;
    bool forward;

    float Source(const Box2<float>& rect) const noexcept {
        if (axis == 0) {
            return forward ? rect.max.x : rect.min.x;
        }

        return forward ? rect.max.y : rect.min.y;
    }

    float Dest(const Box2<float>& rect) const noexcept {
        if (axis == 0) {
            return forward ? rect.min.x : rect.max.x;
        }

        return forward ? rect.min.y : rect.max.y;
    }

    // a가 b보다 진행 방향 쪽(또는 같은)인지다(UE CompareFunc).
    bool Compare(float a, float b) const noexcept {
        return forward ? a + FocusCompareTolerance > b : a - FocusCompareTolerance < b;
    }
};

bool ParentsEnabled(const Element& element) noexcept {
    for (const Element* parent = element.Parent(); parent; parent = parent->Parent()) {
        if (!parent->IsEnabled()) {
            return false;
        }
    }

    return true;
}

int32 ClampCell(float value, int32 count) noexcept {
    int32 cell = 0;
    if (!math::TryFloorToInt32(static_cast<double>(value) / CellSize, cell)) {
        cell = value < 0 ? 0 : count - 1;
    }

    return cell < 0 ? 0 : (cell >= count ? count - 1 : cell);
}

} // namespace

FocusSearchResult HitGrid::FindNextFocusable(const Element& start, FocusDirection direction,
                                             NavigationRule rule, const Element& boundary,
                                             const FocusCandidateFilter& filter) const noexcept {
    if (!root_.Lock() || direction == FocusDirection::Next ||
        direction == FocusDirection::Previous) {
        return {};
    }

    auto startBounds = start.GetGeometry().WindowBounds();
    auto ruleBounds = boundary.GetGeometry().WindowBounds();
    if (!startBounds || !ruleBounds) {
        return {};
    }

    const Box2<float> widget = startBounds.Value();
    const Box2<float> ruleRect = ruleBounds.Value();
    const bool horizontal = direction == FocusDirection::Left || direction == FocusDirection::Right;
    const bool forward = direction == FocusDirection::Right || direction == FocusDirection::Down;
    const FocusAxis axis{horizontal ? 0 : 1, forward ? 1 : -1, forward};
    // 진행 축은 경계 끝까지 넓히고 수직 축은 0.5씩 줄인다(UE 598–640행).
    Box2<float> swept = widget;
    if (horizontal) {
        swept.min.x = ruleRect.min.x;
        swept.max.x = ruleRect.max.x;
        swept.min.y += FocusSweepInset;
        swept.max.y -= FocusSweepInset;
    } else {
        swept.min.y = ruleRect.min.y;
        swept.max.y = ruleRect.max.y;
        swept.min.x += FocusSweepInset;
        swept.max.x -= FocusSweepInset;
    }

    // 셀 수는 창 영역으로 정한다(UE NumCells — 창 크기 / 128).
    int32 numCells[2] = {1, 1};
    for (int32 i = 0; i < 2; ++i) {
        const float extent = i == 0 ? extent_.max.x : extent_.max.y;
        int32 count = 1;
        if (math::TryFloorToInt32(static_cast<double>(extent) / CellSize, count)) {
            numCells[i] = count + 1;
        }
    }

    const Vec2 center = widget.Center();
    const int32 strideAxis = horizontal ? 1 : 0;
    const int32 strideMin = ClampCell(horizontal ? swept.min.y : swept.min.x, numCells[strideAxis]);
    const int32 strideMax = ClampCell(horizontal ? swept.max.y : swept.max.x, numCells[strideAxis]);
    Array<const Element*> disabled(entries_.GetAllocator());
    for (usize restart = 0; restart <= MaxFocusSearchRestarts; ++restart) {
        int32 current[2] = {ClampCell(center.x, numCells[0]), ClampCell(center.y, numCells[1])};
        const int32 startingIndex = current[axis.axis];
        float sourceSide = axis.Source(widget);
        bool wrapped = false;
        bool retry = false;
        while (current[axis.axis] >= 0 && current[axis.axis] < numCells[axis.axis]) {
            const int32 processed = current[axis.axis];
            current[axis.axis] += axis.increment;
            SharedPtr<Element> best;
            Box2<float> bestRect{};
            const auto consider = [&](usize entryIndex) noexcept {
                auto candidate = entries_[entryIndex].element.Lock();
                if (!candidate || candidate.Get() == &start) {
                    return;
                }

                auto bounds = candidate->GetGeometry().WindowBounds();
                if (!bounds) {
                    return;
                }

                const Box2<float> rect = bounds.Value();
                if (!(axis.Compare(axis.Dest(rect), sourceSide) && swept.Overlaps(rect))) {
                    return;
                }

                if (best) {
                    const float bestDest = axis.Dest(bestRect);
                    const float testDest = axis.Dest(rect);
                    // 진행 축으로 더 멀면 지금 것을 지킨다.
                    if (!axis.Compare(bestDest, testDest)) {
                        return;
                    }

                    // 진행 축으로 같으면 중심이 더 가까운 쪽이다.
                    if (axis.Compare(testDest, bestDest) &&
                        (center - rect.Center()).LengthSquared() >=
                            (center - bestRect.Center()).LengthSquared()) {
                        return;
                    }
                }

                // Escape가 아니면 경계의 자손이어야 한다(UE NotADescendant 454–460행).
                if (rule != NavigationRule::Escape &&
                    (candidate.Get() == &boundary || !candidate->IsWithin(boundary))) {
                    return;
                }

                if (!candidate->IsEnabled() || !candidate->SupportsKeyboardFocus()) {
                    return;
                }

                for (const Element* excluded : disabled) {
                    if (excluded == candidate.Get()) {
                        return;
                    }
                }

                if (filter.IsBound() && !filter.Execute(*candidate)) {
                    return;
                }

                bestRect = rect;
                best = Move(candidate);
            };

            for (int32 stride = strideMin; stride <= strideMax; ++stride) {
                const int32 x = horizontal ? processed : stride;
                const int32 y = horizontal ? stride : processed;
                const auto* head = cells_.Find(CellKey(x, y));
                for (usize node = head ? *head : IndexNone; node != IndexNone;
                     node = links_[node].previous) {
                    consider(links_[node].entry);
                }
            }

            // 셀에 넣지 않은 넓은 항목은 줄마다 본다(위에서부터).
            for (usize i = broad_.Size(); i > 0; --i) {
                consider(broad_[i - 1]);
            }

            if (best) {
                // 경계 밖이면 규칙을 따른다(UE 495–523행).
                if (axis.Compare(axis.Dest(bestRect), axis.Source(swept))) {
                    if (rule == NavigationRule::Stop) {
                        return {};
                    }

                    if (rule == NavigationRule::CustomBoundary) {
                        return {FocusSearchOutcome::BeyondBoundary, {}};
                    }

                    if (rule == NavigationRule::Wrap) {
                        // UE와 같이 반대쪽 변에서 다시 훑을 준비만 하고 이 후보를 돌려준다.
                        sourceSide = axis.Dest(swept);
                        wrapped = true;
                    }
                }

                // 조상이 비활성이면 빼고 처음부터 다시 찾는다(UE IsParentsEnabled 528–533행).
                if (!ParentsEnabled(*best)) {
                    if (!disabled.TryPushBack(best.Get())) {
                        return {};
                    }

                    retry = true;
                    break;
                }

                return {FocusSearchOutcome::Found, Move(best)};
            }

            // 한 바퀴 돌아 시작 줄로 왔으면 멈춘다.
            if (wrapped && startingIndex == processed) {
                break;
            }

            // 경계 끝이다(UE 543–566행).
            if (!(current[axis.axis] >= 0 && current[axis.axis] < numCells[axis.axis])) {
                if (rule == NavigationRule::Wrap) {
                    if (wrapped) {
                        break;
                    }

                    sourceSide = axis.Dest(swept);
                    Vec2 sample = center;
                    (axis.axis == 0 ? sample.x : sample.y) = sourceSide;
                    current[0] = ClampCell(sample.x, numCells[0]);
                    current[1] = ClampCell(sample.y, numCells[1]);
                    wrapped = true;
                } else if (rule == NavigationRule::CustomBoundary) {
                    return {FocusSearchOutcome::ReachedEdge, {}};
                }
            }
        }

        if (!retry) {
            return {};
        }
    }

    return {};
}

} // namespace rocks::ui
