#include <UiCore/CacheRoot.h>
#include <UiCore/DrawBatch.h>

namespace rocks::ui {

namespace {

Result<void> AppendVertices(const PaintBox& box, Array<DrawVertex>& vertices) noexcept {
    const Vec2 size = box.geometry.size;
    if (!size.IsFinite() || size.x < 0 || size.y < 0 || !box.color.IsFinite() ||
        !box.uvStart.IsFinite() || !box.uvEnd.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const Vec2 corners[] = {{0, 0}, {size.x, 0}, {size.x, size.y}, {0, size.y}};
    Vec2 uvs[] = {
        box.uvStart, {box.uvEnd.x, box.uvStart.y}, box.uvEnd, {box.uvStart.x, box.uvEnd.y}};
    if ((box.operation == PaintOperation::BlurBackdrop ||
         box.operation == PaintOperation::BlurToBuffer)) {
        if (!math::IsFinite(box.blurRadius) || box.blurRadius < 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 블러 정점 셰이더는 화면 UV를 위치에서 계산하고 이 채널에서 샘플 간격을 읽는다.
        for (auto& uv : uvs) {
            uv = {box.blurRadius,
                  box.blurKernel < 0 ? box.blurRadius : static_cast<float>(box.blurKernel) * 0.5f};
        }
    }

    for (usize i = 0; i < 4; ++i) {
        const Vec2 position = box.geometry.render.Apply(corners[i]);
        if (!position.IsFinite()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const auto color = (box.operation == PaintOperation::BlurBackdrop ||
                            box.operation == PaintOperation::BlurToBuffer)
                               ? LinearColor{corners[i].x, corners[i].y, size.x, size.y}
                               : box.color;
        auto added = vertices.TryPushBack({position, uvs[i], color});
        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace

DrawBatch::DrawBatch(Allocator& allocator) noexcept
    : vertices_(allocator), indices_(allocator), commands_(allocator), scratchVertices_(allocator),
      scratchIndices_(allocator), scratchCommands_(allocator), scratchOrder_(allocator),
      firstVertex_(allocator), scratchFirstVertex_(allocator), changedVertices_(allocator),
      scratchRanges_(allocator) {
}

Result<void> DrawBatch::Build(const PaintList& list) noexcept {
    const Span<const PaintBox> boxes = list.Boxes();
    if (boxes.Size() > NumericLimits<uint32>::Max() / 6 || boxes.Size() > MaxSize / 6) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    Array<PaintOrder>& order = scratchOrder_;
    Array<DrawVertex>& vertices = scratchVertices_;
    Array<uint32>& indices = scratchIndices_;
    Array<DrawCommand>& commands = scratchCommands_;
    order.Clear();
    vertices.Clear();
    indices.Clear();
    commands.Clear();
    auto mapped = scratchFirstVertex_.TryResize(boxes.Size());
    if (!mapped) {
        return mapped;
    }

    auto orderReserved = order.TryReserve(boxes.Size());
    if (!orderReserved) {
        return orderReserved;
    }

    auto vertexReserved = vertices.TryReserve(boxes.Size() * 4);
    if (!vertexReserved) {
        return vertexReserved;
    }

    auto indexReserved = indices.TryReserve(boxes.Size() * 6);
    if (!indexReserved) {
        return indexReserved;
    }

    auto commandReserved = commands.TryReserve(boxes.Size());
    if (!commandReserved) {
        return commandReserved;
    }

    for (usize i = 0; i < boxes.Size(); ++i) {
        auto added = order.TryPushBack({i, boxes[i].layer});
        if (!added) {
            return added;
        }
    }

    algo::Sort(order.View(), [](const PaintOrder& a, const PaintOrder& b) noexcept {
        return a.layer == b.layer ? a.index < b.index : a.layer < b.layer;
    });

    for (usize i = 0; i < order.Size(); ++i) {
        const PaintBox& box = boxes[order[i].index];
        scratchFirstVertex_[order[i].index] = NumericLimits<uint32>::Max();
        if (box.hasClip && box.emptyClip) {
            continue;
        }

        if (box.hasClip && !box.clip.IsValid()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (box.hasClip && (box.clip.min.x == box.clip.max.x || box.clip.min.y == box.clip.max.y)) {
            continue;
        }

        const uint32 first = static_cast<uint32>(vertices.Size());
        auto appended = AppendVertices(box, vertices);
        if (!appended) {
            return appended;
        }

        scratchFirstVertex_[order[i].index] = first;

        const uint32 quadIndices[] = {first, first + 1, first + 2, first, first + 2, first + 3};
        for (usize index = 0; index < 6; ++index) {
            auto added = indices.TryPushBack(quadIndices[index]);
            if (!added) {
                return added;
            }
        }

        const uint32 firstIndex = static_cast<uint32>(indices.Size() - 6);
        const auto sameRoundedBox = [&](const DrawCommand& last) noexcept {
            return last.roundedBox == box.roundedBox &&
                   (!box.roundedBox ||
                    (last.roundedSize == box.roundedSize && last.cornerRadius == box.cornerRadius &&
                     last.outlineWidth == box.outlineWidth &&
                     last.outlineColor == box.outlineColor));
        };
        if (box.operation == PaintOperation::Draw && !commands.IsEmpty() &&
            commands[commands.Size() - 1].operation == PaintOperation::Draw &&
            commands[commands.Size() - 1].hasClip == box.hasClip &&
            commands[commands.Size() - 1].resource == box.resource &&
            (!box.hasClip || commands[commands.Size() - 1].clip == box.clip) &&
            sameRoundedBox(commands[commands.Size() - 1])) {
            commands[commands.Size() - 1].indexCount += 6;
        } else {
            auto added =
                commands.TryPushBack({firstIndex, 6, box.hasClip, box.clip, box.resource,
                                      box.operation, box.cornerRadius, box.roundedBox,
                                      box.roundedSize, box.outlineWidth, box.outlineColor});
            if (!added) {
                return added;
            }
        }
    }

    auto previousMap = Move(firstVertex_);
    firstVertex_ = Move(scratchFirstVertex_);
    scratchFirstVertex_ = Move(previousMap);
    cache_ = nullptr;
    Array<DrawVertex> oldVertices = Move(vertices_);
    Array<uint32> oldIndices = Move(indices_);
    Array<DrawCommand> oldCommands = Move(commands_);
    vertices_ = Move(scratchVertices_);
    indices_ = Move(scratchIndices_);
    commands_ = Move(scratchCommands_);
    scratchVertices_ = Move(oldVertices);
    scratchIndices_ = Move(oldIndices);
    scratchCommands_ = Move(oldCommands);
    ++batchRevision_;
    ++topologyRevision_;
    changedVertices_.Clear();
    return Result<void>::Success();
}

Result<void> DrawBatch::Update(const CacheRoot& cache) noexcept {
    if (cache_ == &cache && revision_ == cache.Revision()) {
        return Result<void>::Success();
    }

    if (cache_ != &cache || !cache.CanUpdateVertices(revision_)) {
        auto built = Build(cache.List());
        if (!built) {
            return built;
        }
    } else {
        // 모든 정점을 먼저 검증한다. 실패하면 이전 프레임 배치를 그대로 다시 제출할 수 있다.
        scratchVertices_.Clear();
        scratchRanges_.Clear();
        const auto boxes = cache.List().Boxes();
        for (const auto range : cache.ChangedRanges()) {
            for (usize i = range.first; i < range.first + range.count; ++i) {
                if (firstVertex_[i] == NumericLimits<uint32>::Max()) {
                    continue;
                }

                auto added = AppendVertices(boxes[i], scratchVertices_);
                if (!added) {
                    return added;
                }

                const uint32 first = firstVertex_[i];
                if (!scratchRanges_.IsEmpty()) {
                    auto& previous = scratchRanges_[scratchRanges_.Size() - 1];
                    if (previous.first + previous.count == first) {
                        previous.count += 4;
                        continue;
                    }
                }

                auto rangeAdded = scratchRanges_.TryPushBack({first, 4});
                if (!rangeAdded) {
                    return rangeAdded;
                }
            }
        }

        usize read = 0;
        for (const auto range : scratchRanges_) {
            for (uint32 vertex = 0; vertex < range.count; ++vertex) {
                vertices_[range.first + vertex] = scratchVertices_[read++];
            }
        }

        auto previous = Move(changedVertices_);
        changedVertices_ = Move(scratchRanges_);
        scratchRanges_ = Move(previous);
        ++batchRevision_;
    }

    cache_ = &cache;
    revision_ = cache.Revision();
    return Result<void>::Success();
}

Span<const DrawVertex> DrawBatch::Vertices() const noexcept {
    return vertices_.View();
}

uint64 DrawBatch::Revision() const noexcept {
    return batchRevision_;
}

uint64 DrawBatch::TopologyRevision() const noexcept {
    return topologyRevision_;
}

Span<const DrawVertexRange> DrawBatch::ChangedVertices() const noexcept {
    return changedVertices_.View();
}

Span<const uint32> DrawBatch::Indices() const noexcept {
    return indices_.View();
}

Span<const DrawCommand> DrawBatch::Commands() const noexcept {
    return commands_.View();
}

} // namespace rocks::ui
