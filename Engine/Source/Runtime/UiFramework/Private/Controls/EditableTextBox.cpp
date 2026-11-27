#include <UiFramework/Controls/EditableTextBox.h>

#include "EditableTextErrorPopup.h"

#include <UiCore/PaintList.h>
#include <UiCore/UiHostServices.h>
#include <UiFramework/Popups/MenuAnchor.h>
#include <UiFramework/Scrolling/ScrollBar.h>

namespace rocks::ui {

namespace {

// 오류 표지 글자다(UE SPopupErrorText의 HasErrorSymbol "!").
constexpr StringView ErrorMarkerGlyph = "!";
// Initialize 전 오류 표지를 셰이핑할 글꼴 크기다(편집기 기본과 같다).
constexpr float DefaultMarkerFontSize = 16.0f;
// 오류 팝업 안쪽 여백이다(UE SErrorText의 FMargin(3, 0) — SErrorText.cpp).
constexpr Vec4 ErrorPopupPadding{3, 0, 3, 0};

bool IsValidPadding(Vec4 padding) noexcept {
    return padding.IsFinite() && padding.x >= 0 && padding.y >= 0 && padding.z >= 0 &&
           padding.w >= 0;
}

// 표지 버튼 스타일이다 — 모든 상태가 오류 바탕이고 여백이 없다(UE SPopupErrorText의 콤보 버튼은
// 내용 여백 0, 바탕은 SErrorText "Box.Error").
ButtonStyle MarkerButtonStyle(const EditableTextBoxStyle& style) noexcept {
    const ColorRef white = ColorRef::FromTheme(ThemeSlot::White);
    return ButtonStyle{
        .normal = style.errorMarker,
        .hovered = style.errorMarker,
        .pressed = style.errorMarker,
        .disabled = style.errorMarker,
        .normalForeground = white,
        .hoveredForeground = white,
        .pressedForeground = white,
        .disabledForeground = white,
    };
}

} // namespace

EditableTextBox::EditableTextBox(Allocator& allocator, SharedPtr<TextShaper> shaper,
                                 Platform platform, bool multiline) noexcept
    : Element(allocator), allocator_(allocator), shaper_(Move(shaper)), platform_(platform),
      multiline_(multiline) {
}

EditableTextBox::~EditableTextBox() noexcept {
    // 편집기·막대·표지가 밖에서 붙잡혀 있어도 사라진 상자를 부르지 않게 한다(엔진 정리 — 사용자
    // hook 아님). 표지는 열린 팝업도 닫는다.
    if (editor_) {
        editor_->SetOnStateChanged({});
    }

    if (bar_) {
        bar_->SetOnScrolled({});
    }

    if (marker_) {
        marker_->DisconnectMenu();
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

    SharedPtr<ScrollBar> bar;
    if (multiline_) {
        auto scroll = SharedPtr<ScrollBar>::TryMake(allocator_, allocator_);
        if (!scroll) {
            return Result<void>::Failure(scroll.Error());
        }

        bar = Move(scroll.Value());
        // 넘칠 때만 보인다(22.7절 — UE AlwaysShowScrollbars 기본 false).
        bar->SetVisibility(Visibility::Collapsed);
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

    if (ready && bar) {
        ready = TryAddChild(bar);
        if (!ready) {
            (void)RemoveChild(editor);
        }
    }

    if (!ready) {
        return ready;
    }

    // 초점·읽기 전용이 바뀌면 바탕·전경을 다시 고른다. 편집기가 밖에 붙잡혀 상자보다 오래 살 수
    // 있으므로 상자 소멸자가 이 콜백을 끊는다.
    editor.SetOnStateChanged(
        Delegate<void()>::CreateLambda([this]() noexcept { OnEditorStateChanged(); }));
    editor_ = Move(made.Value());
    if (bar) {
        // 막대를 끌면 편집기 스크롤을 옮긴다(소멸자가 끊는다).
        bar->SetOnScrolled(Delegate<void(float)>::CreateLambda(
            [this](float fraction) noexcept { OnScrollBarMoved(fraction); }));
        bar_ = Move(bar);
    }

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

Result<SharedPtr<TextLabel>> EditableTextBox::MakeLabel(const Text& text, ColorRef color,
                                                        bool centered) noexcept {
    if (!shaper_) {
        return Result<SharedPtr<TextLabel>>::Failure(ErrorCode::NotReady);
    }

    auto label = SharedPtr<TextLabel>::TryMake(allocator_, allocator_);
    if (!label) {
        return Result<SharedPtr<TextLabel>>::Failure(label.Error());
    }

    const float size =
        editor_ && editor_->FontSize() > 0 ? editor_->FontSize() : DefaultMarkerFontSize;
    auto shaped = shaper_->Shape(*label.Value(), text, size);
    if (shaped) {
        shaped = label.Value()->SetLayoutOptions(
            0, centered ? text::TextAlignment::Center : text::TextAlignment::Left, false);
    }

    if (!shaped) {
        return Result<SharedPtr<TextLabel>>::Failure(shaped.Error());
    }

    label.Value()->SetColor(color);
    label.Value()->SetVisibility(Visibility::HitTestInvisible);
    return label;
}

Result<SharedPtr<MenuAnchor>> EditableTextBox::MakeMarker() noexcept {
    auto glyph = Text::Invariant(allocator_, ErrorMarkerGlyph);
    if (!glyph) {
        return Result<SharedPtr<MenuAnchor>>::Failure(glyph.Error());
    }

    auto label = MakeLabel(glyph.Value(), ColorRef::FromTheme(ThemeSlot::White), true);
    auto anchor = SharedPtr<MenuAnchor>::TryMake(allocator_, allocator_);
    if (!label || !anchor) {
        return Result<SharedPtr<MenuAnchor>>::Failure(!label ? label.Error() : anchor.Error());
    }

    MenuAnchor& marker = *anchor.Value();
    auto ready = marker.SetStyle(MarkerButtonStyle(style_));
    if (ready) {
        ready = marker.TryAddChild(label.Value());
    }

    if (!ready) {
        return Result<SharedPtr<MenuAnchor>>::Failure(ready.Error());
    }

    // UE SPopupErrorText: 초점을 받지 않는 콤보 버튼(IsFocusable(false)), 팝업은 아래
    // (MenuPlacement_BelowAnchor). 오류 팝업은 바깥 누름을 막지 않는다. 생성기는 상자를 raw로
    // 부른다 — 상자 소멸자가 DisconnectMenu로 끊는다.
    marker.SetFocusable(false);
    marker.SetPlacement(MenuPlacement::Below);
    marker.SetCatchOutsidePresses(false);
    marker.SetMenuFactory(
        MenuAnchor::MenuFactory::CreateRaw(this, &EditableTextBox::MakeErrorPopup));
    return anchor;
}

Result<SharedPtr<Element>> EditableTextBox::MakeErrorPopup() noexcept {
    if (!errorText_.IsValid()) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::NotReady);
    }

    auto label = MakeLabel(errorText_, style_.errorText.color, false);
    if (!label) {
        return Result<SharedPtr<Element>>::Failure(label.Error());
    }

    auto popup = SharedPtr<EditableTextErrorPopup>::TryMake(allocator_, allocator_,
                                                            style_.errorMarker, ErrorPopupPadding);
    if (!popup) {
        return Result<SharedPtr<Element>>::Failure(popup.Error());
    }

    auto added = popup.Value()->TryAddChild(label.Value());
    if (!added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    return Result<SharedPtr<Element>>::Success(Move(popup.Value()));
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

    const bool hasError = copy.Value().IsValid();
    if (hasError && editor_ && !shaper_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    SharedPtr<MenuAnchor> marker;
    if (hasError && editor_ && !marker_) {
        auto made = MakeMarker();
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        auto added = TryAddChild(made.Value());
        if (!added) {
            return added;
        }

        marker = Move(made.Value());
        popupOffset_ = {};
    }

    if (!hasError && marker_) {
        marker_->DisconnectMenu();
        (void)RemoveChild(*marker_);
        marker_ = {};
    }

    if (marker) {
        marker_ = Move(marker);
    }

    errorText_ = Move(copy.Value());
    InvalidateLayout();
    SyncErrorPopup();
    return Result<void>::Success();
}

void EditableTextBox::SyncErrorPopup() noexcept {
    if (!marker_) {
        return;
    }

    // UE SPopupErrorText::SetError — SetIsOpen(HasError, false). 열려 있으면 새 문구·스타일로 다시
    // 만든다. 팝업 호스트가 없는 트리(붙기 전 포함)면 열지 못한다 — 붙을 때 다시 연다
    // (OnUiHostServicesChanged).
    marker_->Close();
    if (errorText_.IsValid()) {
        (void)marker_->Open();
    }
}

void EditableTextBox::OnUiHostServicesChanged() noexcept {
    Element::OnUiHostServicesChanged();
    if (!marker_ || marker_->IsOpen() || popupQueued_ || !Parent()) {
        return;
    }

    const SharedPtr<UiHostServices> host = FindUiHostServicesRef();
    const SharedPtr<Element> self = Parent()->FindChild(this);
    if (!host || !self) {
        return;
    }

    // 붙는 통지는 트리 순회 안이다 — 루트 Overlay 자식 목록을 바꾸지 않게 프레임 시작으로 미룬다
    // (22.6절). 그 사이 버려진 상자는 약한 참조라 아무 일도 하지 않는다.
    auto queued = host->DeferFrameAction(
        Delegate<void()>::CreateLambda([weak = WeakPtr<Element>(self)]() noexcept {
            if (const SharedPtr<Element> box = weak.Lock()) {
                StaticElementCast<EditableTextBox>(*box).OpenQueuedPopup();
            }
        }));
    if (queued) {
        popupQueued_ = true;
    }
}

void EditableTextBox::OpenQueuedPopup() noexcept {
    popupQueued_ = false;
    if (marker_ && errorText_.IsValid() && !marker_->IsOpen()) {
        (void)marker_->Open();
    }
}

bool EditableTextBox::HasError() const noexcept {
    return errorText_.IsValid();
}

const Text& EditableTextBox::ErrorText() const noexcept {
    return errorText_;
}

MenuAnchor* EditableTextBox::ErrorMarker() const noexcept {
    return marker_.Get();
}

ScrollBar* EditableTextBox::VerticalScrollBar() const noexcept {
    return bar_.Get();
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
    const StyleBrush* brushes[] = {&style.normal, &style.hovered, &style.focused, &style.readOnly,
                                   &style.errorMarker};
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

    if (marker_) {
        auto ready = marker_->SetStyle(MarkerButtonStyle(style));
        if (!ready) {
            return ready;
        }
    }

    const bool popupStyleChanged =
        style_.errorMarker != style.errorMarker || style_.errorText != style.errorText;
    style_ = style;
    if (popupStyleChanged && marker_ && marker_->IsOpen()) {
        // 열린 팝업을 새 바탕·글자색으로 다시 만든다.
        SyncErrorPopup();
    }

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
    // 표지는 오른쪽 여백 안쪽에 정사각으로 놓인다(편집 칸이 그만큼 줄어든다 — 22.6절). 스크롤
    // 막대는 넘칠 때만 생기므로 원하는 크기에 넣지 않는다(UE 막대도 Collapsed면 자리가 없다).
    const float height = math::Max(editor.y, marker + style_.padding.y + style_.padding.w);
    return {editor.x + marker, height};
}

Result<void> EditableTextBox::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    if (!editor_) {
        return Result<void>::Success();
    }

    // 오른쪽은 편집 칸 | 막대 | 표지 순서다(UE SMultiLineEditableTextBox 가로 상자 — 22.7절). 뒤
    // 칸이 있으면 오른쪽 여백 하나를 둔다(편집기 내용 여백이 칸 사이 간격이 된다).
    const float marker = MarkerExtent();
    const float thickness = bar_ ? bar_->GetStyle().thickness : 0.0f;
    const auto editorWidth = [&](bool withBar) noexcept {
        const float trailing = marker + (withBar ? thickness : 0.0f);
        const float zone = trailing > 0 ? trailing + style_.padding.z : 0.0f;
        return math::Max(0.0f, geometry.size.x - zone);
    };

    bool showBar = false;
    if (bar_) {
        // 지난 배치의 폭으로 잰다. 막대가 보이던 좁은 폭에서 안 넘치면 넓은 폭에서도 안 넘치고,
        // 숨던 넓은 폭에서 넘치면 좁은 폭에서는 더 넘친다 — 한 번 재면 정해진다(결정적). 잰 폭이
        // 놓을 폭과 같으면 편집기 배치를 건너뛰어도 줄 배치가 그 폭에 남는다.
        const bool shown = bar_->GetVisibility() != Visibility::Collapsed;
        const float view = geometry.size.y - style_.padding.y - style_.padding.w;
        showBar = editor_->MeasureContentHeight(editorWidth(shown)) > view;
    }

    const float width = editorWidth(showBar);
    auto placed =
        editor_->ArrangeIfNeeded(GetGeometry().MakeChild({width, geometry.size.y}, {}, {}, {}));
    if (!placed) {
        return placed;
    }

    if (bar_) {
        bar_->SetVisibility(showBar ? Visibility::Visible : Visibility::Collapsed);
        if (showBar) {
            // 편집기 스크롤로 막대를 맞춘다(UE 막대 갱신 — SlateEditableTextLayout.cpp
            // 3648–3659행).
            const float content = editor_->ContentHeight();
            const float fraction =
                content > 0 ? math::Clamp(editor_->ViewHeight() / content, 0.0f, 1.0f) : 1.0f;
            const float offset = content > 0 ? editor_->ScrollOffset() / content : 0.0f;
            (void)bar_->SetState(offset, fraction);
            const float height =
                math::Max(0.0f, geometry.size.y - style_.padding.y - style_.padding.w);
            placed = bar_->ArrangeIfNeeded(GetGeometry().MakeChild(
                {thickness, height}, {1, 0, 0, 1, width, style_.padding.y}, {}, {}));
            if (!placed) {
                return placed;
            }
        }
    }

    if (!marker_) {
        return Result<void>::Success();
    }

    const float x = math::Max(0.0f, geometry.size.x - style_.padding.z - marker);
    const float y = math::Max(0.0f, (geometry.size.y - marker) * 0.5f);
    // 팝업은 표지 아래가 기준이다(UE BelowAnchor). 상자 아래 끝까지 내려 입력 글자를 가리지 않게
    // 한다(22.6절). 바뀔 때만 정한다(열린 팝업 층 배치를 무효화한다).
    const Vec2 offset{0, math::Max(0.0f, geometry.size.y - y - marker)};
    if (offset != popupOffset_) {
        popupOffset_ = offset;
        marker_->SetMenuOffset(offset);
    }

    return marker_->ArrangeIfNeeded(
        GetGeometry().MakeChild({marker, marker}, {1, 0, 0, 1, x, y}, {}, {}));
}

void EditableTextBox::OnScrollBarMoved(float fraction) noexcept {
    if (editor_) {
        editor_->SetScrollOffset(fraction * editor_->ContentHeight());
    }
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
    // 표지 바탕은 표지 버튼이 그린다.
    return list.TryAddBrush(GetGeometry(),
                            CurrentBackground().Resolve(list.Theme(), list.ColorContext()),
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
