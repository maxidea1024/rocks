#include <UiFramework/Controls/EditableText.h>

#include <UiCore/InputMetrics.h>
#include <UiCore/UiHostServices.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Menus/MenuBuilder.h>
#include <UiFramework/Menus/MenuContext.h>
#include <UiFramework/Popups/MenuAnchor.h>

// 편집 문맥 메뉴·오른쪽 끌기 스크롤·편집 명령이다(Docs/Design/UiToolkit 22.7a절 — UE
// FSlateEditableTextLayout BuildDefaultContextMenu·HandleMouseButtonDown/Up,
// SMultiLineEditableText OnMouseMove).
namespace rocks::ui {

namespace {

// 메뉴 묶음 제목과 항목 글자다(UE EditableTextContextMenu "Modify Text"와 FGenericCommands 이름).
constexpr StringView ContextMenuHeading = "Modify Text";
constexpr StringView UndoLabel = "Undo";
constexpr StringView CutLabel = "Cut";
constexpr StringView CopyLabel = "Copy";
constexpr StringView PasteLabel = "Paste";
constexpr StringView DeleteLabel = "Delete";
constexpr StringView SelectAllLabel = "Select All";
// 메뉴 스타일 키다(StyleSet "Menu").
constexpr StringView MenuStyleKey = "Menu";

// 명령 단축키 표시다(Cmd인지는 호스트가 Platform으로 알린다 — 키 처리의 command 규칙).
KeyChord CommandChord(input::Key key, bool super) noexcept {
    KeyChord chord;
    chord.key = key;
    chord.super = super;
    chord.control = !super;
    return chord;
}

// 메뉴를 만들 때 정한 사용 가능 여부다(MenuRow는 만들 때 한 번 본다).
Delegate<bool()> Enabled(bool enabled) noexcept {
    return Delegate<bool()>::CreateLambda([enabled]() noexcept { return enabled; });
}

} // namespace

bool EditableText::IsContextMenuOpen() const noexcept {
    return contextMenuOpen_;
}

bool EditableText::ToLocal(Vec2 window, Vec2& local) const noexcept {
    AffineTransform2D inverse;
    if (!GetGeometry().render.TryInverse(inverse)) {
        return false;
    }

    local = inverse.Apply(window);
    return local.IsFinite();
}

void EditableText::CopySelection(bool cut) noexcept {
    const usize begin = anchor_ < caret_ ? anchor_ : caret_;
    const usize end = anchor_ < caret_ ? caret_ : anchor_;
    // 비밀번호는 복사하지 않는다. 읽기 전용은 잘라내지 않는다(UE CanExecuteCut·Copy).
    if (password_ || begin == end || (cut && readOnly_)) {
        return;
    }

    const StringView selection = text_.View().SliceBytes(begin, end - begin);
    if (platform_.writeClipboardText && platform_.writeClipboardText(selection) && cut) {
        Replace(begin, end, {});
    }
}

void EditableText::PasteClipboard() noexcept {
    if (readOnly_ || !platform_.readClipboardText) {
        return;
    }

    const usize begin = anchor_ < caret_ ? anchor_ : caret_;
    const usize end = anchor_ < caret_ ? caret_ : anchor_;
    String clipboard(allocator_);
    if (platform_.readClipboardText(clipboard)) {
        Paste(begin, end, clipboard.View());
    }
}

void EditableText::DeleteSelection() noexcept {
    const usize begin = anchor_ < caret_ ? anchor_ : caret_;
    const usize end = anchor_ < caret_ ? caret_ : anchor_;
    if (!readOnly_ && begin != end) {
        Replace(begin, end, {});
    }
}

void EditableText::UndoEdit() noexcept {
    // UE Undo: 조합 중이면 하지 않는다(3369–3371행).
    if (!composing_) {
        RestoreHistory(false);
    }
}

PointerReply EditableText::OnRightPointer(const PointerEvent& event) noexcept {
    Vec2 local;
    if (event.type == PointerEventType::Down) {
        // 조합 중의 누름은 먹는다(왼쪽 누름과 같은 규칙).
        if (composing_) {
            return {.handled = true};
        }

        if (rightPressed_ || !label_ || !ToLocal(event.position, local)) {
            return {};
        }

        // 선택 밖을 누르면 선택을 풀고 caret을 옮긴다. 선택 안이면 그 선택을 메뉴가
        // 쓴다(UE HandleMouseButtonDown 1354–1363행).
        const usize begin = anchor_ < caret_ ? anchor_ : caret_;
        const usize end = anchor_ < caret_ ? caret_ : anchor_;
        const usize at = PointerOffset(event.position);
        if (begin != end && (at < begin || at > end)) {
            MoveCaret(at, false);
        }

        MarkInteraction();
        rightPressed_ = true;
        rightScrolling_ = false;
        rightTravel_ = 0;
        rightLastY_ = local.y;
        rightPointer_ = event.pointerId;
        rightUser_ = event.userId;
        return {.handled = true, .capture = true};
    }

    if (!rightPressed_) {
        return {};
    }

    if (event.type == PointerEventType::Move) {
        if (!ToLocal(event.position, local)) {
            return {.handled = true};
        }

        // 커서를 잠갔으면 Application이 준 이동량(창 좌표)을 local로 바꾼다. 위치는 잠금 자리에
        // 머문다(23.5절).
        float delta = 0;
        Vec2 moved{};
        if (event.hasDelta && ToLocal(event.position + event.delta, moved)) {
            delta = moved.y - local.y;
        } else {
            delta = local.y - rightLastY_;
            rightLastY_ = local.y;
        }

        // 여러 줄이고 넘칠 때만 끌기 스크롤이다(UE IsRightClickScrolling — 막대가 필요할 때).
        // 끈 양이 문턱을 넘으면 내용을 잡아 끄는 방향으로 옮긴다(UE OnMouseMove 823–857행).
        if (multiline_ && contentSize_.y > viewSize_.y) {
            rightTravel_ += math::Abs(delta);
            if (rightTravel_ >= PointerDragThreshold) {
                // 스크롤이 시작된 자리가 소프트웨어 커서 자리다. 커서는 그동안 숨긴다(UE
                // UseHighPrecisionMouseMovement).
                if (!rightScrolling_) {
                    rightScrolling_ = true;
                    softwareCursor_ = local;
                }

                const float before = scroll_.y;
                SetScrollOffset(scroll_.y - delta);
                // 내용이 움직인 만큼 소프트웨어 커서도 따라간다(잡은 글자 위에 머문다).
                softwareCursor_.y += before - scroll_.y;
                InvalidatePaint();
                return {.handled = true, .lockCursor = true};
            }
        }

        return {.handled = true};
    }

    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
        const bool scrolled = rightScrolling_;
        rightPressed_ = false;
        rightScrolling_ = false;
        rightTravel_ = 0;
        // 끌어 스크롤했으면 커서를 소프트웨어 커서 자리(잡은 글자)로 되돌린다(UE OnMouseButtonUp
        // SetMousePos).
        if (scrolled) {
            InvalidatePaint();
            return {.handled = true,
                    .releaseCapture = true,
                    .releaseCursorAt = GetGeometry().render.Apply(softwareCursor_)};
        }

        // 끌어 스크롤했으면 메뉴를 열지 않는다. 놓은 자리가 편집기 안이면 그 자리에
        // 연다(UE HandleMouseButtonUp 1436–1470행).
        const Vec2 size = GetGeometry().size;
        if (event.type == PointerEventType::Up && !scrolled && ToLocal(event.position, local) &&
            local.x >= 0 && local.y >= 0 && local.x <= size.x && local.y <= size.y) {
            (void)OpenContextMenu(local, FocusCause::Pointer);
        }

        return {.handled = true, .releaseCapture = true};
    }

