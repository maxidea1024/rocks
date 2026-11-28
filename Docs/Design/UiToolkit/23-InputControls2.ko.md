## 23. 입력 컨트롤 2차(UW4b 2차 — 9.1절)

9.1절 UW4b의 1차(슬라이더·진행·스크롤 막대·throbber·tooltip)는 끝났다. 이 장은 2차 묶음의 세부 설계다: 콤보 스타일, 체크 상자 형식(토글·라디오), 키 선택 상자, 플랫폼 커서 숨김·되돌림, 스크롤 막대 표시 정책(UW4v), 고정 최소 크기 지우기(UW4a3에서 미룬 것). 숫자 입력(UW4n)은 24장이다. 순서는 23.9절이다.

### 23.1 지금 상태

- `ComboBox`(MenuAnchor 파생)는 기본 버튼 스타일(`DefaultButtonStyle`)과 고정 크기 80×24를 쓴다. 화살표는 스타일 없이 상수로 그린다. 펼침 목록의 바탕·행 강조도 스타일이 없다.
- `CheckBoxStyle`은 체크 상자 모양 하나뿐이다. 해제 이미지·형식(UE `ESlateCheckBoxType`)이 없어서 토글 버튼과 라디오를 만들 수 없다. 갤러리의 Toggle Button·Radio Button 행은 비어 있다(16장 표 67·70행 — UW4b).
- `InputKeySelector`는 고정 크기 100×24이고, `SpinBox`는 72×24다(24장에서 바꾼다).
- `ScrollView`는 넘칠 때만 손잡이를 그리고, 늘 내용 위에 겹친다. 정책 선택이 없다(9.1절 UW4v).
- 창에는 `SetCursorVisible`·`SetMouseLocked`가 있지만 커서를 옮기는 API가 없다. UI에는 "끄는 동안 커서를 숨기고 놓을 때 되돌리는" 모드가 없다. 그래서 편집 텍스트의 오른쪽 끌기 스크롤(22.7a절)과 숫자 입력 끌기(24장)가 UE와 다르다.

### 23.2 콤보(UE `FComboButtonStyle`·`FComboBoxStyle`·"ComboBox.Row" — StarshipCoreStyle.cpp 1557–1640행)

- **UiCore 스타일**
  - `ComboButtonStyle`
    - `button`: ButtonStyle(아래 값). `contentPadding` 0.
    - `downArrow`: StyleBrush(SVG `WideChevronDown` 16×16, 전경 tint). `downArrowPadding` 2(사방 — UE `FComboButtonStyle` 기본, SlateTypes.cpp 176행), 세로 가운데.
    - `menuBorder`: RB(Dropdown, 0, DropdownOutline, 1). `menuBorderPadding` 0.
  - `ComboBoxStyle`: `comboButton`(위), `contentPadding` 0, `menuRowPadding` (8, 3).
  - `TableRowStyle`(UE `FTableRowStyle`의 콤보 행에 쓰는 부분만): 보통 없음, 올림·강조 Color(Primary), 글자 White, 초점 선택 RB(Transparent, 4, Select, 1).
  - 버튼 값(UE `ComboButtonButton`): 보통 RB(Input, 4, InputOutline, 1), 올림·눌림 RB(Input, 4, Hover, 1), 비활성 RB(Dropdown, 4, DropdownOutline, 1). 전경은 Foreground이고 올림·눌림·비활성은 ForegroundHover. 여백 (8, 1, 8, 1)·눌림 (8, 2, 8, 0)(UE `ComboButtonMargin`·`PressedComboButtonMargin` — CoreStyle.cpp 58–59행).
  - "SimpleComboButton"·"SimpleComboBox"(테두리 없음): 버튼은 `SimpleButtonStyle`이고, 행 여백은 (8, 2)다.
  - StyleSet 키는 "ComboButton"·"ComboBox"·"SimpleComboButton"·"SimpleComboBox"·"ComboBox.Row"다(`AddCoreStyles`).
