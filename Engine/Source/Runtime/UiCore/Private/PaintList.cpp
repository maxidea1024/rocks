#include <UiCore/PaintList.h>

namespace rocks::ui {

namespace {

// 9분할 브러시는 3×3 조각이고, 한 축은 경계 위치 넷으로 나뉜다.
constexpr usize NineSliceAxisCount = 3;
constexpr usize NineSlicePieceCount = NineSliceAxisCount * NineSliceAxisCount;
constexpr usize NineSliceEdgeCount = NineSliceAxisCount + 1;

} // namespace

PaintList::PaintList(Allocator& allocator) noexcept : boxes_(allocator), clips_(allocator) {
}

Result<void> PaintList::TryAddBox(const PaintBox& box) noexcept {
    PaintBox clipped = box;
    if (box.operation == PaintOperation::Draw) {
        const float opacity =
            disabled_ && !disabledExempt_ ? opacity_ * DisabledEffectOpacity : opacity_;
        clipped.color.a *= opacity;
        clipped.outlineColor.a *= opacity;
    }

    if (!clips_.IsEmpty()) {
        clipped.hasClip = true;
        clipped.emptyClip = clips_[clips_.Size() - 1].empty;
        clipped.clip = clips_[clips_.Size() - 1].bounds;
    }

    return boxes_.TryPushBack(clipped);
}

Result<void> PaintList::TryAddGlyphRun(const Geometry& geometry, Span<const PaintGlyph> glyphs,
                                       LinearColor color, int32 layer,
                                       bool useGlyphColor) noexcept {
    if (!color.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const auto tint = [color, useGlyphColor](LinearColor glyph) noexcept {
        return useGlyphColor ? color * glyph
                             : LinearColor(color.r, color.g, color.b, color.a * glyph.a);
    };
    usize count = 0;
    for (const PaintGlyph& glyph : glyphs) {
        if (!glyph.position.IsFinite() || !glyph.size.IsFinite() || glyph.size.x < 0 ||
            glyph.size.y < 0 || !glyph.uv.IsValid() || !tint(glyph.tint).IsFinite()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (glyph.size.x > 0 && glyph.size.y > 0) {
            if (glyph.resource.IsNull()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            ++count;
        }
    }

    if (count > MaxSize - boxes_.Size()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto reserved = boxes_.TryEnsureCapacity(boxes_.Size() + count);
    if (!reserved) {
        return reserved;
    }

    // 축 정렬 변환(회전·기울임 없음)이면 글리프 왼쪽 위를 창 픽셀에 맞춘다(UE 글자 픽셀 맞춤).
    // 가운데 정렬 등으로 0.5 px에 놓인 글리프는 두 픽셀에 걸쳐 보간되어 뿌옇게 보인다.
    const AffineTransform2D& render = geometry.render;
    const bool snap = render.m01 == 0 && render.m10 == 0 && render.m00 > 0 && render.m11 > 0;
    for (const PaintGlyph& glyph : glyphs) {
        if (glyph.size.x == 0 || glyph.size.y == 0) {
            continue;
        }

        Vec2 position = glyph.position;
        if (snap) {
            const Vec2 window = render.Apply(position);
            position.x += (math::Round(window.x) - window.x) / render.m00;
            position.y += (math::Round(window.y) - window.y) / render.m11;
        }

        const AffineTransform2D offset{1, 0, 0, 1, position.x, position.y};
        PaintBox box{geometry.MakeChild(glyph.size, offset, {}, {}), tint(glyph.tint), layer};
        box.resource = glyph.resource;
        box.uvStart = glyph.uv.min;
        box.uvEnd = glyph.uv.max;
        auto added = TryAddBox(box);
        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

namespace {

struct SliceAxis {
    float positions[NineSliceEdgeCount];
    float uvs[NineSliceEdgeCount];
};

SliceAxis Slice(float length, float original, float before, float after, float uvMin, float uvMax,
                bool mirror) noexcept {
    const float start = mirror ? uvMax : uvMin;
    const float end = mirror ? uvMin : uvMax;
    const float first = mirror ? after : before;
    const float last = mirror ? before : after;
    float nearEdge = original * first;
    float farEdge = length - original * last;
    if (nearEdge > farEdge) {
        nearEdge = farEdge = length * 0.5f;
    }

    return {{0, nearEdge, farEdge, length},
            {start, start + (end - start) * first, end - (end - start) * last, end}};
}

} // namespace

Result<void> PaintList::TryAddBrush(const Geometry& geometry, const Brush& brush, LinearColor color,
                                    int32 layer) noexcept {
    return TryAddBrushRegion(geometry, brush, color, layer, {{}, geometry.size});
}

Result<void> PaintList::TryAddBrushRegion(const Geometry& geometry, const Brush& brush,
                                          LinearColor color, int32 layer,
                                          Box2<float> region) noexcept {
    if (!region.min.IsFinite() || !region.max.IsFinite() || region.min.x > region.max.x ||
        region.min.y > region.max.y) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!brush.IsValid() || !geometry.size.IsFinite() || geometry.size.x < 0 ||
        geometry.size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const LinearColor tint = color * brush.tint;
    if (!tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (brush.mode == BrushMode::None || geometry.size.x == 0 || geometry.size.y == 0) {
        return Result<void>::Success();
    }

    PaintBox box{geometry, tint, layer};
    box.resource = brush.resource;
    const bool crop = region.min != Vec2{} || region.max != geometry.size;
    const auto addPiece = [&](Vec2 origin) noexcept -> Result<void> {
        if (crop) {
            const auto extent = box.geometry.size;
            const Vec2 start{math::Max(origin.x, region.min.x), math::Max(origin.y, region.min.y)};
            const Vec2 end{math::Min(origin.x + extent.x, region.max.x),
                           math::Min(origin.y + extent.y, region.max.y)};
            if (end.x <= start.x || end.y <= start.y) {
                return Result<void>::Success();
            }

            const auto uv = box.uvStart;
            const auto span = box.uvEnd - uv;
            box.uvStart = uv + Vec2{span.x * ((start.x - origin.x) / extent.x),
                                    span.y * ((start.y - origin.y) / extent.y)};
            box.uvEnd = uv + Vec2{span.x * ((end.x - origin.x) / extent.x),
                                  span.y * ((end.y - origin.y) / extent.y)};
            box.geometry = geometry.MakeChild(end - start, {1, 0, 0, 1, start.x, start.y}, {}, {});
        }

        return TryAddBox(box);
    };
    if (brush.mode == BrushMode::RoundedBox) {
        // 상자 전체 크기와 반경을 싣고, 잘라낸 조각은 UV 부분 구간으로 같은 모양을 유지한다.
        const float half = geometry.size.y * 0.5f;
        box.roundedBox = true;
        box.roundedSize = geometry.size;
        box.cornerRadius = brush.outline.rounding == BrushRounding::HalfHeight
                               ? Vec4{half, half, half, half}
                               : brush.outline.cornerRadii;
        box.outlineWidth = brush.outline.width;
        box.outlineColor = brush.outline.color;
        return addPiece({});
    }

    if (brush.mode == BrushMode::Image) {
        Box2<float> uv = brush.uv;
        if (!brush.vectorImage.IsNull()) {
            // 벡터 brush: 그릴 화소 크기로 래스터화한 자리로 바꾼다(17.2절). 화소 크기는 로컬 크기
            // x 누적 변환의 두 열 길이를 반올림한 것이다(UE FVectorCacheKey는 IntPoint로 버린다 —
            // float 오차로 1 px 큰 래스터가 생기지 않게 반올림한다).
            if (vectorImages_ == nullptr) {
                return Result<void>::Success();
            }

            const AffineTransform2D& render = geometry.render;
            const float scaleX = math::Sqrt(render.m00 * render.m00 + render.m10 * render.m10);
            const float scaleY = math::Sqrt(render.m01 * render.m01 + render.m11 * render.m11);
            const float drawnWidth = geometry.size.x * scaleX;
            const float drawnHeight = geometry.size.y * scaleY;
            if (!math::IsFinite(drawnWidth) || !math::IsFinite(drawnHeight)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            const float limit = static_cast<float>(MaxVectorImagePixels);
            const float width = math::Clamp(math::Round(drawnWidth), 1.0f, limit);
            const float height = math::Clamp(math::Round(drawnHeight), 1.0f, limit);
            auto resolved = vectorImages_->Resolve(brush.vectorImage, static_cast<uint32>(width),
                                                   static_cast<uint32>(height));
            if (!resolved) {
                return Result<void>::Failure(resolved.Error());
            }

            // brush uv는 래스터 자리 안의 부분 범위다(래스터 brush와 같은 뜻).
            const Box2<float>& slot = resolved.Value().uv;
            const Vec2 span = slot.max - slot.min;
            box.resource = resolved.Value().resource;
            uv = {{slot.min.x + span.x * brush.uv.min.x, slot.min.y + span.y * brush.uv.min.y},
                  {slot.min.x + span.x * brush.uv.max.x, slot.min.y + span.y * brush.uv.max.y}};
        }

        box.uvStart = {brush.mirrorX ? uv.max.x : uv.min.x, brush.mirrorY ? uv.max.y : uv.min.y};
        box.uvEnd = {brush.mirrorX ? uv.min.x : uv.max.x, brush.mirrorY ? uv.min.y : uv.max.y};
        return addPiece({});
    }

    // 일부 조각만 목록에 남지 않도록 필요한 공간을 먼저 확보한다.
    if (boxes_.Size() > MaxSize - NineSlicePieceCount) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto reserved = boxes_.TryEnsureCapacity(boxes_.Size() + NineSlicePieceCount);
    if (!reserved) {
        return reserved;
    }

    const auto x = Slice(geometry.size.x, brush.imageSize.x, brush.margin.x, brush.margin.z,
                         brush.uv.min.x, brush.uv.max.x, brush.mirrorX);
    const auto y = Slice(geometry.size.y, brush.imageSize.y, brush.margin.y, brush.margin.w,
                         brush.uv.min.y, brush.uv.max.y, brush.mirrorY);
    for (usize row = 0; row < NineSliceAxisCount; ++row) {
        for (usize col = 0; col < NineSliceAxisCount; ++col) {
            if (brush.mode == BrushMode::Border && row == 1 && col == 1) {
                continue;
            }

            const Vec2 size{x.positions[col + 1] - x.positions[col],
                            y.positions[row + 1] - y.positions[row]};
            if (size.x <= 0 || size.y <= 0) {
                continue;
            }

            const AffineTransform2D offset{1, 0, 0, 1, x.positions[col], y.positions[row]};
            box.geometry = geometry.MakeChild(size, offset, {}, {});
            box.uvStart = {x.uvs[col], y.uvs[row]};
            box.uvEnd = {x.uvs[col + 1], y.uvs[row + 1]};
            auto added = addPiece({x.positions[col], y.positions[row]});
            if (!added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> PaintList::PushClip(const Geometry& geometry) noexcept {
    const AffineTransform2D& transform = geometry.render;
    const bool axisAligned =
        (transform.m01 == 0 && transform.m10 == 0) || (transform.m00 == 0 && transform.m11 == 0);
    if (!axisAligned) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const Vec2 a = transform.Apply({0, 0});
    const Vec2 b = transform.Apply(geometry.size);
    if (!a.IsFinite() || !b.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ClipState state;
    state.bounds.min = Vec2(math::Min(a.x, b.x), math::Min(a.y, b.y));
    state.bounds.max = Vec2(math::Max(a.x, b.x), math::Max(a.y, b.y));
    if (!clips_.IsEmpty()) {
        const ClipState& parent = clips_[clips_.Size() - 1];
        state.bounds.min.x = math::Max(state.bounds.min.x, parent.bounds.min.x);
        state.bounds.min.y = math::Max(state.bounds.min.y, parent.bounds.min.y);
        state.bounds.max.x = math::Min(state.bounds.max.x, parent.bounds.max.x);
        state.bounds.max.y = math::Min(state.bounds.max.y, parent.bounds.max.y);
        state.empty = parent.empty;
    }

    state.empty = state.empty || state.bounds.min.x >= state.bounds.max.x ||
                  state.bounds.min.y >= state.bounds.max.y;
    return clips_.TryPushBack(state);
}

void PaintList::PopClip() noexcept {
    if (!clips_.IsEmpty()) {
        (void)clips_.TryRemoveAt(clips_.Size() - 1);
    }
}

Span<const PaintBox> PaintList::Boxes() const noexcept {
    return boxes_.View();
}

float PaintList::InheritedOpacity() const noexcept {
    return opacity_;
}

void PaintList::SetVectorImageResolver(VectorImageResolver* resolver) noexcept {
    vectorImages_ = resolver;
}

VectorImageResolver* PaintList::GetVectorImageResolver() const noexcept {
    return vectorImages_;
}

void PaintList::SetTheme(const ThemeColors* theme, uint64 revision) noexcept {
    theme_ = theme;
    themeRevision_ = revision;
}

uint64 PaintList::ThemeRevision() const noexcept {
    return themeRevision_;
}

const ThemeColors& PaintList::Theme() const noexcept {
    return theme_ ? *theme_ : DefaultThemeColors();
}

LinearColor PaintList::ForegroundColor() const noexcept {
    return foreground_;
}

bool PaintList::IsDisabled() const noexcept {
    return disabled_;
}

bool PaintList::IsDisabledEffect() const noexcept {
    return disabled_ && !disabledExempt_;
}

StyleColorContext PaintList::ColorContext() const noexcept {
    return {foreground_, foreground_};
}

void PaintList::Clear() noexcept {
    opacity_ = 1;
    foreground_ = {1, 1, 1, 1};
    disabled_ = false;
    disabledExempt_ = false;
    boxes_.Clear();
    clips_.Clear();
}

Result<void> PaintList::TryAppendRecorded(Span<const PaintBox> boxes) noexcept {
    if (clips_.IsEmpty() && opacity_ == 1) {
        return boxes_.TryAppend(boxes);
    }

    if (boxes.Size() > MaxSize - boxes_.Size()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto reserved = boxes_.TryEnsureCapacity(boxes_.Size() + boxes.Size());
    if (!reserved) {
        return reserved;
    }

    for (PaintBox box : boxes) {
        if (box.operation == PaintOperation::Draw) {
            box.color.a *= opacity_;
            box.outlineColor.a *= opacity_;
        }

        if (clips_.IsEmpty()) {
            (void)boxes_.TryPushBack(box);
            continue;
        }

        const auto& clip = clips_[clips_.Size() - 1];
        if (box.hasClip) {
            box.clip.min.x = math::Max(box.clip.min.x, clip.bounds.min.x);
            box.clip.min.y = math::Max(box.clip.min.y, clip.bounds.min.y);
            box.clip.max.x = math::Min(box.clip.max.x, clip.bounds.max.x);
            box.clip.max.y = math::Min(box.clip.max.y, clip.bounds.max.y);
        } else {
            box.clip = clip.bounds;
        }

        box.hasClip = true;
        box.emptyClip = box.emptyClip || clip.empty || box.clip.min.x >= box.clip.max.x ||
                        box.clip.min.y >= box.clip.max.y;
        (void)boxes_.TryPushBack(box);
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