    return {.handled = true};
}

Result<void> EditableText::OpenContextMenu(Vec2 local, FocusCause cause) noexcept {
    Application* app = Application::Find(*this);
    if (!app || !label_ || !shaper_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (!contextAnchor_) {
        auto made = SharedPtr<MenuAnchor>::TryMake(allocator_, allocator_);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        // 보이지 않는 크기 0 anchor다 — 바탕을 그리지 않고 초점·누름을 받지 않는다. 생성기·통지는
        // 편집기를 raw로 부른다(편집기 소멸자가 DisconnectMenu로 끊는다).
        MenuAnchor& anchor = *made.Value();
        auto styled = anchor.SetStyle(ButtonStyle{});
        if (!styled) {
            return styled;
        }

        anchor.SetFocusable(false);
        anchor.SetVisibility(Visibility::HitTestInvisible);
        anchor.SetPlacement(MenuPlacement::Below);
        anchor.SetMenuFactory(
            MenuAnchor::MenuFactory::CreateRaw(this, &EditableText::MakeContextMenu));
        anchor.SetOnOpenChanged(
            Delegate<void(bool)>::CreateRaw(this, &EditableText::OnContextMenuOpenChanged));
        contextAnchor_ = Move(made.Value());
    }

    if (contextMenuOpen_) {
        contextAnchor_->Close();
    }

    // 붙여 넣기 가능 여부는 열 때 한 번 읽는다(UE는 그릴 때마다 읽는다 — 22.7a절).
    canPaste_ = false;
    if (!readOnly_ && platform_.readClipboardText) {
        String clipboard(allocator_);
        canPaste_ = platform_.readClipboardText(clipboard) && !clipboard.IsEmpty();
    }

    contextPoint_ = local;
    if (contextAnchor_->Parent() != this) {
        auto added = TryAddChild(contextAnchor_);
        if (!added) {
            return added;
        }
    }

    // 팝업 층이 anchor 배치를 읽으므로 열기 전에 자리를 정한다.
    auto placed = contextAnchor_->ArrangeIfNeeded(
        GetGeometry().MakeChild({}, {1, 0, 0, 1, local.x, local.y}, {}, {}));
    if (!placed) {
        (void)RemoveChild(*contextAnchor_);
        return placed;
    }

    // 초점이 메뉴로 가는 동안 초점 잃음을 미룬다.
    contextMenuOpen_ = true;
    auto opened = contextAnchor_->OpenFocused(*app, focusUser_, cause);
    if (!opened || !contextAnchor_->IsOpen()) {
        contextMenuOpen_ = false;
        (void)RemoveChild(*contextAnchor_);
        InvalidatePaint();
        if (!opened) {
            return opened;
        }

        return Result<void>::Failure(ErrorCode::NotFound);
    }

    InvalidatePaint();
    return Result<void>::Success();
}

Result<SharedPtr<Element>> EditableText::MakeContextMenu() noexcept {
    Application* app = Application::Find(*this);
    if (!app || !contextAnchor_ || !shaper_) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::NotReady);
    }

    const UiHostServices* host = FindUiHostServices();
    const MenuStyle* style = host ? host->FindStyle<MenuStyle>(MenuStyleKey) : nullptr;
    // 메뉴 글자는 편집기 셰이퍼로 shaping한다(메뉴 스타일 글꼴 크기).
    auto context = SharedPtr<MenuContext>::TryMake(
        allocator_, allocator_, *app,
        MenuLabelFactory::CreateLambda([this](StringView value, const FontStyle& font) noexcept
                                           -> Result<SharedPtr<TextLabel>> {
            auto label = SharedPtr<TextLabel>::TryMake(allocator_, allocator_);
            auto text = Text::Invariant(allocator_, value);
            if (!label || !text) {
                return Result<SharedPtr<TextLabel>>::Failure(!label ? label.Error() : text.Error());
            }

            auto shaped = shaper_->Shape(*label.Value(), text.Value(), font.size);
            if (!shaped) {
                return Result<SharedPtr<TextLabel>>::Failure(shaped.Error());
            }

            return label;
        }),
        style ? *style : DefaultMenuStyle());
    if (!context) {
        return Result<SharedPtr<Element>>::Failure(context.Error());
    }

    // 항목·사용 조건은 UE BuildDefaultContextMenu(3808–3852행)와 CanExecute*다.
    context.Value()->SetRoot(contextAnchor_.Get());
    MenuBuilder menu(context.Value(), contextAnchor_.Get());
    const bool selected = anchor_ != caret_;
    menu.BeginSection(ContextMenuHeading);
    menu.AddEntry(UndoLabel,
                  {.execute = Delegate<void()>::CreateRaw(this, &EditableText::UndoEdit),
                   .canExecute = Enabled(!readOnly_ && !composing_)},
                  CommandChord(input::Key::Z, platform_.commandIsSuper));
    menu.BeginSection();
    menu.AddEntry(
        CutLabel,
        {.execute = Delegate<void()>::CreateLambda([this]() noexcept { CopySelection(true); }),
         .canExecute = Enabled(!readOnly_ && !password_ && selected)},
        CommandChord(input::Key::X, platform_.commandIsSuper));
    menu.AddEntry(
        CopyLabel,
        {.execute = Delegate<void()>::CreateLambda([this]() noexcept { CopySelection(false); }),
         .canExecute = Enabled(!password_ && selected)},
        CommandChord(input::Key::C, platform_.commandIsSuper));
    menu.AddEntry(PasteLabel,
                  {.execute = Delegate<void()>::CreateRaw(this, &EditableText::PasteClipboard),
                   .canExecute = Enabled(canPaste_)},
                  CommandChord(input::Key::V, platform_.commandIsSuper));
    KeyChord deleteChord;
    deleteChord.key = input::Key::Delete;
    menu.AddEntry(DeleteLabel,
                  {.execute = Delegate<void()>::CreateRaw(this, &EditableText::DeleteSelection),
                   .canExecute = Enabled(!readOnly_ && selected)},
                  deleteChord);
    menu.BeginSection();
    menu.AddEntry(SelectAllLabel,
                  {.execute = Delegate<void()>::CreateRaw(this, &EditableText::SelectAllText)},
                  CommandChord(input::Key::A, platform_.commandIsSuper));
    auto panel = menu.MakePanel();
    if (!panel) {
        return Result<SharedPtr<Element>>::Failure(panel.Error());
    }

    return Result<SharedPtr<Element>>::Success(panel.Value());
}

void EditableText::OnContextMenuOpenChanged(bool open) noexcept {
    if (open) {
        return;
    }

    contextMenuOpen_ = false;
    // 자리 anchor는 열린 동안만 자식이다(편집기 자식은 평소대로 레이블·힌트).
    if (contextAnchor_ && contextAnchor_->Parent() == this) {
        (void)RemoveChild(*contextAnchor_);
    }

    MarkInteraction();
    InvalidatePaint();
    // 초점이 편집기로 돌아오지 않았으면(메뉴 밖으로 옮김·트리에서 빠짐) 미룬 초점 잃음을
    // 처리한다(UE는 메뉴가 닫히면 편집기에 초점을 준다 — OnContextMenuClosed 2219–2224행).
    Application* app = Application::Find(*this);
    if (focused_ && (!app || app->GetFocus(focusUser_).Get() != this)) {
        HandleFocusLost(focusUser_);
    }
}

} // namespace rocks::ui