- **`ComboBox`**
  - 스타일은 `ComboBoxStyle`이다(`SetStyle`·`ApplyStyles` — 20.11절 규칙). 버튼 모양은 `comboButton.button`을 MenuAnchor(Button)에 넘긴다.
  - 고정 크기 80×24를 지운다. 원하는 크기는 내용 + 버튼 여백 + 화살표 칸이다(UE `SComboButton` — 내용 슬롯 옆에 화살표 이미지 AutoWidth). 화살표 칸은 화살표 이미지 크기(16)와 `downArrowPadding`(2 — UE `SComboButton` 화살표 슬롯 `Padding(DownArrowMargin)`, SComboButton.cpp 26–41·82행)이다. 지금 상수 `ComboArrowPadding`(2)을 스타일 값으로 바꾼다.
  - 펼침 목록은 `menuBorder` 바탕과 `menuBorderPadding` 안에 행을 둔다. 바탕은 `Frame::SetStyleBrush`(새 — StyleBrush를 그릴 때 테마로 풀어 테마가 바뀌어도 따라간다, UE `SBorder` BorderImage가 FSlateBrush 그대로인 것과 같다. `SetBrush`로 풀린 Brush를 주면 지운다)로 그린다.
  - 행은 콤보 전용 행 버튼이다(Private `ComboRow` — 버튼 스타일을 행 스타일에서 만든다: 보통 `evenRow`(없음), 올림·눌림 `evenRowHovered`(Primary), 글자 `text`(White), 여백 `menuRowPadding`). 키보드로 옮긴 강조 행도 올림 모양이다(`IsHighlighted`). 지금의 박힌 행 색(`SetColor` 두 상수)과 고정 행 높이 24를 지운다 — 행 높이는 내용 + 여백이다.
  - 처음 펼칠 때 선택 행이 보이게 한다. 행 높이를 어림하지 않고 `ScrollView::RequestScrollIntoView(행, Nearest)`(새 — 다음 배치에서 자식을 배치한 뒤 같은 배치 안에서 맞춘다, UE `STableView::RequestScrollIntoView`)를 쓴다.
  - 콤보 버튼(`MenuAnchor` — 16장 87행 "Combo Button")도 `ComboButtonStyle`을 받는다(`MenuAnchor::SetComboButtonStyle`이 아니라 `ComboButton` Element 이름으로 둘지는 착수 때 UE `SComboButton` 구조를 보고 정한다 — 지금 `MenuAnchor`는 메뉴 막대·하위 메뉴의 기반이라 모양을 넣지 않는다).
- 갤러리: Combo Box(스타일), Simple Combo Box(테두리 없음) 행.

### 23.3 체크 상자 형식(UE `ESlateCheckBoxType` — StarshipCoreStyle.cpp 1662–1841행, `SCheckBox::BuildCheckBox` 521–590행)

- **`CheckBoxStyle`에 더한다**: `type`(CheckBox·ToggleButton), 해제 이미지 셋(`uncheckedImage`·`uncheckedHoveredImage`·`uncheckedPressedImage`), `borderBackgroundColor`(토글의 바탕 tint — UE 기본 White). 지금 필드는 그대로다.
- **그리기(UE `BuildCheckBox`)**
  - CheckBox 형식: 왼쪽에 바탕 이미지(상태별 `background*`), 그 위에 상태 이미지(해제·체크·미정 × 보통·올림·눌림)를 전경색 tint로 그린다. 오른쪽 내용은 `padding` 뒤에 둔다. 상태 이미지가 없으면(해제 기본) 바탕만 그린다.
  - ToggleButton 형식: 상태 이미지를 버튼 바탕(내용 전체 뒤)으로 그리고, 내용은 `padding` 안에 둔다(상자 칸 없음).
- **기본 스타일(UiCore — 값은 UE 그대로)**
  - "Checkbox": 지금 값.
  - "ToggleButtonCheckbox": 체크 RB(Primary, 4)·올림 PrimaryHover·눌림 Dropdown, 해제 없음·올림·눌림 RB(Dropdown, 4). 전경은 Foreground이고 나머지 상태는 ForegroundHover. 여백은 UE `ToggleButtonMargins` (16, 2)다(CoreStyle.cpp 57행).
  - "ToggleButtonCheckboxAlt": 체크 RB(Input, 4)·올림 RB(Hover, 4), 체크 전경 White.
  - "TransparentCheckBox": 토글 형식, 이미지 없음.
  - "RadioButton": 체크 상자 형식, 바탕 없음. 해제 SVG `RadioOff` 16 Dropdown·올림 Hover, 체크 `RadioOn` Primary·올림 PrimaryHover, 미정 `RadioOff` White25·올림 ForegroundHover. 전경은 White, 여백은 4.
- 라디오 묶음(하나만 체크)은 부품이 아니다 — UE처럼 호출자가 상태 delegate로 묶는다. 갤러리 견본이 셋을 묶는다.
- 갤러리: Toggle Button(텍스트·아이콘), Toggle Button Alt, Radio Button(셋 묶음), Transparent Check Box 행. 16장 표 67·70행을 채운다.

