#include <TextLayout/TextLayoutModel.h>
#include <Base/NumericLimits.h>

namespace rocks::text {

namespace {

usize ByteBegin(Span<const TextCluster> clusters, usize index) noexcept {
    return index == 0 ? 0 : clusters[index - 1].byteEnd;
}

} // namespace

bool ParagraphMetrics::IsValid() const noexcept {
    return margin.IsFinite() && margin.x >= 0 && margin.y >= 0 && margin.z >= 0 && margin.w >= 0 &&
           math::IsFinite(lineHeightScale) && lineHeightScale > 0;
}

TextLayoutModel::TextLayoutModel(Allocator& allocator) noexcept
    : allocator_(&allocator), ends_(allocator), lines_(allocator), visual_(allocator) {
}

Result<void> TextLayoutModel::Layout(Span<const TextCluster> clusters, float wrapWidth,
                                     float lineHeight, TextAlignment alignment, bool rightToLeft,
                                     float alignmentWidth, TextWrapPolicy policy,
                                     const ParagraphMetrics& metrics) noexcept {
    if (!metrics.IsValid() || !math::IsFinite(wrapWidth) || wrapWidth < 0 ||
        !math::IsFinite(lineHeight) || lineHeight <= 0 || !math::IsFinite(alignmentWidth) ||
        alignmentWidth < 0 ||
        (policy != TextWrapPolicy::Word && policy != TextWrapPolicy::WordOrCluster) ||
        (alignment != TextAlignment::Left && alignment != TextAlignment::Center &&
         alignment != TextAlignment::Right)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Array<usize> ends(*allocator_);
    Array<TextLine> lines(*allocator_);
    Array<VisualCluster> visual(*allocator_);
    auto reserved = ends.TryReserve(clusters.Size());
    if (!reserved) {
        return reserved;
    }
    reserved = visual.TryReserve(clusters.Size());
    if (!reserved) {
        return reserved;
    }
    usize previousEnd = 0;
    for (const auto& cluster : clusters) {
        if (!math::IsFinite(cluster.advance) || cluster.advance < 0 ||
            cluster.byteEnd <= previousEnd || cluster.bidiLevel > 126 ||
            (rightToLeft && cluster.bidiLevel == 0)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        auto added = ends.TryPushBack(cluster.byteEnd);
        if (!added) {
            return added;
        }
        previousEnd = cluster.byteEnd;
    }

    const double horizontal = static_cast<double>(metrics.margin.x) + metrics.margin.z;
    const double innerWrap = math::Max(0.0, wrapWidth - horizontal);
    const double stride = static_cast<double>(lineHeight) * metrics.lineHeightScale;
    if (stride > NumericLimits<float>::Max() || static_cast<float>(stride) == 0) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    float widest = 0;
    usize begin = 0;
    for (;;) {
        usize end = begin;
        usize lastBreak = begin;
        double width = 0;
        double breakWidth = 0;
        while (end < clusters.Size() && !clusters[end].hardBreak) {
            const double next = width + clusters[end].advance;
            if (wrapWidth > 0 && next > innerWrap && end > begin &&
                (lastBreak > begin || policy == TextWrapPolicy::WordOrCluster)) {
                if (lastBreak > begin) {
                    end = lastBreak;
                    width = breakWidth;
                }
                break;
            }
            width = next;
            if (clusters[end++].breakAfter) {
                lastBreak = end;
                breakWidth = width;
            }
        }
        if (width > NumericLimits<float>::Max()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        const float measured = static_cast<float>(width);
        widest = math::Max(widest, measured);
        const usize first = visual.Size();
        usize trailing = end;
        while (trailing > begin && clusters[trailing - 1].resetAtLineEnd) {
            --trailing;
        }
        const auto levelAt = [&](usize i) noexcept -> uint8 {
            return i < trailing ? clusters[i].bidiLevel : static_cast<uint8>(rightToLeft);
        };
        uint8 highest = 0;
        uint8 lowestOdd = 127;
        for (usize i = begin; i < end; ++i) {
            const uint8 level = levelAt(i);
            highest = highest > level ? highest : level;
            if ((level & 1) != 0) {
                lowestOdd = lowestOdd < level ? lowestOdd : level;
            }
            auto added = visual.TryPushBack({i, 0, clusters[i].advance, (level & 1) != 0});
            if (!added) {
                return added;
            }
        }

        // BiDi 레벨을 높은 쪽부터 내리며 해당 레벨 이상의 연속 구간을 뒤집는다.
        for (int level = highest; level >= lowestOdd; --level) {
            usize i = first;
            while (i < visual.Size()) {
                if (levelAt(visual[i].logicalIndex) < level) {
                    ++i;
                    continue;
                }
                const usize start = i;
                while (i < visual.Size() && levelAt(visual[i].logicalIndex) >= level) {
                    ++i;
                }
                for (usize k = 0; k < (i - start) / 2; ++k) {
                    const auto tmp = visual[start + k];
                    visual[start + k] = visual[i - 1 - k];
                    visual[i - 1 - k] = tmp;
                }
            }
        }
        double x = 0;
        for (usize i = first; i < visual.Size(); ++i) {
            visual[i].x = static_cast<float>(x);
            x += visual[i].advance;
        }
        auto added = lines.TryPushBack({ByteBegin(clusters, begin), ByteBegin(clusters, end), first,
                                        end - begin, 0, measured});
        if (!added) {
            return added;
        }

        if (end == clusters.Size()) {
            break;
        }
        begin = clusters[end].hardBreak ? end + 1 : end;
    }

    // 모든 줄을 측정한 뒤 정렬해야 지정 너비가 없는 여러 줄 문단도 긴 줄을 기준으로 맞춰진다.
    const double extent = alignmentWidth > 0 ? math::Max(0.0, alignmentWidth - horizontal)
                                             : (wrapWidth > 0 ? innerWrap : widest);
    const double lastHeight = metrics.scaleLastLine ? stride : lineHeight;
    const double height = metrics.margin.y + static_cast<double>(lines.Size() - 1) * stride +
                          lastHeight + metrics.margin.w;
    const double width = horizontal + widest;
    if (height > NumericLimits<float>::Max() || width > NumericLimits<float>::Max()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    for (usize row = 0; row < lines.Size(); ++row) {
        auto& line = lines[row];
        const double spare = math::Max(0.0, extent - line.width);
        const double shift = alignment == TextAlignment::Right
                                 ? spare
                                 : (alignment == TextAlignment::Center ? spare * 0.5 : 0.0);
        line.x = static_cast<float>(metrics.margin.x + shift);
        line.y = static_cast<float>(metrics.margin.y + static_cast<double>(row) * stride);
        line.height = static_cast<float>(row + 1 == lines.Size() ? lastHeight : stride);
        if (line.x != 0) {
            for (usize i = 0; i < line.visualCount; ++i) {
                visual[line.firstVisual + i].x += line.x;
            }
        }
    }
    ends_ = Move(ends);
    lines_ = Move(lines);
    visual_ = Move(visual);
    lineHeight_ = static_cast<float>(stride);
    size_ = {static_cast<float>(width), static_cast<float>(height)};
    rightToLeft_ = rightToLeft;
    return Result<void>::Success();
}

Vec2 TextLayoutModel::DesiredSize() const noexcept {
    return size_;
}

Span<const TextLine> TextLayoutModel::Lines() const noexcept {
    return lines_.View();
}

Span<const VisualCluster> TextLayoutModel::VisualClusters() const noexcept {
    return visual_.View();
}

usize TextLayoutModel::HitTest(Vec2 local) const noexcept {
    if (lines_.IsEmpty() || !local.IsFinite()) {
        return 0;
    }
    const double row = math::Max(0.0, (static_cast<double>(local.y) - lines_[0].y) / lineHeight_);
    const usize index =
        row >= static_cast<double>(lines_.Size()) ? lines_.Size() - 1 : static_cast<usize>(row);
    const TextLine& line = lines_[index];
    for (usize i = 0; i < line.visualCount; ++i) {
        const auto& glyph = visual_[line.firstVisual + i];
        const usize begin = glyph.logicalIndex == 0 ? 0 : ends_[glyph.logicalIndex - 1];
        const usize end = ends_[glyph.logicalIndex];
        if (local.x < glyph.x + glyph.advance * 0.5f) {
            return glyph.rightToLeft ? end : begin;
        }
        if (local.x < glyph.x + glyph.advance || i + 1 == line.visualCount) {
            return glyph.rightToLeft ? begin : end;
        }
    }
    return line.byteBegin;
}

Result<Vec2> TextLayoutModel::CaretPosition(usize byteOffset, bool trailing) const noexcept {
    if (lines_.IsEmpty() || byteOffset > (ends_.IsEmpty() ? 0 : ends_[ends_.Size() - 1])) {
        return Result<Vec2>::Failure(ErrorCode::InvalidArgument);
    }
    for (usize row = 0; row < lines_.Size(); ++row) {
        const TextLine& line = lines_[row];
        if (row + 1 < lines_.Size() &&
            (byteOffset > line.byteEnd || (!trailing && byteOffset == lines_[row + 1].byteBegin))) {
            continue;
        }
        const float y = line.y;
        for (usize i = 0; i < line.visualCount; ++i) {
            const auto& glyph = visual_[line.firstVisual + i];
            const usize first = glyph.logicalIndex == 0 ? 0 : ends_[glyph.logicalIndex - 1];
            const usize last = ends_[glyph.logicalIndex];
            if (byteOffset < first || byteOffset > last ||
                (byteOffset == first && trailing && byteOffset != line.byteBegin) ||
                (byteOffset == last && !trailing && byteOffset != line.byteEnd)) {
                continue;
            }
            const bool atEnd = byteOffset == last || (byteOffset != first && trailing);
            return Result<Vec2>::Success(
                {glyph.x + (atEnd != glyph.rightToLeft ? glyph.advance : 0), y});
        }
        return Result<Vec2>::Success({line.x + (rightToLeft_ ? line.width : 0), y});
    }
    return Result<Vec2>::Failure(ErrorCode::InvalidArgument);
}

} // namespace rocks::text
