#include "EditableTextElement.h"

#include <Core/Text/StringBuilder.h>
#include <UiWidget/EditableTextWidget.generated.h>

namespace rocks::ui {

EditableTextElement::EditableTextElement(Allocator& allocator, ObjectRegistry& registry,
                                         ObjectHandle owner, SharedPtr<WidgetTextShaper> shaper,
                                         Platform platform, bool multiline, bool boxed) noexcept
    : Element(allocator), undo_(allocator), redo_(allocator), registry_(registry), owner_(owner),
      shaper_(Move(shaper)), focusText_(allocator), platform_(platform),
      padding_(boxed ? 4.0f : 0.0f), multiline_(multiline) {
    SetClipToBounds(true);
}

Result<void> EditableTextElement::Initialize(StringView text, float size, bool password) noexcept {
    auto& allocator = registry_.ObjectAllocator();
    auto child = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!child) {
        return Result<void>::Failure(child.Error());
    }
    label_ = Move(child.Value());
    auto added = TryAddChild(label_);
    if (!added) {
        return added;
    }
    auto shaped = Shape(text, size, password);
    if (shaped) {
        MoveCaret(text.ByteCount(), false);
    }
    return shaped;
}

Result<void> EditableTextElement::Shape(StringView text, float size, bool password) noexcept {
    if (!text.IsEmpty() && !shaper_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    if (text.IsEmpty()) {
        auto empty = label_->SetShapedText({}, {}, {}, size, false);
        if (!empty) {
            return empty;
        }
    } else if (password) {
        auto masked = ShapePassword(text, size);
        if (!masked) {
            return masked;
        }
    } else {
        auto value = Text::Invariant(registry_.ObjectAllocator(), text);
        if (!value) {
            return Result<void>::Failure(value.Error());
        }
        auto shaped = shaper_->Shape(*label_, value.Value(), size);
        if (!shaped) {
            return shaped;
        }
    }
    if (password) {
        focusText_.Clear();
        canRevert_ = false;
    }
    if (hint_) {
        hint_->SetVisibility(text.IsEmpty() ? Visibility::HitTestInvisible : Visibility::Collapsed);
    }
    composing_ = false;
    fontSize_ = size;
    InvalidateLayout();
    InvalidatePaint();
    return Result<void>::Success();
}

Result<void> EditableTextElement::ShapePassword(StringView text, float size) noexcept {
    // 실제 비밀번호를 셰이퍼에 전달하지 않는다. 가림 글리프 하나를 셰이핑하고
    // 원문 UTF-8 경계에 대응시켜 마우스 선택과 삭제가 같은 위치를 사용하게 한다.
    auto& allocator = registry_.ObjectAllocator();
    TextLabel bullet(allocator);
    auto value = Text::Invariant(allocator, "•");
    if (!value) {
        return Result<void>::Failure(value.Error());
    }
    auto shaped = shaper_->Shape(bullet, value.Value(), size);
    if (!shaped) {
        return shaped;
    }
    if (bullet.ShapedClusters().Size() != 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    auto points = text.TryCodePoints();
    if (!points) {
        return Result<void>::Failure(points.Error());
    }
    Array<text::TextCluster> clusters(allocator);
    Array<LabelGlyph> glyphs(allocator);
    for (auto it = points.Value().begin(); it != points.Value().end();) {
        const bool newline = *it == '\n';
        ++it;
        auto cluster = bullet.ShapedClusters()[0];
        cluster.byteEnd = static_cast<usize>(it.Position() - text.Data());
        cluster.hardBreak = newline;
        if (newline) {
            cluster.advance = 0;
        } else {
            for (auto glyph : bullet.ShapedGlyphs()) {
                glyph.clusterIndex = clusters.Size();
                auto added = glyphs.TryPushBack(glyph);
                if (!added) {
                    return added;
                }
            }
        }
        auto added = clusters.TryPushBack(cluster);
        if (!added) {
            return added;
        }
    }
    return label_->SetShapedText(text, clusters.View(), glyphs.View(), bullet.LineHeight(), false);
}

TextLabel& EditableTextElement::Label() const noexcept {
    return *label_;
}

Vec2 EditableTextElement::ComputeDesiredSize(float) const noexcept {
    Vec2 text = label_->GetDesiredSize();
    if (hint_ && label_->GetText().IsEmpty()) {
        text.x = math::Max(text.x, hint_->GetDesiredSize().x);
        text.y = math::Max(text.y, hint_->GetDesiredSize().y);
    }
    const Vec2 error = error_ ? error_->GetDesiredSize() : Vec2{};
    return {math::Max(text.x + 1, error.x) + padding_ * 2,
            math::Max(fontSize_, text.y) + padding_ * 2 + (error_ ? error.y + 2 : 0)};
}

Result<void> EditableTextElement::Arrange(const Geometry& geometry) noexcept {
    const float width = math::Max(1.0f, geometry.size.x - padding_ * 2);
    const float height = math::Max(1.0f, geometry.size.y - padding_ * 2);
    float errorHeight = 0;
    SetGeometry(geometry);
    if (error_) {
        auto wrappedError = error_->ArrangeForWidth(width);
        if (!wrappedError) {
            return wrappedError;
        }
        errorHeight = math::Min(error_->ComputeDesiredSize(1).y, height);
        auto placed = error_->ArrangeIfNeeded(GetGeometry().MakeChild(
            {width, errorHeight}, {1, 0, 0, 1, padding_, padding_ + height - errorHeight}, {}, {}));
        if (!placed) {
            return placed;
        }
    }
    const Vec2 area{width, math::Max(1.0f, height - (error_ ? errorHeight + 2 : 0))};
    if (hint_ && label_->GetText().IsEmpty()) {
        auto placed = label_->CopyParagraphTo(*hint_);
        if (placed) {
            placed = hint_->ArrangeIfNeeded(
                GetGeometry().MakeChild(area, {1, 0, 0, 1, padding_, padding_}, {}, {}));
        }
        if (!placed) {
            return placed;
        }
    }
    auto wrapped = label_->ArrangeForWidth(area.x);
    if (!wrapped) {
        return wrapped;
    }
    const auto pos = label_->CaretPosition(composing_ ? compositionCaret_ : caret_, false);
    if (pos) {
        scroll_.x =
            math::Max(0.0f, math::Clamp(scroll_.x, pos.Value().x + 1 - area.x, pos.Value().x));
        scroll_.y = math::Max(
            0.0f,
            math::Clamp(scroll_.y, pos.Value().y + math::Min(area.y, label_->LineHeight()) - area.y,
                        pos.Value().y));
    }
    const AffineTransform2D offset{1, 0, 0, 1, padding_ - scroll_.x, padding_ - scroll_.y};
    auto arranged = label_->ArrangeIfNeeded(GetGeometry().MakeChild(area, offset, {}, {}));
    if (arranged && focused_) {
        PositionCompositionWindow();
    }
    return arranged;
}

void EditableTextElement::PositionCompositionWindow() const noexcept {
    if (!platform_.moveCompositionWindow) {
        return;
    }
    auto cursor = label_->CaretPosition(composing_ ? compositionCaret_ : caret_, false);
    if (cursor) {
        auto point = cursor.Value();
        point.y += label_->LineHeight();
        point = label_->GetGeometry().render.Apply(point);
        int32 x = 0, y = 0;
        if (math::TryRoundToInt32(point.x, x) && math::TryRoundToInt32(point.y, y)) {
            platform_.moveCompositionWindow(x, y);
        }
    }
}

bool EditableTextElement::SupportsKeyboardFocus() const noexcept {
    return true;
}

void EditableTextElement::OnFocusReceived(uint32 user) noexcept {
    focused_ = true;
    focusUser_ = user;
    auto model = registry_.Resolve<EditableTextWidget>(registry_.OwnerAccess(), owner_);
    if (model) {
        auto* owner = model.Value().Get().Value();
        RememberFocusText(*owner);
        if (owner->selectAllOnFocus_) {
            keepPointerSelection_ = dragging_;
            SelectAll();
        }
    }
    if (platform_.setInlineComposition) {
        platform_.setInlineComposition(true);
        PositionCompositionWindow();
    }
    chord_ = {};
    InvalidatePaint();
}

void EditableTextElement::OnFocusLost(uint32 user) noexcept {
    if (!focused_ || user != focusUser_) {
        return;
    }
    focused_ = false;
    canRevert_ = false;
    focusText_.Clear();
    keepPointerSelection_ = false;
    if (platform_.setInlineComposition) {
        platform_.setInlineComposition(false);
    }
    chord_ = {};
    dragging_ = false;
    InvalidatePaint();
    auto model = registry_.ResolveForWrite<EditableTextWidget>(registry_.OwnerAccess(), owner_);
    if (model) {
        auto* widget = model.Value().Get().Value();
        if (widget->CachedElement().Get() == this) {
            EndComposition(*widget);
            widget->Commit(TextCommitReason::FocusLost);
        }
    }
}

void EditableTextElement::MoveCaret(usize pos, bool extend) noexcept {
    // 새로 삽입한 코드 포인트가 다음 문자와 한 클러스터로 결합하는 경우 그 끝에 둔다.
    if (pos != 0 && pos < label_->GetText().ByteCount()) {
        for (const auto& cluster : label_->ShapedClusters()) {
            if (cluster.byteEnd >= pos) {
                pos = cluster.byteEnd;
                break;
            }
        }
    }
    if (caret_ != pos || (!extend && anchor_ != pos)) {
        caret_ = pos;
        if (!extend) {
            anchor_ = pos;
        }
        InvalidateArrangement();
        InvalidatePaint();
    }
}

usize EditableTextElement::Adjacent(bool forward) const noexcept {
    usize previous = 0;
    for (const auto& cluster : label_->ShapedClusters()) {
        if (forward && cluster.byteEnd > caret_) {
            return cluster.byteEnd;
        }
        if (!forward && cluster.byteEnd >= caret_) {
            return previous;
        }
        previous = cluster.byteEnd;
    }
    return previous;
}

void EditableTextElement::Replace(EditableTextWidget& owner, usize begin, usize end,
                                  StringView insertion) noexcept {
    if (owner.IsReadOnly() || (begin == end && insertion.IsEmpty())) {
        return;
    }
    const StringView before = owner.GetText();
    auto left = before.TryLeftBytes(begin);
    auto right = before.TryMidBytes(end);
    if (!left || !right) {
        return;
    }
    StringBuilder<> result(registry_.ObjectAllocator());
    if (!result.TryAppend(left.Value()) || !result.TryAppend(insertion)) {
        return;
    }
    const usize pos = result.View().ByteCount();
    if (!result.TryAppend(right.Value())) {
        return;
    }
    auto& allocator = registry_.ObjectAllocator();
    String next(allocator);
    if (!next.TryAssign(result.View()) ||
        (!owner.IsPassword() && !undo_.TryEnsureCapacity(undo_.Size() + 1))) {
        return;
    }
    const usize previousAnchor = anchor_;
    const usize previousCaret = caret_;
    if (!owner.ExchangeText(next)) {
        return;
    }
    if (!owner.IsPassword()) {
        if (undo_.Size() == 64) {
            (void)undo_.TryRemoveAt(0);
        }
        (void)undo_.TryPushBack({Move(next), previousAnchor, previousCaret});
        redo_.Reset();
    }
    MoveCaret(pos, false);
    // 문자열은 콜백에서 다시 편집될 수 있다. 이 호출이 소유한 스냅샷을 전달한다.
    owner.changed_.ExecuteIfBound(result.View());
}

void EditableTextElement::EndComposition(EditableTextWidget& owner) noexcept {
    if (composing_) {
        (void)Shape(owner.GetText(), fontSize_, owner.IsPassword());
    }
}

void EditableTextElement::UpdateComposition(EditableTextWidget& owner,
                                            const input::InputEvent& event) noexcept {
    const usize begin = (anchor_ < caret_ ? anchor_ : caret_);
    const usize end = (anchor_ > caret_ ? anchor_ : caret_);
    const auto text = event.composition ? event.composition->View() : StringView{};
    if (owner.IsReadOnly() || text.IsEmpty()) {
        EndComposition(owner);
        return;
    }
    if (event.type == input::InputEventType::TextCompositionCommit) {
        EndComposition(owner);
        Paste(owner, begin, end, text);
        return;
    }
    if (event.compositionCaret > text.ByteCount() || !text.TryLeftBytes(event.compositionCaret)) {
        return;
    }
    StringBuilder<> preview(registry_.ObjectAllocator());
    const auto original = owner.GetText();
    if (preview.TryAppend(original.SliceBytes(0, begin)) && preview.TryAppend(text) &&
        preview.TryAppend(original.SliceBytes(end, original.ByteCount() - end)) &&
        Shape(preview.View(), fontSize_, owner.IsPassword())) {
        composing_ = true;
        compositionEnd_ = begin + text.ByteCount();
        compositionCaret_ = begin + event.compositionCaret;
    }
}

void EditableTextElement::Paste(EditableTextWidget& owner, usize begin, usize end,
                                StringView text) noexcept {
    // 클립보드의 CRLF·CR을 내부 LF로 맞춘다. 한 줄 입력에서는 줄 경계가 공백이 된다.
    auto points = text.TryCodePoints();
    if (!points || text.IsEmpty()) {
        return;
    }
    StringBuilder<> clean(registry_.ObjectAllocator());
    bool carriageReturn = false;
    for (auto point : points.Value()) {
        if (point == '\n' && carriageReturn) {
            carriageReturn = false;
            continue;
        }
        carriageReturn = point == '\r';
        if (carriageReturn || point == '\n') {
            point = multiline_ ? '\n' : ' ';
        }
        if (!clean.TryAppendCodePoint(point)) {
            return;
        }
    }
    Replace(owner, begin, end, clean.View());
}

void EditableTextElement::ClearHistory() noexcept {
    undo_.Reset();
    redo_.Reset();
}

void EditableTextElement::RestoreHistory(EditableTextWidget& owner, bool redo) noexcept {
    auto& from = redo ? redo_ : undo_;
    auto& to = redo ? undo_ : redo_;
    if (owner.IsReadOnly() || owner.IsPassword() || from.IsEmpty() ||
        !to.TryEnsureCapacity(to.Size() + 1)) {
        return;
    }
    const usize previousAnchor = anchor_;
    const usize previousCaret = caret_;
    auto& state = from[from.Size() - 1];
    auto snapshot = state.text.TryClone(registry_.ObjectAllocator());
    if (!snapshot || !owner.ExchangeText(state.text)) {
        return;
    }
    MoveCaret(state.anchor, false);
    MoveCaret(state.caret, true);
    (void)to.TryPushBack({Move(state.text), previousAnchor, previousCaret});
    (void)from.TryPopBack();
    owner.changed_.ExecuteIfBound(snapshot.Value().View());
}

bool EditableTextElement::OnPreviewKeyChord(uint32 user, const input::InputEvent&,
                                            KeyChord chord) noexcept {
    if (focused_ && focusUser_ == user) {
        chord_ = chord;
    }
    return false;
}

bool EditableTextElement::OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept {
    if (!focused_ || focusUser_ != user) {
        return false;
    }
    auto model = registry_.ResolveForWrite<EditableTextWidget>(registry_.OwnerAccess(), owner_);
    if (!model) {
        return false;
    }
    auto* owner = model.Value().Get().Value();
    if (owner->CachedElement().Get() != this) {
        return false;
    }
    const usize begin = anchor_ < caret_ ? anchor_ : caret_;
    const usize end = anchor_ < caret_ ? caret_ : anchor_;
    if (event.type == input::InputEventType::TextComposition ||
        event.type == input::InputEventType::TextCompositionCommit) {
        UpdateComposition(*owner, event);
        return true;
    }
    if (composing_) {
        // 조합 중의 방향키·삭제·Enter는 IME가 처리한다. 확정 문자열은 별도 이벤트로 받는다.
        return true;
    }
    if (event.type == input::InputEventType::Text) {
        // 플랫폼은 Enter·Backspace의 제어 문자도 보낼 수 있다. 편집 명령은 KeyDown에서만 처리한다.
        if (event.codePoint >= 32 && event.codePoint != 127) {
            StringBuilder<> value(registry_.ObjectAllocator());
            if (value.TryAppendCodePoint(event.codePoint)) {
                Replace(*owner, begin, end, value.View());
            }
        }
        return true;
    }
    using input::Key;
    if (event.type != input::InputEventType::KeyDown) {
        return false;
    }
    const bool extend = chord_.shift;
    const bool command = (chord_.control || chord_.super) && !chord_.alt;
    switch (event.key) {
    case Key::C:
    case Key::X:
    case Key::V: {
        if (!command) {
            return false;
        }
        if (event.key == Key::V) {
            if (!owner->IsReadOnly()) {
                String clipboard(registry_.ObjectAllocator());
                if (platform_.readClipboardText && platform_.readClipboardText(clipboard)) {
                    Paste(*owner, begin, end, clipboard.View());
                }
            }
        } else if (!owner->IsPassword() && begin != end &&
                   (event.key == Key::C || !owner->IsReadOnly())) {
            const auto selection = owner->GetText().SliceBytes(begin, end - begin);
            if (platform_.writeClipboardText && platform_.writeClipboardText(selection) &&
                event.key == Key::X) {
                Replace(*owner, begin, end, {});
            }
        }
        break;
    }
    case Key::Z:
    case Key::Y:
        if (!command) {
            return false;
        }
        RestoreHistory(*owner, event.key == Key::Y || extend);
        break;
    case Key::A:
        if (!command) {
            return false;
        }
        anchor_ = 0;
        MoveCaret(owner->GetText().ByteCount(), true);
        InvalidatePaint();
        break;
    case Key::Left:
    case Key::Right: {
        const bool forward = event.key == Key::Right;
        MoveCaret(!extend && begin != end ? (forward ? end : begin) : Adjacent(forward), extend);
        break;
    }
    case Key::Home:
    case Key::End:
    case Key::Up:
    case Key::Down: {
        auto pos = label_->CaretPosition(caret_, false);
        if (!pos) {
            return true;
        }
        Vec2 point = pos.Value();
        if (event.key == Key::Up || event.key == Key::Down) {
            if (!multiline_) {
                return false;
            }
            point.y += (event.key == Key::Up ? -0.5f : 1.5f) * label_->LineStride();
        } else {
            point.x = event.key == Key::Home ? -1.0f : NumericLimits<float>::Max();
            point.y += 0.5f * label_->LineStride();
        }
        MoveCaret(label_->HitText(point), extend);
        break;
    }
    case Key::Backspace:
        Replace(*owner, begin == end ? Adjacent(false) : begin, end, {});
        break;
    case Key::Delete:
        Replace(*owner, begin, begin == end ? Adjacent(true) : end, {});
        break;
    case Key::Escape:
        return RevertToFocusText(*owner);
    case Key::Enter:
    case Key::NumpadEnter:
        if (multiline_) {
            Replace(*owner, begin, end, "\n");
        } else if (!event.repeat) {
            RememberFocusText(*owner);
            if (owner->selectAllOnCommit_) {
                SelectAll();
            }
            owner->Commit(TextCommitReason::Enter);
        }
        break;
    default:
        return false;
    }
    return true;
}

usize EditableTextElement::PointerOffset(Vec2 point) const noexcept {
    AffineTransform2D inverse;
    return label_->GetGeometry().render.TryInverse(inverse) ? label_->HitText(inverse.Apply(point))
                                                            : caret_;
}

PointerReply EditableTextElement::OnPointerEvent(const PointerEvent& event) noexcept {
    if (composing_) {
        return {.handled = true};
    }
    if (event.type == PointerEventType::Down && !dragging_ &&
        (event.source != PointerSource::Mouse || event.button == input::MouseButton::Left)) {
        pointerAnchor_ = PointerOffset(event.position);
        keepPointerSelection_ = false;
        MoveCaret(pointerAnchor_, false);
        dragging_ = true;
        dragSource_ = event.source;
        dragPointer_ = event.pointerId;
        dragUser_ = event.userId;
        return {.handled = true, .capture = true};
    }
    if (dragging_ && dragSource_ == event.source && dragPointer_ == event.pointerId &&
        dragUser_ == event.userId) {
        if (event.type == PointerEventType::Move || event.type == PointerEventType::Up) {
            const usize offset = PointerOffset(event.position);
            if (!keepPointerSelection_ || offset != pointerAnchor_) {
                if (keepPointerSelection_) {
                    MoveCaret(pointerAnchor_, false);
                    keepPointerSelection_ = false;
                }
                MoveCaret(offset, true);
            }
        }
        if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
            dragging_ = false;
            return {.handled = true, .releaseCapture = true};
        }
        return {.handled = true};
    }
    return {};
}

void EditableTextElement::OnPointerCaptureLost(PointerSource source, uint32 user,
                                               uint32 pointer) noexcept {
    if (source == dragSource_ && user == dragUser_ && pointer == dragPointer_) {
        dragging_ = false;
    }
}

Result<void> EditableTextElement::OnPaint(PaintList& list, int32 layer) const noexcept {
    if (padding_ > 0) {
        auto painted = list.TryAddBox({GetGeometry(), {0.06f, 0.07f, 0.09f, 1}, layer});
        if (!painted) {
            return painted;
        }
    }
    if (!focused_ || (!composing_ && caret_ == anchor_)) {
        return Result<void>::Success();
    }
    const usize begin = caret_ < anchor_ ? caret_ : anchor_;
    const usize end = composing_ ? compositionEnd_ : (anchor_ > caret_ ? anchor_ : caret_);
    const auto& layout = label_->Layout();
    for (const auto& line : layout.Lines()) {
        for (usize i = 0; i < line.visualCount; ++i) {
            const auto& visual = layout.VisualClusters()[line.firstVisual + i];
            const auto cluster = label_->ShapedClusters()[visual.logicalIndex];
            if (begin < cluster.byteEnd && cluster.byteEnd <= end && visual.advance > 0) {
                const auto box = label_->GetGeometry().MakeChild(
                    {visual.advance, composing_ ? 1.0f : line.height},
                    {1, 0, 0, 1, visual.x, composing_ ? line.y + label_->LineHeight() - 1 : line.y},
                    {}, {});
                auto added = list.TryAddBox({box, {0.12f, 0.32f, 0.7f, 0.7f}, layer});
                if (!added) {
                    return added;
                }
            }
        }
    }
    return Result<void>::Success();
}

Result<void> EditableTextElement::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    if (!focused_) {
        return Result<void>::Success();
    }
    const auto pos = label_->CaretPosition(composing_ ? compositionCaret_ : caret_, false);
    const auto point = pos ? pos.Value() : Vec2{};
    const auto caret = label_->GetGeometry().MakeChild({1, label_->LineHeight()},
                                                       {1, 0, 0, 1, point.x, point.y}, {}, {});
    return list.TryAddBox({caret, {1, 1, 1, 1}, layer});
}

} // namespace rocks::ui