### 23.4 키 선택 상자(UE `SInputKeySelector`)

- 버튼 스타일은 "Button"(`DefaultButtonStyle`), 글자 스타일은 NormalText다(UMG `UInputKeySelector` 기본 WidgetStyle·TextStyle).
- 고정 크기 100×24를 지운다. 원하는 크기는 글자 + 버튼 여백 + `margin`(UE `Margin` 인자)이다. 기본 `margin`은 0이다(UMG `UInputKeySelector::Margin`·`SInputKeySelector` `Margin` 인자 기본 — 빈 FMargin).
- UE는 `Margin`을 안쪽 글자(`STextBlock::Margin`)에 준다. Rocks 내용은 호출자가 넣는 요소라 `margin`을 버튼 내용 여백(`Button::SetContentPadding`)으로 넣는다. 스타일 여백에 더해지는 결과는 같다. `SetMargin`·`GetMargin`이 이를 감싼다.
- 키를 기다리는 동안의 글자("...")·선택 해제 키(Escape)·허용 키 규칙은 지금 그대로다.

### 23.5 끄는 동안 커서 숨김·되돌림(UE `FReply::UseHighPrecisionMouseMovement`·`SetMousePos`)

- **왜**: 숫자 입력 끌기(UE `SSpinBox`)와 편집 텍스트 오른쪽 끌기 스크롤(UE `SMultiLineEditableText` — 22.7a절)은 끄는 동안 커서를 숨기고, 화면 끝에 닿아도 계속 움직이며, 놓으면 누른 자리(또는 정한 자리)로 커서를 되돌린다.
- **이동량은 창이 잰다**: 커서를 되돌리는 일(재중심)은 창 메시지를 바꾸는 자리에서 한다. UI가 큐를 처리하면서 되돌리면 이미 큐에 쌓인 절대 위치들이 옛 기준이라 이동을 두 번 센다. UE Windows high precision mouse는 raw input이지만 Rocks 창 입력은 raw mouse가 없어 재중심 방식이다. 창 메시지 처리와 UI는 같은 게임 lane이라 잠금 요청과 메시지 변환 사이에 경쟁이 없다.
- **창**(UE `ICursor::SetPosition`·`FWindowsCursor::SetPosition`)
  - `Window::LockPointer(x, y)`: 커서를 숨기고(`SetCursorVisible` 값과 따로) 잠금 자리 (x, y)(client)를 정한다. 그 뒤 마우스 이동은 `MouseMove`에 `relative = true`·`deltaX`·`deltaY`(직전 기준에서 움직인 client 픽셀)를 싣고, 위치는 잠금 자리다. 이동을 받을 때마다 커서를 잠금 자리로 되돌린다(`ClientToScreen` + `SetCursorPos`). 첫 기준은 창이 마지막으로 바꾼 마우스 위치다 — 잠금 요청 전에 움직였지만 아직 받지 않은 이동을 잃지 않는다. 되돌림이 만든 이동(이동량 0)은 이벤트를 내지 않는다. 잠금 중 버튼 이벤트 위치도 잠금 자리다.
  - `Window::UnlockPointer()`: 잠금을 풀고 커서 보임을 `SetCursorVisible` 값으로 되돌린다. 잠그지 않았으면 아무것도 하지 않는다.
  - `Window::SetCursorPosition(x, y)`: 커서를 client (x, y)로 옮긴다. 창에 초점이 없으면 옮기지 않는다(다른 창을 쓰는 사람의 커서를 뺏지 않는다).
  - 창이 초점을 잃으면 잠금을 스스로 푼다(UI는 FocusLost로 capture를 푼다).
  - 지원하지 않는 backend는 Unsupported다(기본 구현).
