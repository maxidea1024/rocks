#include <UiFramework/Controls/EditableTextBox.h>

#include <UiCore/PaintList.h>

namespace rocks::ui {

namespace {

// 오류 표지 글자다(UE SPopupErrorText의 HasErrorSymbol "!").
constexpr StringView ErrorMarkerGlyph = "!";
// Initialize 전 오류 표지를 셰이핑할 글꼴 크기다(편집기 기본과 같다).
constexpr float DefaultMarkerFontSize = 16.0f;

bool IsValidPadding(Vec4 padding) noexcept {
    return padding.IsFinite() && padding.x >= 0 && padding.y >= 0 && padding.z >= 0 &&
           padding.w >= 0;
}

} // namespace

EditableTextBox::EditableTextBox(Allocator& allocator, SharedPtr<TextShaper> shaper,
                                 Platform platform, bool multiline) noexcept
    : Element(allocator), allocator_(allocator), shaper_(Move(shaper)), platform_(platform),
      multiline_(multiline) {
}

EditableTextBox::~EditableTextBox() noexcept {
    // 편집기가 밖에서 붙잡혀 있어도 사라진 상자를 부르지 않게 한다(엔진 정리 — 사용자 hook 아님).
    if (editor_) {
        editor_->SetOnStateChanged({});
    }
}

Result<void> EditableTextBox::Initialize(StringView text, float fontSize) noexcept {
    if (editor_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto made =
        SharedPtr<EditableText>::TryMake(allocator_, allocator_, shaper_, platform_, multiline_);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    EditableText& editor = *made.Value();
    auto ready = editor.SetContentPadding(style_.padding);
    if (ready) {
        ready = editor.SetStyle(style_.text);
    }

    if (ready) {
        ready = editor.Initialize(text, fontSize);
    }

    if (ready) {
        ready = TryAddChild(made.Value());
    }

    if (!ready) {
        return ready;
    }

    // 초점·읽기 전용이 바뀌면 바탕·전경을 다시 고른다. 편집기가 밖에 붙잡혀 상자보다 오래 살 수
    // 있으므로 상자 소멸자가 이 콜백을 끊는다.
    editor.SetOnStateChanged(
        Delegate<void()>::CreateLambda([this]() noexcept { OnEditorStateChanged(); }));
    editor_ = Move(made.Value());
    // Initialize 전에 정한 오류 표지를 만든다.
    if (errorText_.IsValid()) {
        Text pending = Move(errorText_);
        errorText_ = {};
        return SetError(pending);
    }

    return Result<void>::Success();
}

bool EditableTextBox::IsInitialized() const noexcept {
    return editor_.Get() != nullptr;
}

EditableText& EditableTextBox::Editor() const noexcept {
    ROCKS_ASSERT(editor_);
    return *editor_;
}

Result<void> EditableTextBox::SetError(const Text& message) noexcept {
    auto copy = message.IsValid() ? message.DeepClone(allocator_) : Result<Text>::Success({});
    if (!copy) {
        return Result<void>::Failure(copy.Error());
    }

    // 빈 원문의 불변 글자는 지우기와 같다.
    TextPersistentView view;
    if (copy.Value().TryGetPersistentForm(view) && view.form == TextPersistentForm::Invariant &&
        view.source.IsEmpty()) {
        copy.Value() = {};
    }

    SharedPtr<TextLabel> marker;
    if (copy.Value().IsValid() && editor_) {
        if (!shaper_) {
            return Result<void>::Failure(ErrorCode::NotReady);
        }

        auto glyph = Text::Invariant(allocator_, ErrorMarkerGlyph);
        auto label = SharedPtr<TextLabel>::TryMake(allocator_, allocator_);
        if (!glyph || !label) {
            return Result<void>::Failure(!glyph ? glyph.Error() : label.Error());
        }

        const float size = editor_->FontSize() > 0 ? editor_->FontSize() : DefaultMarkerFontSize;
        auto shaped = shaper_->Shape(*label.Value(), glyph.Value(), size);
        if (shaped) {
            shaped = label.Value()->SetLayoutOptions(0, text::TextAlignment::Center, false);
        }

        if (!shaped) {
            return shaped;
        }

        label.Value()->SetColor(ColorRef::FromTheme(ThemeSlot::White));
        label.Value()->SetVisibility(Visibility::HitTestInvisible);
        marker = Move(label.Value());
    }

    if (marker_) {
        (void)RemoveChild(*marker_);
        marker_ = {};
    }

    if (marker) {
        auto added = TryAddChild(marker);
        if (!added) {
            return added;
        }

        marker_ = Move(marker);
    }

    errorText_ = Move(copy.Value());
    InvalidateLayout();
    return Result<void>::Success();
}

bool EditableTextBox::HasError() const noexcept {
    return errorText_.IsValid();
}

const Text& EditableTextBox::ErrorText() const noexcept {
    return errorText_;
}

Result<void> EditableTextBox::SetStyle(const EditableTextBoxStyle& style) noexcept {
    auto applied = ApplyStyle(style);
    if (applied) {
        styleExplicit_ = true;
    }

    return applied;
}

const EditableTextBoxStyle& EditableTextBox::GetStyle() const noexcept {
    return style_;
}

void EditableTextBox::ApplyStyles(const UiHostServices& host) noexcept {
    if (!styleExplicit_) {
        // 활성 집합에 키가 없으면(등록부 없음 포함) 내장 기본값으로 돌린다(20.11절).
        const EditableTextBoxStyle* style =
            host.FindStyle<EditableTextBoxStyle>("NormalEditableTextBox");
        (void)ApplyStyle(style ? *style : DefaultEditableTextBoxStyle());
    }
}

Result<void> EditableTextBox::ApplyStyle(const EditableTextBoxStyle& style) noexcept {
    const ThemeColors& theme = DefaultThemeColors();
    const StyleBrush* brushes[] = {&style.normal, &style.hovered, &style.focused, &style.readOnly};
    for (const StyleBrush* brush : brushes) {
        if (!brush->Resolve(theme, {}).IsValid()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (!IsValidPadding(style.padding)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (style_ == style) {
        return Result<void>::Success();
    }

    if (editor_) {
        auto ready = editor_->SetContentPadding(style.padding);
        if (ready) {
            ready = editor_->SetStyle(style.text);
        }

        if (!ready) {
            return ready;
        }
    }

    style_ = style;
    InvalidateLayout();
    InvalidatePaint();
    return Result<void>::Success();
}

float EditableTextBox::MarkerExtent() const noexcept {
    return marker_ ? marker_->GetDesiredSize().y : 0.0f;
}

Vec2 EditableTextBox::ComputeDesiredSize(float) const noexcept {
    const float marker = MarkerExtent();
    if (!editor_) {
        return {style_.padding.x + style_.padding.z + marker,
                style_.padding.y + style_.padding.w + math::Max(DefaultMarkerFontSize, marker)};
    }

    const Vec2 editor = editor_->GetDesiredSize();
    // 표지는 오른쪽 여백 안쪽에 정사각으로 놓인다(편집 칸이 그만큼 줄어든다 — 22.6절).
    const float height = math::Max(editor.y, marker + style_.padding.y + style_.padding.w);
    return {editor.x + marker, height};
}

Result<void> EditableTextBox::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    if (!editor_) {
        return Result<void>::Success();
    }

    const float marker = MarkerExtent();
    const float zone = marker > 0 ? marker + style_.padding.z : 0.0f;
    const Vec2 editorSize{math::Max(0.0f, geometry.size.x - zone), geometry.size.y};
    auto placed = editor_->ArrangeIfNeeded(GetGeometry().MakeChild(editorSize, {}, {}, {}));
    if (!placed || !marker_) {
        return placed;
    }

    const float x = math::Max(0.0f, geometry.size.x - style_.padding.z - marker);
    const float y = math::Max(0.0f, (geometry.size.y - marker) * 0.5f);
    return marker_->ArrangeIfNeeded(
        GetGeometry().MakeChild({marker, marker}, {1, 0, 0, 1, x, y}, {}, {}));
}

const StyleBrush& EditableTextBox::CurrentBackground() const noexcept {
    // UE SEditableTextBox::GetBorderImage — 읽기 전용 > 초점 > 올림 > 보통.
    if (editor_ && editor_->IsReadOnly()) {
        return style_.readOnly;
    }

    if (editor_ && editor_->IsFocused()) {
        return style_.focused;
    }

    return hoverCount_ > 0 ? style_.hovered : style_.normal;
}

Result<void> EditableTextBox::OnPaint(PaintList& list, int32 layer) const noexcept {
    const StyleColorContext context = list.ColorContext();
    auto painted =
        list.TryAddBrush(GetGeometry(), CurrentBackground().Resolve(list.Theme(), context),
                         LinearColor{1, 1, 1, 1}, layer);
    if (!painted || !marker_) {
        return painted;
    }

    return list.TryAddBrush(marker_->GetGeometry(),
                            style_.errorMarker.Resolve(list.Theme(), context),
                            LinearColor{1, 1, 1, 1}, layer);
}

ColorRef EditableTextBox::PaintForegroundColor(bool) const noexcept {
    return CurrentForeground();
}

ColorRef EditableTextBox::CurrentForeground() const noexcept {
    // UE SEditableTextBox::DetermineForegroundColor(SEditableTextBox.cpp) — 읽기 전용 > 초점 >
    // 보통.
    if (editor_ && editor_->IsReadOnly()) {
        return style_.readOnlyForeground;
    }

    return editor_ && editor_->IsFocused() ? style_.focusedForeground : style_.foreground;
}

SharedPtr<Element> EditableTextBox::FocusDelegate() const noexcept {
    return editor_;
}

void EditableTextBox::OnMouseEnter(uint32 user) noexcept {
    Element::OnMouseEnter(user);
    if (++hoverCount_ == 1) {
        InvalidatePaint();
    }
}

void EditableTextBox::OnMouseLeave(uint32 user) noexcept {
    Element::OnMouseLeave(user);
    if (hoverCount_ > 0 && --hoverCount_ == 0) {
        InvalidatePaint();
    }
}

void EditableTextBox::OnEditorStateChanged() noexcept {
    // 바탕과 물려주는 전경이 바뀐다 — 그리기 무효화가 자식의 물려받은 전경도 다시 계산한다.
    InvalidatePaint();
}

} // namespace rocks::ui
