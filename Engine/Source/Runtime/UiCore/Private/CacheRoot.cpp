#include <UiCore/CacheRoot.h>

namespace rocks::ui {

CacheRoot::CacheRoot(Allocator& allocator) noexcept
    : list_(allocator), scratch_(allocator), entries_(allocator), next_(allocator),
      volatileEntries_(allocator), changed_(allocator), visited_(allocator), ranges_(allocator),
      assembled_(allocator) {
}

void CacheRoot::Reset() noexcept {
    root_ = nullptr;
    verticesOnly_ = false;
    entries_.Clear();
    next_.Clear();
    list_.Clear();
    scratch_.Clear();
    volatileEntries_.Clear();
    changed_.Clear();
    visited_.Clear();
    assembled_.Clear();
    ++revision_;
}

void CacheRoot::SetTheme(const ThemeColors* theme, uint64 revision) noexcept {
    list_.SetTheme(theme, revision);
    scratch_.SetTheme(theme, revision);
    assembled_.SetTheme(theme, revision);
    Reset();
}

void CacheRoot::SetVectorImageResolver(VectorImageResolver* resolver) noexcept {
    list_.SetVectorImageResolver(resolver);
    scratch_.SetVectorImageResolver(resolver);
    assembled_.SetVectorImageResolver(resolver);
    Reset();
}

void CacheRoot::SetFrameTime(const FrameTime& time) noexcept {
    list_.SetFrameTime(time);
    scratch_.SetFrameTime(time);
    assembled_.SetFrameTime(time);
}

const PaintList& CacheRoot::List() const noexcept {
    return list_;
}

uint64 CacheRoot::Revision() const noexcept {
    return revision_;
}

const PaintCacheStats& CacheRoot::LastUpdate() const noexcept {
    return stats_;
}

int32 CacheRoot::HighestLayer() const noexcept {
    return highest_;
}

bool CacheRoot::CanUpdateVertices(uint64 previousRevision) const noexcept {
    return verticesOnly_ && previousRevision + 1 == revision_;
}

Span<const PaintRange> CacheRoot::ChangedRanges() const noexcept {
    return ranges_.View();
}

Result<void> CacheRoot::Update(const Element& root, int32 layer, LinearColor inheritedForeground,
                               bool inheritedDisabled) noexcept {
    stats_ = {};
    if (root_ != &root || plan_ != root.planRevision_ || layer_ != layer ||
        rootForeground_ != inheritedForeground || rootDisabled_ != inheritedDisabled) {
        rootForeground_ = inheritedForeground;
        rootDisabled_ = inheritedDisabled;
        return Rebuild(root, layer);
    }

    if (volatileEntries_.IsEmpty() && paint_ == root.subtreePaintRevision_) {
        return Result<void>::Success();
    }

    return Refresh(root, layer);
}

Result<void> CacheRoot::Record(const Element& element, int32 layer, bool overlay, bool everyFrame,
                               int32& highest) noexcept {
    Entry entry{};
    entry.element = &element;
    entry.revision = element.paintRevision_;
    entry.layer = layer;
    entry.overlay = overlay;
    entry.everyFrame = everyFrame;
    entry.subtreeRevision = element.subtreePaintRevision_;
    entry.opacity = scratch_.opacity_;
    entry.nextOpacity = entry.opacity;
    entry.foreground = scratch_.foreground_;
    entry.nextForeground = entry.foreground;
    entry.disabled = scratch_.disabled_;
    entry.clipped = !scratch_.clips_.IsEmpty();
    if (entry.clipped) {
        entry.clip = scratch_.clips_[scratch_.clips_.Size() - 1];
    }

    entry.first = scratch_.Boxes().Size();
    if (overlay) {
        ++stats_.overlayCalls;
    } else {
        ++stats_.paintCalls;
    }

    auto result =
        overlay ? element.PaintOverlayPass(scratch_, layer) : element.OnPaint(scratch_, layer);
    if (!result) {
        return result;
    }

    entry.count = scratch_.Boxes().Size() - entry.first;
    entry.highest = layer;
    for (const auto& box : scratch_.Boxes().Slice(entry.first, entry.count)) {
        entry.highest = entry.highest > box.layer ? entry.highest : box.layer;
    }

    highest = entry.highest;
    return next_.TryPushBack(entry);
}

Result<void> CacheRoot::Rebuild(const Element& root, int32 layer) noexcept {
    scratch_.Clear();
    scratch_.foreground_ = rootForeground_;
    scratch_.disabled_ = rootDisabled_;
    next_.Clear();
    const uint64 plan = root.planRevision_;
    const uint64 paint = root.subtreePaintRevision_;
    int32 highest = layer;
    auto recorded = root.PaintSubtree(scratch_, layer, highest, this, false);
    if (!recorded) {
        return recorded;
    }

    if (plan != root.planRevision_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    usize count = 0;
    for (const Entry& entry : next_) {
        count += entry.everyFrame ? 1 : 0;
    }

    auto reserved = volatileEntries_.TryEnsureCapacity(count);
    if (!reserved) {
        return reserved;
    }

    volatileEntries_.Clear();
    for (usize i = 0; i < next_.Size(); ++i) {
        if (next_[i].everyFrame) {
            (void)volatileEntries_.TryPushBack(i);
        }
    }

    usize volatileIndex = 0;
    for (usize i = 0; i < next_.Size(); ++i) {
        while (volatileIndex < volatileEntries_.Size() && volatileEntries_[volatileIndex] < i) {
            ++volatileIndex;
        }

        next_[i].subtreeVolatile = volatileIndex < volatileEntries_.Size() &&
                                   volatileEntries_[volatileIndex] < next_[i].subtreeEnd;
    }

    stats_.rebuilt = true;
    verticesOnly_ = false;
    root_ = &root;
    plan_ = plan;
    paint_ = paint;
    layer_ = layer;
    highest_ = highest;
    Publish();
    return Result<void>::Success();
}

Result<void> CacheRoot::Refresh(const Element& root, int32 layer) noexcept {
    const uint64 paint = root.subtreePaintRevision_;
    changed_.Clear();
    visited_.Clear();
    Span<const usize> dirty = volatileEntries_.View();
    if (paint != paint_) {
        for (usize i = 0; i < entries_.Size(); ++i) {
            Entry& entry = entries_[i];
            const float inherited =
                entry.parentBody == MaxSize ? 1.0f : entries_[entry.parentBody].nextOpacity;
            entry.nextOpacity = inherited * entry.element->GetRenderOpacity();
            const LinearColor inheritedForeground = entry.parentBody == MaxSize
                                                        ? rootForeground_
                                                        : entries_[entry.parentBody].nextForeground;
            entry.nextForeground = entry.element->ResolveForeground(
                scratch_.Theme(), inheritedForeground, entry.disabled);
            const bool inheritedChanged =
                entry.nextOpacity != entry.opacity || entry.nextForeground != entry.foreground;
            if (!entry.overlay) {
                if (entry.subtreeRevision == entry.element->subtreePaintRevision_ &&
                    !entry.subtreeVolatile && !inheritedChanged) {
                    i = entry.subtreeEnd - 1;
                    continue;
                }

                if (entry.subtreeRevision != entry.element->subtreePaintRevision_) {
                    auto visited = visited_.TryPushBack(i);
                    if (!visited) {
                        return visited;
                    }
                }
            }

            if (inheritedChanged || entry.everyFrame ||
                entry.revision != entry.element->paintRevision_) {
                auto queued = changed_.TryPushBack(i);
                if (!queued) {
                    return queued;
                }
            }
        }

        dirty = changed_.View();
    }

    if (dirty.IsEmpty()) {
        for (usize i : visited_) {
            entries_[i].subtreeRevision = entries_[i].element->subtreePaintRevision_;
        }

        paint_ = paint;
        return Result<void>::Success();
    }

    scratch_.Clear();
    next_.Clear();
    bool movedRanges = false;
    usize previousCount = 0;
    for (usize i : dirty) {
        const Entry& entry = entries_[i];
        if (entry.clipped) {
            auto pushed = scratch_.clips_.TryPushBack(entry.clip);
            if (!pushed) {
                return pushed;
            }
        }

        int32 highest = entry.layer;
        scratch_.opacity_ = paint != paint_ ? entry.nextOpacity : entry.opacity;
        scratch_.foreground_ = paint != paint_ ? entry.nextForeground : entry.foreground;
        scratch_.disabled_ = entry.disabled;
        auto painted =
            Record(*entry.element, entry.layer, entry.overlay, entry.everyFrame, highest);
        if (entry.clipped) {
            scratch_.PopClip();
        }

        if (!painted) {
            return painted;
        }

        if (plan_ != root.planRevision_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        // 형제와 전경의 시작 레이어가 바뀌면 트리 순서로 다시 계산한다.
        if (highest != entry.highest) {
            return Rebuild(root, layer);
        }

        movedRanges |= next_[next_.Size() - 1].count != entry.count;
        previousCount += entry.count;
    }

    bool different = movedRanges;
    if (movedRanges) {
        const usize unchanged = list_.Boxes().Size() - previousCount;
        if (scratch_.Boxes().Size() > MaxSize - unchanged) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        assembled_.Clear();
        auto reserve = assembled_.boxes_.TryEnsureCapacity(unchanged + scratch_.Boxes().Size());
        if (!reserve) {
            return reserve;
        }

        usize cursor = 0;
        for (usize i = 0; i < entries_.Size(); ++i) {
            Entry& entry = entries_[i];
            auto commands = list_.Boxes().Slice(entry.first, entry.count);
            if (cursor < dirty.Size() && dirty[cursor] == i) {
                const Entry& replacement = next_[cursor++];
                commands = scratch_.Boxes().Slice(replacement.first, replacement.count);
                entry.count = replacement.count;
                entry.opacity = replacement.opacity;
                entry.foreground = replacement.foreground;
                entry.revision = replacement.revision;
            }

            entry.first = assembled_.Boxes().Size();
            (void)assembled_.boxes_.TryAppend(commands);
        }

        auto previous = Move(list_);
        list_ = Move(assembled_);
        assembled_ = Move(previous);
        verticesOnly_ = false;
    } else {
        auto reserved = ranges_.TryEnsureCapacity(dirty.Size());
        if (!reserved) {
            return reserved;
        }

        // 같은 판을 뒤늦게 소비하는 호스트를 위해 실제 변경이 생긴 시점에만 이전 범위를 교체한다.
        bool compatible = true;
        // 개수가 같은 색·진행률·volatile 갱신은 바뀐 명령 범위만 덮어쓴다.
        for (usize i = 0; i < dirty.Size(); ++i) {
            Entry& entry = entries_[dirty[i]];
            const Entry& replacement = next_[i];
            bool changed = false;
            for (usize j = 0; j < entry.count; ++j) {
                auto& old = list_.boxes_[entry.first + j];
                const auto& updated = scratch_.boxes_[replacement.first + j];
                if (old == updated) {
                    continue;
                }

                compatible &= old.layer == updated.layer && old.resource == updated.resource &&
                              old.operation == updated.operation &&
                              old.cornerRadius == updated.cornerRadius &&
                              old.roundedBox == updated.roundedBox &&
                              old.roundedSize == updated.roundedSize &&
                              old.outlineWidth == updated.outlineWidth &&
                              old.outlineColor == updated.outlineColor &&
                              old.hasClip == updated.hasClip &&
                              old.emptyClip == updated.emptyClip &&
                              (!old.hasClip || old.clip == updated.clip);
                old = updated;
                changed = true;
            }

            if (changed) {
                if (!different) {
                    ranges_.Clear();
                }

                different = true;
                (void)ranges_.TryPushBack({entry.first, entry.count});
            }

            entry.opacity = replacement.opacity;
            entry.foreground = replacement.foreground;
            entry.revision = replacement.revision;
        }

        if (different) {
            verticesOnly_ = compatible;
        }
    }

    if (root.subtreePaintRevision_ == paint) {
        for (usize i : visited_) {
            entries_[i].subtreeRevision = entries_[i].element->subtreePaintRevision_;
        }
    }

    paint_ = paint;
    if (different) {
        ++revision_;
    }

    return Result<void>::Success();
}

void CacheRoot::Publish() noexcept {
    auto previous = Move(list_);
    list_ = Move(scratch_);
    scratch_ = Move(previous);
    auto previousEntries = Move(entries_);
    entries_ = Move(next_);
    next_ = Move(previousEntries);
    ++revision_;
}

} // namespace rocks::ui