- **UI 계약**
  - `PointerReply::lockCursor`(마우스만): capture를 잡는 reply(Down) 또는 capture 중인 요소의 reply(Move — UE `UseHighPrecisionMouseMovement`는 끌기가 시작된 Move에서 온다)에 둔다. Application이 그 capture를 잠근다. 그 이벤트 위치를 잠금 자리로 기억하고 커서 서비스 `LockPointer(잠금 자리)`를 부른다. 이미 잠갔으면 다시 부르지 않는다. capture가 없는 요소의 lockCursor는 무시한다.
  - `PointerEvent::delta`·`hasDelta`: 잠근 capture의 Move에 이동량을 싣는다. 창이 준 이동량(`relative`)이면 그대로, 아니면(서비스 없음·Unsupported) 직전 위치와의 차이다. 잠근 동안 Move의 `position`은 잠금 자리 그대로다(커서가 보이지 않는다). 잠그지 않은 Move는 `hasDelta`가 거짓이다.
  - `PointerReply::releaseCursorAt`(Optional 창 좌표): capture 중 어느 reply든 정하면 기억한다. capture가 풀리면(Up·`releaseCapture`·capture 잃음·요소 떼어짐·FocusLost) 서비스 `UnlockPointer(releaseCursorAt 또는 잠금 자리)`를 부른다(UE `FReply::SetMousePos`).
  - 커서 서비스는 호스트가 준다(`UiCore/CursorService.h` — `LockPointer(Vec2)`·`UnlockPointer(Vec2 releaseAt)`, `Application::SetCursorService`. UiCore는 PlatformWindow에 의존하지 않는다). 서비스가 없으면 잠금은 capture만 하고 delta는 위치 차이다(시험·헤드리스). `LockPointer`가 실패하면 원인을 경고 로그로 남기고 capture만 한다(조용히 넘기지 않는다).
  - 잠금 중 마우스 hover·툴팁은 멈춘다(커서가 보이지 않는다).
- **쓰는 곳**: 24장 숫자 입력, 22.7a절 편집 텍스트 오른쪽 끌기 스크롤.
- **편집 텍스트 오른쪽 끌기**(UE `SMultiLineEditableText::OnMouseMove`·`OnMouseButtonUp`): 끌기 스크롤이 시작된 Move reply에 `lockCursor`를 둔다. 그때 위치를 소프트웨어 커서 자리로 기억하고, 스크롤로 내용이 움직인 만큼 그 자리도 옮긴다(UE `SoftwareCursorPosition.Y += 이전 - 새 offset`). 그동안 그 자리에 잡은 손 아이콘(`icons::CursorGrab` — UE `SoftwareCursor_Grab`, Rocks가 그린 SVG, 16px)을 그린다. 놓으면 `releaseCursorAt`을 그 자리로 둔다(커서가 잡았던 글자 위로 돌아온다). 이동량은 `delta.y`다.
- **시험**: 가짜 커서 서비스가 받은 잠금·풀기 자리와 순서, 창 이동량(relative) 그대로 전달, 서비스 없을 때 위치 차이, Up·capture 잃음·FocusLost·`releaseCursorAt` 복원, 잠금 중 hover 멈춤, Win64 창의 잠금 상태 전이(초점 잃음 풀기).
- **사람 확인**: 실제 창에서 끄는 동안 커서가 보이지 않고 화면 끝에서도 값이 계속 바뀌는지, 놓으면 제자리인지.

### 23.5a 놓친 modifier 뗌 보정(UE `FWindowsApplication::CheckForShiftUpEvents`·`UpdateAllModifierKeyStates`)

- **결함**: "hello" 뒤에 Win+.(이모지 패널)로 이모지를 넣고 Backspace를 두 번 누르면, 두 번째에 "hello"가 한꺼번에 지워졌다. Win 누름(`LeftSuper` KeyDown)은 창에 오지만, 뗌은 이모지 패널이 가져가 창에 오지 않는다. 창 초점은 그대로라 FocusLost도 없다. Application의 modifier 상태(`UpdateChord`)가 Super를 누른 채로 두고, 편집 텍스트는 Super+Backspace를 Ctrl+Backspace처럼 단어 지우기로 처리했다. 첫 Backspace도 이모지를 단어 하나로 지워 정상처럼 보였다.
- **UE**: Windows에서 Win 키를 modifier로 세지 않는다. Shift는 다른 Shift가 눌려 있으면 뗌 메시지가 오지 않아 키 메시지 때 `GetKeyState`로 확인하고 KeyUp을 만든다(`CheckForShiftUpEvents`). 입력을 버리는 창·활성화 때는 `GetAsyncKeyState`로 전체를 다시 읽는다(`UpdateAllModifierKeyStates`). 다른 modifier의 놓친 뗌은 보정하지 않는다.
- **Rocks(UE보다 넓게)**: 창이 누름을 보고한 modifier(Shift·Control·Alt·Super 좌우 여덟)를 적어 둔다(`ModifierLatch` — `PlatformWindow/Private/Common/ModifierLatch.h`, 플랫폼 헤더 없는 판정, EngineTests로 시험).
  - 확인 시점: 키·글자 메시지(`WM_KEYDOWN`·`WM_KEYUP`·`WM_SYSKEY*`·`WM_CHAR`)와 마우스 버튼 누름을 처리하기 전, 그리고 스레드 메시지를 다 처리한 뒤(펌프 끝 — 키를 누르지 않고 마우스만 써도 풀린다).
  - 판정: 적힌 modifier 가운데 OS 물리 상태(`GetAsyncKeyState`)가 뗌이면 KeyUp을 만들어 넣고 적힘을 지운다. 단, 스레드 큐에 키 메시지가 남아 있으면(`GetQueueStatus(QS_KEY)`) 보정하지 않는다 — 빨리 뗀 키의 진짜 뗌이 아직 큐에 있으면 순서대로 오게 둔다(그 앞 키의 chord가 바뀌지 않는다).
  - 보정한 뒤 늦게 온 진짜 뗌(적히지 않은 modifier의 KeyUp)은 버린다(뗌이 두 번 가지 않는다). 창 밖에서 누르고 창 안에서 뗀 modifier도 누름을 보고하지 않았으므로 같다.
  - 초점을 잃으면 적힘을 비운다(FocusLost가 입력 상태 전체를 되돌린다).
  - 보정은 정보 로그 한 줄을 남긴다(어느 키를 왜 — 조용히 넘기지 않는다).
