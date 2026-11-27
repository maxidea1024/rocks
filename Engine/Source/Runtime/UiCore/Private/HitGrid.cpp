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
    *this = Move(next);
    return Result<void>::Success();
}

Result<void> HitGrid::Record(const SharedPtr<Element>& element, usize parentClip,
                             const Box2<float>* clip) noexcept {
    const auto visibility = element->GetVisibility();
    if (!element->IsEnabled() || visibility == Visibility::HitTestInvisible ||
        visibility == Visibility::Hidden || visibility == Visibility::Collapsed) {
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
        !target->IsInputPathActive(scope, true)) {
        return false;
    }

    hit = Move(target);
    return true;
}

usize HitGrid::CandidateCount() const noexcept {
    return candidates_.Size();
}

SharedPtr<Element> HitGrid::CandidateFromTop(usize fromTop, Box2<float>& area) const noexcept {
    const auto owner = root_.Lock();
    if (!owner || fromTop >= candidates_.Size()) {
        return {};
    }

    const Candidate& candidate = candidates_[candidates_.Size() - 1 - fromTop];
    auto target = entries_[candidate.entry].element.Lock();
    if (!target || target->GetVisibility() != Visibility::Visible ||
        !target->IsInputPathActive(*owner, true)) {
        return {};
    }

    area = candidate.area;
    return target;
}

SharedPtr<Element> HitGrid::Find(Vec2 point, const Element& scope) const noexcept {
    const auto owner = root_.Lock();
    if (!point.IsFinite() || !owner || !scope.IsInputPathActive(*owner, true)) {
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
            return hit;
        }
    }

    return {};
}

} // namespace rocks::ui
