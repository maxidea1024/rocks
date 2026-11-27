#include <UiFramework/Controls/EditableText.h>

#include <Core/Assert.h>

#include <UiCore/TextInputPositions.h>

namespace rocks::ui {

namespace {

usize SmallerOffset(usize a, usize b) noexcept {
    return a < b ? a : b;
}

// 되돌리기 기록 상한이다(UE EditableTextDefs::MaxUndoLevels — TextEditHelper.h 15행).
constexpr usize MaxUndoSteps = 99;
// caret 폭은 줄 높이의 이 비율을 [최소, 최대] px로 자른 값이다(UE CaretWidthPercent·
// FTextEditHelper::CalculateCaretWidth — TextEditHelper.cpp 15–19행).
constexpr float CaretWidthPercent = 0.08f;
constexpr float MinCaretWidth = 1.0f;
constexpr float MaxCaretWidth = 2.0f;
// 조합 밑줄 두께(px)다.
constexpr float CompositionUnderlineThickness = 1.0f;
// 글자로 넣지 않는 제어 문자: U+0000–U+001F와 DEL(U+007F)이다.
constexpr uint32 FirstPrintableCodePoint = 0x20;
constexpr uint32 DeleteCodePoint = 0x7F;

} // namespace

EditableText::EditableText(Allocator& allocator, SharedPtr<TextShaper> shaper, Platform platform,
                           bool multiline) noexcept
    : Element(allocator), allocator_(allocator), undo_(allocator), redo_(allocator),
      shaper_(Move(shaper)), text_(allocator), focusText_(allocator), platform_(platform),
      preview_(allocator), multiline_(multiline) {
    SetClipToBounds(true);
}

EditableText::~EditableText() noexcept {
    // 서비스가 남은 기록을 이 문맥에 적용하지 않게 조합을 먼저 버린 뒤 끈다(19.2절).
    if (auto service = Move(textInput_)) {
        composing_ = false;
        service->CancelComposition(*this);
        service->Deactivate(*this);
    }
}

Result<void> EditableText::Initialize(StringView text, float size) noexcept {
    if (label_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (!math::IsFinite(size) || size <= 0 ||
        (!multiline_ && (text.Contains("\n") || text.Contains("\r")))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto& allocator = allocator_;
    auto child = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!child) {
        return Result<void>::Failure(child.Error());
    }

    label_ = Move(child.Value());
    auto added = TryAddChild(label_);
    if (!added) {
        return added;
    }

    auto shaped = Shape(text, size, password_);
    if (shaped) {
        shaped = text_.TryAssign(text);
    }

    if (shaped) {
        MoveCaret(text.ByteCount(), false);
    }

    // Initialize 전에 정한 힌트를 만든다.
    if (shaped && hintText_.IsValid()) {
        auto hint = AddHint(hintText_, fontSize_);
        if (!hint) {
            return Result<void>::Failure(hint.Error());
        }

        PublishHint(Move(hint.Value()));
    }

    return shaped;
}

Result<void> EditableText::SetText(StringView text) noexcept {
    if (!multiline_ && (text.Contains("\n") || text.Contains("\r"))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (text == text_.View()) {
        return Result<void>::Success();
    }

    String copy(allocator_);
    auto assigned = copy.TryAssign(text);
    if (assigned) {
        assigned = ExchangeText(copy);
    }

    if (assigned) {
        ClearHistory();
    }

    return assigned;
}

StringView EditableText::GetText() const noexcept {
    return text_.View();
}

Result<void> EditableText::ExchangeText(String& next) noexcept {
    if (label_) {
        auto shaped = Shape(next.View(), fontSize_, password_);
        if (!shaped) {
            return shaped;
        }
    }

    text_.Swap(next);
    if (label_) {
        MoveCaret(text_.ByteCount(), false);
    }

    return Result<void>::Success();
}

Result<void> EditableText::SetPassword(bool password) noexcept {
    if (password == password_) {
        return Result<void>::Success();
    }

    if (label_) {
        auto shaped = Shape(text_.View(), fontSize_, password);
        if (!shaped) {
            return shaped;
        }

        MoveCaret(text_.ByteCount(), false);
        ClearHistory();
    }

    password_ = password;
    // 비밀번호 입력란은 IME를 끈다(19.2절).
    if (textInput_) {
        textInput_->Activate(*this);
    }

    return Result<void>::Success();
}

bool EditableText::IsPassword() const noexcept {
    return password_;
}

void EditableText::SetReadOnly(bool readOnly) noexcept {
    if (readOnly) {
        CancelComposition();
    }

    if (readOnly_ != readOnly) {
        readOnly_ = readOnly;
        // IME 허용이 바뀌었다(읽기 전용은 IME를 끈다 — 19.2절).
        if (textInput_) {
            textInput_->Activate(*this);
        }

        NotifyStateChanged();
    }
}

void EditableText::SetSelectAllTextWhenFocused(bool enabled) noexcept {
    selectAllOnFocus_ = enabled;
}

void EditableText::SetSelectAllTextOnCommit(bool enabled) noexcept {
    selectAllOnCommit_ = enabled;
}

void EditableText::SetRevertTextOnEscape(bool enabled) noexcept {
    if (revertOnEscape_ != enabled) {
        revertOnEscape_ = enabled;
        RememberFocusText();
    }
}

bool EditableText::IsMultiline() const noexcept {
    return multiline_;
}

bool EditableText::IsFocused() const noexcept {
    return focused_;
}

Result<void> EditableText::SetContentPadding(Vec4 padding) noexcept {
    if (!padding.IsFinite() || padding.x < 0 || padding.y < 0 || padding.z < 0 || padding.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (padding_ != padding) {
        padding_ = padding;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

Vec4 EditableText::ContentPadding() const noexcept {
    return padding_;
}

Result<void> EditableText::SetStyle(const EditableTextStyle& style) noexcept {
    auto applied = ApplyStyle(style);
    if (applied) {
        styleExplicit_ = true;
    }

    return applied;
}

const EditableTextStyle& EditableText::GetStyle() const noexcept {
    return style_;
}

void EditableText::ApplyStyles(const UiHostServices& host) noexcept {
    if (!styleExplicit_) {
        // 활성 집합에 키가 없으면(등록부 없음 포함) 내장 기본값으로 돌린다(20.11절).
        const EditableTextStyle* style = host.FindStyle<EditableTextStyle>("NormalEditableText");
        (void)ApplyStyle(style ? *style : DefaultEditableTextStyle());
    }
}

Result<void> EditableText::ApplyStyle(const EditableTextStyle& style) noexcept {
    if (!math::IsFinite(style.hintOpacity) || style.hintOpacity < 0 || style.hintOpacity > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (style_ != style) {
        style_ = style;
        if (hint_) {
            (void)hint_->SetRenderOpacity(style_.hintOpacity);
        }

        InvalidatePaint();
    }

    return Result<void>::Success();
}

void EditableText::SetOnStateChanged(Delegate<void()>&& callback) noexcept {
    stateChanged_ = Move(callback);
}

void EditableText::NotifyStateChanged() noexcept {
    stateChanged_.ExecuteIfBound();
}

void EditableText::SetOnTextChanged(Delegate<void(StringView)>&& callback) noexcept {
    changed_ = Move(callback);
}

void EditableText::SetOnTextCommitted(
    Delegate<void(StringView, TextCommitReason)>&& callback) noexcept {
    committed_ = Move(callback);
}

void EditableText::NotifyChanged(StringView text) noexcept {
    changed_.ExecuteIfBound(text);
}

void EditableText::Commit(TextCommitReason reason) noexcept {
    if (!committed_.IsBound()) {
        return;
    }

    // 수신자가 SetText를 불러도 전달한 문자열을 계속 읽을 수 있게 보존한다.
    auto snapshot = text_.TryClone(allocator_);
    if (snapshot) {
        committed_.ExecuteIfBound(snapshot.Value().View(), reason);
    }
}

Result<void> EditableText::Shape(StringView text, float size, bool password) noexcept {
    // 확정 글자로 셰이핑한다. 조합 중이면 앱이 글자를 바꾼 것이다(바인딩·되돌리기 — 19.2절).
    AbandonComposition();
    return ShapeText(text, size, password);
}

void EditableText::AbandonComposition() noexcept {
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

Result<void> EditableText::ShapeText(StringView text, float size, bool password) noexcept {
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
        auto value = Text::Invariant(allocator_, text);
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

Result<void> EditableText::ShapePassword(StringView text, float size) noexcept {
    // 실제 비밀번호를 셰이퍼에 전달하지 않는다. 가림 글리프 하나를 셰이핑하고
    // 원문 UTF-8 경계에 대응시켜 마우스 선택과 삭제가 같은 위치를 사용하게 한다.
    auto& allocator = allocator_;
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

TextLabel& EditableText::Label() const noexcept {
    ROCKS_ASSERT(label_);
    return *label_;
}

Vec2 EditableText::ComputeDesiredSize(float) const noexcept {
    const Vec2 inset{padding_.x + padding_.z, padding_.y + padding_.w};
    // Initialize 전에는 빈 편집기다.
    if (!label_) {
        return {inset.x, fontSize_ + inset.y};
    }

    Vec2 text = label_->GetDesiredSize();
    if (hint_ && label_->GetText().IsEmpty()) {
        text.x = math::Max(text.x, hint_->GetDesiredSize().x);
        text.y = math::Max(text.y, hint_->GetDesiredSize().y);
    }

    // caret 폭만큼 오른쪽을 더 둔다(UE가 caret 폭을 여백에 더한다 — SlateEditableTextLayout.cpp
    // 3717–3718행).
    const float caret =
        math::Clamp(CaretWidthPercent * math::Max(fontSize_, text.y), MinCaretWidth, MaxCaretWidth);
    return {text.x + caret + inset.x, math::Max(fontSize_, text.y) + inset.y};
}

Result<void> EditableText::Arrange(const Geometry& geometry) noexcept {
    if (!label_) {
        SetGeometry(geometry);
        return Result<void>::Success();
    }

    const float width = math::Max(1.0f, geometry.size.x - padding_.x - padding_.z);
    const float height = math::Max(1.0f, geometry.size.y - padding_.y - padding_.w);
    SetGeometry(geometry);
    const Vec2 area{width, height};
    if (hint_ && label_->GetText().IsEmpty()) {
        auto placed = label_->CopyParagraphTo(*hint_);
        if (placed) {
            placed = hint_->ArrangeIfNeeded(
                GetGeometry().MakeChild(area, {1, 0, 0, 1, padding_.x, padding_.y}, {}, {}));
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

    const AffineTransform2D offset{1, 0, 0, 1, padding_.x - scroll_.x, padding_.y - scroll_.y};
    auto arranged = label_->ArrangeIfNeeded(GetGeometry().MakeChild(area, offset, {}, {}));
    if (arranged && textInput_) {
        // 글자 위치가 정해졌다 — 후보 창을 caret으로 옮긴다.
        textInput_->NotifyLayoutChanged(*this);
    }

    return arranged;
}

const StyleBrush* EditableText::FocusOutlineBrush() const noexcept {
    return nullptr;
}

bool EditableText::SupportsKeyboardFocus() const noexcept {
    return true;
}

void EditableText::OnFocusReceived(uint32 user) noexcept {
    focused_ = true;
    focusUser_ = user;
    RememberFocusText();
    if (selectAllOnFocus_) {
        keepPointerSelection_ = dragging_;
        SelectAllText();
    }

    textInput_ = FindTextInputService();
    if (textInput_) {
        textInput_->Activate(*this);
    }

    chord_ = {};
    InvalidatePaint();
    NotifyStateChanged();
}

void EditableText::OnFocusLost(uint32 user) noexcept {
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
    NotifyStateChanged();
    // 서비스가 만들던 글자를 확정해 넣은 뒤 끈다(19.2절). Commit(FocusLost)이 그 글자를 본다.
    if (auto service = Move(textInput_)) {
        service->Deactivate(*this);
    }

    EndComposition();
    Commit(TextCommitReason::FocusLost);
}

void EditableText::MoveCaret(usize pos, bool extend) noexcept {
    if (!label_) {
        return;
    }

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

usize EditableText::Adjacent(bool forward) const noexcept {
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

void EditableText::Replace(usize begin, usize end, StringView insertion) noexcept {
    if (readOnly_ || (begin == end && insertion.IsEmpty())) {
        return;
    }

    const StringView before = text_.View();
    auto left = before.TryLeftBytes(begin);
    auto right = before.TryMidBytes(end);
    if (!left || !right) {
        return;
    }

    StringBuilder<> result(allocator_);
    if (!result.TryAppend(left.Value()) || !result.TryAppend(insertion)) {
        return;
    }

    const usize pos = result.View().ByteCount();
    if (!result.TryAppend(right.Value())) {
        return;
    }

    auto& allocator = allocator_;
    String next(allocator);
    if (!next.TryAssign(result.View()) ||
        (!password_ && !undo_.TryEnsureCapacity(undo_.Size() + 1))) {
        return;
    }

    const usize previousAnchor = anchor_;
    const usize previousCaret = caret_;
    if (!ExchangeText(next)) {
        return;
    }

    if (!password_) {
        if (undo_.Size() == MaxUndoSteps) {
            (void)undo_.TryRemoveAt(0);
        }

        (void)undo_.TryPushBack({Move(next), previousAnchor, previousCaret});
        redo_.Reset();
    }

    MoveCaret(pos, false);
    // 문자열은 콜백에서 다시 편집될 수 있다. 이 호출이 소유한 스냅샷을 전달한다.
    NotifyChanged(result.View());
}

void EditableText::CancelComposition() noexcept {
    if (composing_) {
        AbandonComposition();
        (void)ShapeText(text_.View(), fontSize_, password_);
    }
}

Result<void> EditableText::AppendInsertion(StringView text, StringBuilder<>& out) const noexcept {
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

void EditableText::Paste(usize begin, usize end, StringView text) noexcept {
    StringBuilder<> clean(allocator_);
    if (!text.IsEmpty() && AppendInsertion(text, clean)) {
        Replace(begin, end, clean.View());
    }
}

usize EditableText::ByteOffset(uint32 index) const noexcept {
    return text_input::ToByteOffset(label_->GetText(), index);
}

bool EditableText::IsReadOnly() const noexcept {
    return readOnly_;
}

bool EditableText::AllowsInputMethod() const noexcept {
    return !readOnly_ && !password_;
}

bool EditableText::IsComposing() const noexcept {
    return composing_;
}

uint32 EditableText::GetTextLength() const noexcept {
    return text_input::Utf16Length(label_->GetText());
}

void EditableText::GetSelection(uint32& begin, uint32& length) const noexcept {
    const StringView text = label_->GetText();
    const uint32 first = text_input::ToUtf16Index(text, anchor_ < caret_ ? anchor_ : caret_);
    const uint32 last = text_input::ToUtf16Index(text, anchor_ < caret_ ? caret_ : anchor_);
    begin = first;
    length = last - first;
}

void EditableText::SetSelection(uint32 begin, uint32 length) noexcept {
    const usize first = ByteOffset(begin);
    const usize last = ByteOffset(begin + length);
    MoveCaret(first, false);
    MoveCaret(last, true);
}

Result<void> EditableText::GetText(uint32 begin, uint32 length,
                                   Array<char16_t>& out) const noexcept {
    const StringView text = label_->GetText();
    const usize first = ByteOffset(begin);
    const usize last = ByteOffset(begin + length);
    return text_input::AppendUtf16(text.SliceBytes(first, last - first), out);
}

void EditableText::SetText(uint32 begin, uint32 length, Span<const char16_t> text) noexcept {
    const usize first = ByteOffset(begin);
    const usize last = ByteOffset(begin + length);
    StringBuilder<> utf8(allocator_);
    StringBuilder<> insertion(allocator_);
    if (!text_input::AppendUtf8(text, utf8) || !AppendInsertion(utf8.View(), insertion)) {
        return;
    }

    if (!composing_) {
        // 조합 밖의 바꾸기(TSF 재변환 등)는 편집 하나다.
        Replace(first, last, insertion.View());
        return;
    }

    // 미리보기 층만 바꾼다(위젯 글자·이력·변경 알림 없음 — 19.2절).
    const StringView current = preview_.View();
    StringBuilder<> next(allocator_);
    if (!next.TryAppend(current.SliceBytes(0, first)) || !next.TryAppend(insertion.View()) ||
        !next.TryAppend(current.SliceBytes(last, current.ByteCount() - last)) ||
        !preview_.TryAssign(next.View())) {
        return;
    }

    if (ShapeText(preview_.View(), fontSize_, password_)) {
        const usize end = preview_.ByteCount();
        anchor_ = SmallerOffset(anchor_, end);
        caret_ = SmallerOffset(caret_, end);
        InvalidateArrangement();
    }
}

Optional<uint32> EditableText::GetCharacterIndexFromPoint(Vec2 window) const noexcept {
    AffineTransform2D inverse;
    if (!label_->GetGeometry().render.TryInverse(inverse)) {
        return {};
    }

    return text_input::ToUtf16Index(label_->GetText(), label_->HitText(inverse.Apply(window)));
}

bool EditableText::GetTextBounds(uint32 begin, uint32 length, Box2<float>& window) const noexcept {
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

bool EditableText::GetScreenBounds(Box2<float>& window) const noexcept {
    const auto& geometry = GetGeometry();
    const Vec2 a = geometry.render.Apply({0, 0});
    const Vec2 b = geometry.render.Apply(geometry.size);
    window = {{math::Min(a.x, b.x), math::Min(a.y, b.y)},
              {math::Max(a.x, b.x), math::Max(a.y, b.y)}};
    return true;
}

void EditableText::BeginComposition() noexcept {
    if (composing_ || !preview_.TryAssign(label_->GetText())) {
        return;
    }

    composing_ = true;
    preCompositionAnchor_ = anchor_;
    preCompositionCaret_ = caret_;
    compositionBegin_ = compositionEnd_ = anchor_ < caret_ ? anchor_ : caret_;
}

void EditableText::UpdateCompositionRange(uint32 begin, uint32 length) noexcept {
    compositionBegin_ = ByteOffset(begin);
    compositionEnd_ = ByteOffset(begin + length);
    InvalidatePaint();
}

void EditableText::EndComposition() noexcept {
    if (!composing_) {
        return;
    }

    // 미리보기를 한 번 확정한다: 글자 교체, 편집 이력 하나(조합 전 선택), 변경 알림 한 번.
    composing_ = false;
    const usize anchor = anchor_;
    const usize caret = caret_;
    if (readOnly_ || preview_.View() == text_.View()) {
        (void)ShapeText(text_.View(), fontSize_, password_);
        const usize end = text_.ByteCount();
        MoveCaret(SmallerOffset(anchor, end), false);
        MoveCaret(SmallerOffset(caret, end), true);
        preview_.Clear();
        return;
    }

    auto& allocator = allocator_;
    String next(allocator);
    auto snapshot = preview_.TryClone(allocator);
    if (!snapshot || !next.TryAssign(preview_.View()) ||
        (!password_ && !undo_.TryEnsureCapacity(undo_.Size() + 1)) || !ExchangeText(next)) {
        // 확정하지 못했다(메모리 부족) — 조합 전 글자로 돌린다.
        (void)ShapeText(text_.View(), fontSize_, password_);
        MoveCaret(SmallerOffset(preCompositionCaret_, text_.ByteCount()), false);
        preview_.Clear();
        return;
    }

    if (!password_) {
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
    NotifyChanged(snapshot.Value().View());
}

void EditableText::ClearHistory() noexcept {
    undo_.Reset();
    redo_.Reset();
}

void EditableText::RestoreHistory(bool redo) noexcept {
    auto& from = redo ? redo_ : undo_;
    auto& to = redo ? undo_ : redo_;
    if (readOnly_ || password_ || from.IsEmpty() || !to.TryEnsureCapacity(to.Size() + 1)) {
        return;
    }

    const usize previousAnchor = anchor_;
    const usize previousCaret = caret_;
    auto& state = from[from.Size() - 1];
    auto snapshot = state.text.TryClone(allocator_);
    if (!snapshot || !ExchangeText(state.text)) {
        return;
    }

    MoveCaret(state.anchor, false);
    MoveCaret(state.caret, true);
    (void)to.TryPushBack({Move(state.text), previousAnchor, previousCaret});
    (void)from.TryPopBack();
    NotifyChanged(snapshot.Value().View());
}

bool EditableText::OnPreviewKeyChord(uint32 user, const input::InputEvent&,
                                     KeyChord chord) noexcept {
    if (focused_ && focusUser_ == user) {
        chord_ = chord;
    }

    return false;
}

KeyReply EditableText::OnKeyEvent(const KeyEvent& key) noexcept {
    const uint32 user = key.user;
    const input::InputEvent& event = key.input;
    if (!focused_ || focusUser_ != user) {
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
            StringBuilder<> value(allocator_);
            if (value.TryAppendCodePoint(event.codePoint)) {
                Replace(begin, end, value.View());
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
            if (!readOnly_) {
                String clipboard(allocator_);
                if (platform_.readClipboardText && platform_.readClipboardText(clipboard)) {
                    Paste(begin, end, clipboard.View());
                }
            }
        } else if (!password_ && begin != end && (event.key == Key::C || !readOnly_)) {
            const auto selection = text_.View().SliceBytes(begin, end - begin);
            if (platform_.writeClipboardText && platform_.writeClipboardText(selection) &&
                event.key == Key::X) {
                Replace(begin, end, {});
            }
        }
        break;
    }
    case Key::Z:
    case Key::Y:
        if (!command) {
            return Element::OnKeyEvent(key);
        }

        RestoreHistory(event.key == Key::Y || extend);
        break;
    case Key::A:
        if (!command) {
            return Element::OnKeyEvent(key);
        }

        anchor_ = 0;
        MoveCaret(text_.ByteCount(), true);
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
        Replace(begin == end ? Adjacent(false) : begin, end, {});
        break;
    case Key::Delete:
        Replace(begin, begin == end ? Adjacent(true) : end, {});
        break;
    case Key::Escape:
        return KeyReply::FromHandled(RevertToFocusText());
    case Key::Enter:
    case Key::NumpadEnter:
        if (multiline_) {
            Replace(begin, end, "\n");
        } else if (!event.repeat) {
            RememberFocusText();
            if (selectAllOnCommit_) {
                SelectAllText();
            }

            Commit(TextCommitReason::Enter);
        }
        break;
    default:
        return Element::OnKeyEvent(key);
    }

    return KeyReply::Handled();
}

usize EditableText::PointerOffset(Vec2 point) const noexcept {
    AffineTransform2D inverse;
    return label_->GetGeometry().render.TryInverse(inverse) ? label_->HitText(inverse.Apply(point))
                                                            : caret_;
}

PointerReply EditableText::OnPointerEvent(const PointerEvent& event) noexcept {
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

void EditableText::OnPointerCaptureLost(PointerSource source, uint32 user,
                                        uint32 pointer) noexcept {
    if (source == dragSource_ && user == dragUser_ && pointer == dragPointer_) {
        dragging_ = false;
    }
}

Result<void> EditableText::OnPaint(PaintList& list, int32 layer) const noexcept {
    if (!focused_ || (!composing_ && caret_ == anchor_)) {
        return Result<void>::Success();
    }

    // 조합 중에는 조합 범위에 밑줄, 아니면 선택 영역을 칠한다.
    const usize begin = composing_ ? compositionBegin_ : (caret_ < anchor_ ? caret_ : anchor_);
    const usize end = composing_ ? compositionEnd_ : (anchor_ > caret_ ? anchor_ : caret_);
    // 선택 바탕은 스타일 selection(UE SelectionBackgroundColor), 조합 밑줄은 스타일
    // composition이다.
    const LinearColor color = (composing_ ? style_.composition : style_.selection)
                                  .Resolve(list.Theme(), list.ColorContext());
    const auto& layout = label_->Layout();
    for (const auto& line : layout.Lines()) {
        for (usize i = 0; i < line.visualCount; ++i) {
            const auto& visual = layout.VisualClusters()[line.firstVisual + i];
            const auto cluster = label_->ShapedClusters()[visual.logicalIndex];
            if (begin < cluster.byteEnd && cluster.byteEnd <= end && visual.advance > 0) {
                const float thickness = CompositionUnderlineThickness;
                const auto box = label_->GetGeometry().MakeChild(
                    {visual.advance, composing_ ? thickness : line.height},
                    {1, 0, 0, 1, visual.x,
                     composing_ ? line.y + label_->LineHeight() - thickness : line.y},
                    {}, {});
                auto added = list.TryAddBox({box, color, layer});
                if (!added) {
                    return added;
                }
            }
        }
    }

    return Result<void>::Success();
}

Result<void> EditableText::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    if (!focused_) {
        return Result<void>::Success();
    }

    const auto pos = label_->CaretPosition(caret_, false);
    const auto point = pos ? pos.Value() : Vec2{};
    const float height = label_->LineHeight();
    const float width = math::Clamp(CaretWidthPercent * height, MinCaretWidth, MaxCaretWidth);
    const auto caret =
        label_->GetGeometry().MakeChild({width, height}, {1, 0, 0, 1, point.x, point.y}, {}, {});
    return list.TryAddBox({caret, style_.caret.Resolve(list.Theme(), list.ColorContext()), layer});
}

} // namespace rocks::ui