- **Win 키**: Super를 편집 명령 modifier로 쓰는 정책(`command = control || super`)은 그대로다(macOS Command와 같은 경로). 놓친 뗌을 바로잡으므로 Win+.·Win+V 뒤에도 남지 않는다.
- **macOS**: Command 중 keyUp은 앱 local monitor가 이미 넘긴다(MacPlatform 문서). 같은 판정을 Mac backend에도 둘지는 Mac 빌드 확인 때 정한다.
- **시험**: 판정 — 누름 적기·반복 누름, 물리 뗌이면 KeyUp 목록(여럿·좌우 따로), 큐에 키 메시지가 있으면 보류, 보정 뒤 늦은 뗌 버림, 적히지 않은 modifier 뗌 버림, modifier 아닌 키는 늘 통과, 비우기.
- **사람 확인**: UiDemo Editable Text Box에서 "hello" → Win+. 이모지 → Backspace를 하나씩 누르면 한 글자씩 지워지는지.

### 23.6 스크롤 막대 표시 정책(UW4v — 9.1절)

- **정책 `ScrollBarVisibility`**(`UiFramework/Scrolling/ScrollBarVisibility.h`)
  - `Auto`(UE 기본): 넘칠 때만 보인다(UE `SScrollBar::ShouldBeVisible` 335–350행).
  - `Always`: 늘 보이고, 트랙도 보인다(UE `AlwaysShowScrollbar`·`AlwaysShowScrollbarTrack`). 넘치지 않으면 손잡이가 트랙 전체다.
  - `HideWhenNotInUse`(UE `bHideWhenNotInUse`): 넘칠 때만 보인다. 마지막 상호작용 뒤 0.2초부터 0.2초 동안 흐려져 사라진다(`GetThumbOpacity` 284–305행의 `Lerp(1, 0, Clamp((Δ - 0.2) / 0.2))` 그대로). 상호작용은 UE처럼 스크롤 위치 변화(`SetState`)와 막대 위 포인터 들어옴·나감이다. 내용 끌기·터치 팬 중에는 Δ = 0이다(UE `bIsScrolling`). 처음에는 숨어 있다(UE `LastInteractionTime = 0`).
  - `OnHover`(Rocks 추가 — UE에 없음): 넘칠 때, 포인터가 영역(내용 포함) 안에 있으면 보통 불투명도로 보인다. 나가거나 밖에서 스크롤되면 위 식으로 흐려진다(나간 뒤 0.2초부터, 최대 불투명도는 보통 값 0.75).
