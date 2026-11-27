#include "EditableTextElement.h"

#include <Core/Text/StringBuilder.h>
#include <UiCore/TextInputPositions.h>
#include <UiWidget/EditableTextWidget.generated.h>

namespace rocks::ui {

namespace {

usize SmallerOffset(usize a, usize b) noexcept {
    return a < b ? a : b;
}

// 상자형 편집 칸의 안쪽 여백, 되돌리기 기록 상한이다.
constexpr float BoxedTextPadding = 4.0f;
constexpr usize MaxUndoSteps = 64;
// 글자로 넣지 않는 제어 문자: U+0000–U+001F와 DEL(U+007F)이다.
constexpr uint32 FirstPrintableCodePoint = 0x20;
constexpr uint32 DeleteCodePoint = 0x7F;

} // namespace

EditableTextElement::EditableTextElement(Allocator& allocator, ObjectRegistry& registry,
                                         ObjectHandle owner, SharedPtr<WidgetTextShaper> shaper,
                                         Platform platform, bool multiline, bool boxed) noexcept
    : Element(allocator), undo_(allocator), redo_(allocator), registry_(registry), owner_(owner),
      shaper_(Move(shaper)), focusText_(allocator), platform_(platform),
      padding_(boxed ? BoxedTextPadding : 0.0f), preview_(allocator), multiline_(multiline) {
    SetClipToBounds(true);
}

EditableTextElement::~EditableTextElement() noexcept {
    // 서비스가 남은 기록을 이 문맥에 적용하지 않게 조합을 먼저 버린 뒤 끈다(19.2절).
    if (auto service = Move(textInput_)) {
        composing_ = false;
        service->CancelComposition(*this);
        service->Deactivate(*this);
    }
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
    // 확정 글자로 셰이핑한다. 조합 중이면 앱이 글자를 바꾼 것이다(바인딩·되돌리기 — 19.2절).
    AbandonComposition();
    return ShapeText(text, size, password);
}

void EditableTextElement::AbandonComposition() noexcept {
    if (!composing_) {
        return;
    }

    composing_ = false;
    preview_.Clear();
    anchor_ = preCompositionAnchor_;
    caret_ = preCompositionCaret_;
    if (textInput_) {
        textInput_->CancelComposition(*this);
    }
}

Result<void> EditableTextElement::ShapeText(StringView text, float size, bool password) noexcept {
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

    const auto pos = label_->CaretPosition(caret_, false);
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
    if (arranged && textInput_) {
        // 글자 위치가 정해졌다 — 후보 창을 caret으로 옮긴다.
        textInput_->NotifyLayoutChanged(*this);
    }

    return arranged;
}

const StyleBrush* EditableTextElement::FocusOutlineBrush() const noexcept {
    return nullptr;
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

    textInput_ = FindTextInputService();
    if (textInput_) {
        textInput_->Activate(*this);
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
    chord_ = {};
    dragging_ = false;
    InvalidatePaint();
    // 서비스가 만들던 글자를 확정해 넣은 뒤 끈다(19.2절). Commit(FocusLost)이 그 글자를 본다.
    if (auto service = Move(textInput_)) {
        service->Deactivate(*this);
    }

    EndComposition();
    auto model = registry_.ResolveForWrite<EditableTextWidget>(registry_.OwnerAccess(), owner_);
    if (model) {
        auto* widget = model.Value().Get().Value();
        if (widget->CachedElement().Get() == this) {
            widget->Commit(TextCommitReason::FocusLost);
        }
    }
}

void EditableTextElement::RefreshTextInput() noexcept {
    if (textInput_) {
        textInput_->Activate(*this);
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
        if (undo_.Size() == MaxUndoSteps) {
            (void)undo_.TryRemoveAt(0);
        }

        (void)undo_.TryPushBack({Move(next), previousAnchor, previousCaret});
        redo_.Reset();
    }

    MoveCaret(pos, false);
    // 문자열은 콜백에서 다시 편집될 수 있다. 이 호출이 소유한 스냅샷을 전달한다.
    owner.changed_.ExecuteIfBound(result.View());
}

void EditableTextElement::CancelComposition(EditableTextWidget& owner) noexcept {
    if (composing_) {
        AbandonComposition();
        (void)ShapeText(owner.GetText(), fontSize_, owner.IsPassword());
    }
}

Result<void> EditableTextElement::AppendInsertion(StringView text,
                                                  StringBuilder<>& out) const noexcept {
    // 클립보드·IME의 CRLF·CR을 내부 LF로 맞춘다. 한 줄 입력에서는 줄 경계가 공백이 된다.
    auto points = text.TryCodePoints();
    if (!points) {
        return Result<void>::Failure(points.Error());
    }

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

        auto appended = out.TryAppendCodePoint(point);
        if (!appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

void EditableTextElement::Paste(EditableTextWidget& owner, usize begin, usize end,
                                StringView text) noexcept {
    StringBuilder<> clean(registry_.ObjectAllocator());
    if (!text.IsEmpty() && AppendInsertion(text, clean)) {
        Replace(owner, begin, end, clean.View());
    }
}

usize EditableTextElement::ByteOffset(uint32 index) const noexcept {
    return text_input::ToByteOffset(label_->GetText(), index);
}

bool EditableTextElement::IsReadOnly() const noexcept {
    auto model = registry_.Resolve<EditableTextWidget>(registry_.OwnerAccess(), owner_);
    return !model || model.Value().Get().Value()->IsReadOnly();
}

bool EditableTextElement::AllowsInputMethod() const noexcept {
    auto model = registry_.Resolve<EditableTextWidget>(registry_.OwnerAccess(), owner_);
    if (!model) {
        return false;
    }

    const auto* owner = model.Value().Get().Value();
    return !owner->IsReadOnly() && !owner->IsPassword();
}

bool EditableTextElement::IsComposing() const noexcept {
    return composing_;
}

uint32 EditableTextElement::GetTextLength() const noexcept {
    return text_input::Utf16Length(label_->GetText());
}

void EditableTextElement::GetSelection(uint32& begin, uint32& length) const noexcept {
    const StringView text = label_->GetText();
    const uint32 first = text_input::ToUtf16Index(text, anchor_ < caret_ ? anchor_ : caret_);
    const uint32 last = text_input::ToUtf16Index(text, anchor_ < caret_ ? caret_ : anchor_);
    begin = first;
    length = last - first;
}

void EditableTextElement::SetSelection(uint32 begin, uint32 length) noexcept {
    const usize first = ByteOffset(begin);
    const usize last = ByteOffset(begin + length);
    MoveCaret(first, false);
    MoveCaret(last, true);
}

Result<void> EditableTextElement::GetText(uint32 begin, uint32 length,
                                          Array<char16_t>& out) const noexcept {
    const StringView text = label_->GetText();
    const usize first = ByteOffset(begin);
    const usize last = ByteOffset(begin + length);
    return text_input::AppendUtf16(text.SliceBytes(first, last - first), out);
}

void EditableTextElement::SetText(uint32 begin, uint32 length, Span<const char16_t> text) noexcept {
    const usize first = ByteOffset(begin);
    const usize last = ByteOffset(begin + length);
    StringBuilder<> utf8(registry_.ObjectAllocator());
    StringBuilder<> insertion(registry_.ObjectAllocator());
    if (!text_input::AppendUtf8(text, utf8) || !AppendInsertion(utf8.View(), insertion)) {
        return;
    }

    if (!composing_) {
        // 조합 밖의 바꾸기(TSF 재변환 등)는 편집 하나다.
        auto model = registry_.ResolveForWrite<EditableTextWidget>(registry_.OwnerAccess(), owner_);
        if (model && model.Value().Get().Value()->CachedElement().Get() == this) {
            Replace(*model.Value().Get().Value(), first, last, insertion.View());
        }

        return;
    }

    // 미리보기 층만 바꾼다(위젯 글자·이력·변경 알림 없음 — 19.2절).
    const StringView current = preview_.View();
    StringBuilder<> next(registry_.ObjectAllocator());
    if (!next.TryAppend(current.SliceBytes(0, first)) || !next.TryAppend(insertion.View()) ||
        !next.TryAppend(current.SliceBytes(last, current.ByteCount() - last)) ||
        !preview_.TryAssign(next.View())) {
        return;
    }

    auto model = registry_.Resolve<EditableTextWidget>(registry_.OwnerAccess(), owner_);
    const bool password = model && model.Value().Get().Value()->IsPassword();
    if (ShapeText(preview_.View(), fontSize_, password)) {
        const usize end = preview_.ByteCount();
        anchor_ = SmallerOffset(anchor_, end);
        caret_ = SmallerOffset(caret_, end);
        InvalidateArrangement();
    }
}

Optional<uint32> EditableTextElement::GetCharacterIndexFromPoint(Vec2 window) const noexcept {
    AffineTransform2D inverse;
    if (!label_->GetGeometry().render.TryInverse(inverse)) {
        return {};
    }

    return text_input::ToUtf16Index(label_->GetText(), label_->HitText(inverse.Apply(window)));
}

bool EditableTextElement::GetTextBounds(uint32 begin, uint32 length,
                                        Box2<float>& window) const noexcept {
    const auto first = label_->CaretPosition(ByteOffset(begin), false);
    const auto last = label_->CaretPosition(ByteOffset(begin + length), false);
    if (!first || !last) {
        return false;
    }

    // 범위 끝 두 caret을 감싸는 상자다(여러 줄이면 두 줄을 걸친다).
    const Vec2 low{math::Min(first.Value().x, last.Value().x),
                   math::Min(first.Value().y, last.Value().y)};
    const Vec2 high{math::Max(first.Value().x, last.Value().x),
                    math::Max(first.Value().y, last.Value().y) + label_->LineHeight()};
    const auto& render = label_->GetGeometry().render;
    const Vec2 a = render.Apply(low);
    const Vec2 b = render.Apply(high);
    window = {{math::Min(a.x, b.x), math::Min(a.y, b.y)},
              {math::Max(a.x, b.x), math::Max(a.y, b.y)}};
    return true;
}

bool EditableTextElement::GetScreenBounds(Box2<float>& window) const noexcept {
    const auto& geometry = GetGeometry();
    const Vec2 a = geometry.render.Apply({0, 0});
    const Vec2 b = geometry.render.Apply(geometry.size);
    window = {{math::Min(a.x, b.x), math::Min(a.y, b.y)},
              {math::Max(a.x, b.x), math::Max(a.y, b.y)}};
    return true;
}

void EditableTextElement::BeginComposition() noexcept {
    if (composing_ || !preview_.TryAssign(label_->GetText())) {
        return;
    }

    composing_ = true;
    preCompositionAnchor_ = anchor_;
    preCompositionCaret_ = caret_;
    compositionBegin_ = compositionEnd_ = anchor_ < caret_ ? anchor_ : caret_;
}

void EditableTextElement::UpdateCompositionRange(uint32 begin, uint32 length) noexcept {
    compositionBegin_ = ByteOffset(begin);
    compositionEnd_ = ByteOffset(begin + length);
    InvalidatePaint();
}

void EditableTextElement::EndComposition() noexcept {
    if (!composing_) {
        return;
    }

    // 미리보기를 한 번 확정한다: 위젯 글자 교체, 편집 이력 하나(조합 전 선택), 변경 알림 한 번.
    composing_ = false;
    const usize anchor = anchor_;
    const usize caret = caret_;
    auto model = registry_.ResolveForWrite<EditableTextWidget>(registry_.OwnerAccess(), owner_);
    auto* owner = model ? model.Value().Get().Value() : nullptr;
    if (!owner || owner->CachedElement().Get() != this || owner->IsReadOnly() ||
        preview_.View() == owner->GetText()) {
        if (owner) {
            (void)ShapeText(owner->GetText(), fontSize_, owner->IsPassword());
            const usize end = owner->GetText().ByteCount();
            MoveCaret(SmallerOffset(anchor, end), false);
            MoveCaret(SmallerOffset(caret, end), true);
        }

        preview_.Clear();
        return;
    }

    auto& allocator = registry_.ObjectAllocator();
    String next(allocator);
    auto snapshot = preview_.TryClone(allocator);
    if (!snapshot || !next.TryAssign(preview_.View()) ||
        (!owner->IsPassword() && !undo_.TryEnsureCapacity(undo_.Size() + 1)) ||
        !owner->ExchangeText(next)) {
        // 확정하지 못했다(메모리 부족) — 조합 전 글자로 돌린다.
        (void)ShapeText(owner->GetText(), fontSize_, owner->IsPassword());
        MoveCaret(SmallerOffset(preCompositionCaret_, owner->GetText().ByteCount()), false);
        preview_.Clear();
        return;
    }

    if (!owner->IsPassword()) {
        if (undo_.Size() == MaxUndoSteps) {
            (void)undo_.TryRemoveAt(0);
        }

        // ExchangeText가 바꾼 뒤 next는 조합 전 글자다.
        (void)undo_.TryPushBack({Move(next), preCompositionAnchor_, preCompositionCaret_});
        redo_.Reset();
    }

    preview_.Clear();
    MoveCaret(anchor, false);
    MoveCaret(caret, true);
    owner->changed_.ExecuteIfBound(snapshot.Value().View());
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

KeyReply EditableTextElement::OnKeyEvent(const KeyEvent& key) noexcept {
    const uint32 user = key.user;
    const input::InputEvent& event = key.input;
    if (!focused_ || focusUser_ != user) {
        return Element::OnKeyEvent(key);
    }

    auto model = registry_.ResolveForWrite<EditableTextWidget>(registry_.OwnerAccess(), owner_);
    if (!model) {
        return Element::OnKeyEvent(key);
    }

    auto* owner = model.Value().Get().Value();
    if (owner->CachedElement().Get() != this) {
        return Element::OnKeyEvent(key);
    }

    const usize begin = anchor_ < caret_ ? anchor_ : caret_;
    const usize end = anchor_ < caret_ ? caret_ : anchor_;
    if (composing_) {
        // 조합 중의 방향키·삭제·Enter는 IME가 처리한다. 확정은 텍스트 입력 문맥으로 온다.
        return KeyReply::Handled();
    }

    if (event.type == input::InputEventType::Text) {
        // 플랫폼은 Enter·Backspace의 제어 문자도 보낼 수 있다. 편집 명령은 KeyDown에서만 처리한다.
        if (event.codePoint >= FirstPrintableCodePoint && event.codePoint != DeleteCodePoint) {
            StringBuilder<> value(registry_.ObjectAllocator());
            if (value.TryAppendCodePoint(event.codePoint)) {
                Replace(*owner, begin, end, value.View());
            }
        }

        return KeyReply::Handled();
    }

    using input::Key;
    if (event.type != input::InputEventType::KeyDown) {
        return Element::OnKeyEvent(key);
    }

    const bool extend = chord_.shift;
    const bool command = (chord_.control || chord_.super) && !chord_.alt;
    switch (event.key) {
    case Key::C:
    case Key::X:
    case Key::V: {
        if (!command) {
            return Element::OnKeyEvent(key);
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
            return Element::OnKeyEvent(key);
        }

        RestoreHistory(*owner, event.key == Key::Y || extend);
        break;
    case Key::A:
        if (!command) {
            return Element::OnKeyEvent(key);
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
            return KeyReply::Handled();
        }

        Vec2 point = pos.Value();
        if (event.key == Key::Up || event.key == Key::Down) {
            if (!multiline_) {
                return Element::OnKeyEvent(key);
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
        return KeyReply::FromHandled(RevertToFocusText(*owner));
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
        return Element::OnKeyEvent(key);
    }

    return KeyReply::Handled();
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

    // 조합 중에는 조합 범위에 밑줄, 아니면 선택 영역을 칠한다.
    const usize begin = composing_ ? compositionBegin_ : (caret_ < anchor_ ? caret_ : anchor_);
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

    const auto pos = label_->CaretPosition(caret_, false);
    const auto point = pos ? pos.Value() : Vec2{};
    const auto caret = label_->GetGeometry().MakeChild({1, label_->LineHeight()},
                                                       {1, 0, 0, 1, point.x, point.y}, {}, {});
    return list.TryAddBox({caret, {1, 1, 1, 1}, layer});
}

} // namespace rocks::ui