- **배치 `ScrollBarPlacement`**(`ScrollBarPlacement.h`): `Reserve`(막대 칸 = 두께 + 막대 여백 양쪽을 따로 두어 내용이 줄어든다 — UE `SScrollBox` 기본: 막대가 내용 옆 칸이다) / `Overlay`(내용 위). 정하지 않으면 `OnHover`는 Overlay, 나머지는 Reserve다. Reserve 칸은 막대가 보일 때(`Always`, 또는 넘칠 때 — 흐려져 안 보여도 칸은 둔다, UE는 흐려짐이 불투명도뿐이다)만 둔다. 원하는 크기에도 그 칸을 더한다(넘침은 지난 배치 값 — UE도 지난 프레임 막대 보임으로 정한다).
- **손잡이 불투명도**: 보통 0.75, 막대 위 올림·손잡이 끌기 1이다(UE 287–305행 — UE `IsHovered`는 막대 전체다). 휠·끌기는 위치를 바꾸므로 정책과 관계없이 보인다(그 뒤 흐려짐 정책이면 흐려진다).
- **흐려짐 시계**: 프레임 시각(20.3절, `UiHostServices::Time`)이다. 흐려지는 동안만 프레임 타이머(`Application::StartFrameTimer`)로 매 프레임 다시 그리고, 다 흐려지면 타이머를 놓는다. 시험은 `BeginFrame` 시각을 주입한다.
- 불투명도 식은 공용 함수(`ScrollThumbOpacity` — `UiFramework/Scrolling/ScrollThumbOpacity.h`)다. 목록(`ListView`·`TreeView`·`TileView`)은 지금 막대를 그리지 않는다 — 목록 탭(UW4a2)에서 막대를 얻을 때 같은 정책·함수를 쓴다. 독립 `ScrollBar`는 값 계약만이다(정책은 호스트가 보임을 정한다).
- API: `ScrollView::SetScrollBarVisibility`·`GetScrollBarVisibility`·`SetScrollBarPlacement`·`GetScrollBarPlacement`. `SetShowScrollbar(visible)`는 막대를 아예 끈다(UE `ScrollBarVisibility` Collapsed). 옛 `SetShowScrollbar(visible, alwaysShow)`의 `alwaysShow`는 `Always` 정책으로 바뀐다(하위 호환 없음). `ScrollBoxWidget`도 `alwaysShowScrollbar` 대신 `scrollbarVisibility`·`scrollbarPlacement` 속성이다.
- 갤러리: "Scroll Bar Visibility" 행에 네 정책을 나란히 둔다(16장).
- 시험: 정책마다 보임·불투명도(주입 시각 0·0.2·0.3·0.4초), 올림·끌기 1, Reserve 내용 폭·원하는 크기, Overlay 겹침, 휠 뒤 보임, OnHover 들어옴·나감, 흐려짐 타이머 놓음.

### 23.7 갤러리

- Widgets 탭: Combo Box·Simple Combo Box, Toggle Button·Alt·Transparent·Radio(셋 묶음), Input Key Selector(고정 크기 없음), Scroll View 정책 넷.
- self-test(16.8절): 라디오 묶음에서 하나만 체크되는지, 토글 체크 바탕 Primary, 콤보 펼침 행 올림 Primary.

### 23.8 시험

- 스타일: StyleSet 새 키와 값, 앱 스타일 교체 뒤 다시 받기.
- 콤보: 원하는 크기(내용·여백·화살표), 펼침 바탕·행 여백·올린 행 색.
- 체크 상자: 형식별 그리기(바탕·상태 이미지·전경 tint·여백), 해제 이미지, 토글 바탕 tint.
- 키 선택: 원하는 크기.
- 커서 잠금·놓친 modifier 뗌·스크롤 막대 정책: 23.5·23.5a·23.6절.

### 23.9 단계

- **UW4b2a**: 콤보 스타일·고정 크기 지우기(23.2절).
- **UW4b2b**: 체크 상자 형식(토글·라디오·투명)(23.3절).
- **UW4b2c**: 키 선택 스타일·고정 크기 지우기(23.4절).
- **UW4b2d**: 끄는 동안 커서 숨김·되돌림(23.5절) — 창 `LockPointer`·`UnlockPointer`·`SetCursorPosition`(창이 이동량을 재고 되돌림), 입력 `relative`·`deltaX`·`deltaY`, UI 잠금 계약(`CursorService`), 편집 텍스트 오른쪽 끌기(소프트웨어 커서 `icons::CursorGrab`).
- **UW4b2e**: 스크롤 막대 표시 정책(23.6절) — `ScrollBarVisibility`·`ScrollBarPlacement`, `ScrollThumbOpacity`, ScrollView Reserve 칸·흐려짐 타이머, `ScrollBoxWidget` 속성, 갤러리 행.
- **UW4b2f**: 놓친 modifier 뗌 보정(23.5a절) — `ModifierLatch`, Win64 창 키·글자·버튼 메시지와 펌프 끝 확인, 늦은 뗌 버림.
- **UW4n**: 숫자 입력(24장 — 단계는 24.9절 UW4n1–UW4n6).
- 리뷰어는 9.1절대로 UW4b 2차 끝(UW4n 포함)에 한 번 둔다.
