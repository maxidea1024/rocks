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
- 키를 기다리는 동안의 글자("...")·선택 해제 키(Escape)·허용 키 규칙은 지금 그대로다.

### 23.5 끄는 동안 커서 숨김·되돌림(UE `FReply::UseHighPrecisionMouseMovement`·`SetMousePos`)

- **왜**: 숫자 입력 끌기(UE `SSpinBox`)와 편집 텍스트 오른쪽 끌기 스크롤(UE `SMultiLineEditableText` — 22.7a절)은 끄는 동안 커서를 숨기고, 화면 끝에 닿아도 계속 움직이며, 놓으면 누른 자리(또는 정한 자리)로 커서를 되돌린다.
- **창**: `Window::SetCursorPosition(Vec2 client)`(UE `ICursor::SetPosition` — Win64 `ClientToScreen` + `SetCursorPos`)를 더한다. 지원하지 않으면 Unsupported다.
- **UI 계약**
  - `PointerReply`에 `lockCursor`를 더한다(누름 처리 — capture와 함께). Application이 그 포인터에 대해 "잠금"에 들어간다: 커서를 숨기고(`SetCursorVisible(false)`), 누른 창 좌표를 기억한다.
  - 잠금 중 Move는 위치 대신 이동량을 싣는다: `PointerEvent::delta`(새 필드 — 직전 Move 뒤 움직인 창 좌표)를 채우고, 커서를 잠금 자리로 되돌려(`SetCursorPosition`) 화면 끝에 닿지 않게 한다(재중심 방식 — UE Windows high precision mouse는 raw input이지만 Rocks 창 입력은 raw mouse가 없어 이 방식이다). `position`은 잠금 자리 그대로다.
  - Up·capture 잃음이면 잠금을 풀고 커서를 보인다. 커서를 둘 자리는 reply `releaseCursorAt`(Optional 창 좌표 — 없으면 누른 자리)이다(UE `FReply::SetMousePos`).
  - 커서 서비스는 호스트가 준다(`UiHostServices::SetCursorService` — 숨김·보임·위치. UiCore는 PlatformWindow에 의존하지 않는다). 서비스가 없으면 잠금은 capture만 하고 delta는 위치 차이다(시험·헤드리스).
  - 잠금 중 그 포인터의 hover·툴팁은 멈춘다(커서가 보이지 않는다).
- **쓰는 곳**: 24장 숫자 입력, 22.7a절 편집 텍스트 오른쪽 끌기 스크롤(놓을 때 누른 자리로 — UE `SoftwareCursorPosition`).
- **시험**: 가짜 커서 서비스가 받은 숨김·위치·보임 순서, delta 누적, Up·capture 잃음 복원, 서비스 없을 때.
- **사람 확인**: 실제 창에서 끄는 동안 커서가 보이지 않고 화면 끝에서도 값이 계속 바뀌는지, 놓으면 제자리인지.

### 23.6 스크롤 막대 표시 정책(UW4v — 9.1절)

- **정책 `ScrollBarVisibility`**
  - `Auto`(UE 기본): 넘칠 때만 보인다(UE `SScrollBar::ShouldBeVisible` 335–350행).
  - `Always`: 늘 보이고, 트랙도 보인다(UE `AlwaysShowScrollbar`·`AlwaysShowScrollbarTrack`).
  - `HideWhenNotInUse`(UE `bHideWhenNotInUse`): 넘칠 때만 보인다. 마지막 스크롤·올림 뒤 0.2초부터 0.2초 동안 흐려져 사라진다(`GetThumbOpacity` 284–305행의 Lerp 식 그대로).
  - `OnHover`(Rocks 추가 — UE에 없음): 포인터가 영역에 들어오거나 스크롤하면 나타나고, 나가면 같은 식으로 흐려진다.
- **배치 `ScrollBarPlacement`**: `Reserve`(막대 칸을 따로 두어 내용이 줄어든다 — UE `SScrollBox` 기본) / `Overlay`(내용 위). `OnHover`의 기본은 Overlay이고, 나머지 기본은 Reserve다.
- **손잡이 불투명도**: 보통 0.75, 올림·끌기 1이다(UE 287–305행). 휠·끌기 중에는 정책과 관계없이 보인다.
- **흐려짐 시계**: 프레임 시각(20.3절)이다. 흐려지는 동안은 volatile로 다시 그린다(20.12절). 시험은 시각을 주입한다.
- 대상은 `ScrollView`와 그것을 쓰는 목록(`ListView`·`TreeView`·`TileView`), 그리고 독립 `ScrollBar`(값 계약만 — 정책은 호스트가 보임을 정한다)다.
- 갤러리: Scroll View 행에 네 정책을 나란히 둔다(16장).
- 시험: 정책마다 보임·불투명도(주입 시각 0·0.2·0.3·0.4초), Reserve 내용 폭, Overlay 겹침, 휠 중 보임.

### 23.7 갤러리

- Widgets 탭: Combo Box·Simple Combo Box, Toggle Button·Alt·Transparent·Radio(셋 묶음), Input Key Selector(고정 크기 없음), Scroll View 정책 넷.
- self-test(16.8절): 라디오 묶음에서 하나만 체크되는지, 토글 체크 바탕 Primary, 콤보 펼침 행 올림 Primary.

### 23.8 시험

- 스타일: StyleSet 새 키와 값, 앱 스타일 교체 뒤 다시 받기.
- 콤보: 원하는 크기(내용·여백·화살표), 펼침 바탕·행 여백·올린 행 색.
- 체크 상자: 형식별 그리기(바탕·상태 이미지·전경 tint·여백), 해제 이미지, 토글 바탕 tint.
- 키 선택: 원하는 크기.
- 커서 잠금·스크롤 막대 정책: 23.5·23.6절.

### 23.9 단계

- **UW4b2a**: 콤보 스타일·고정 크기 지우기(23.2절).
- **UW4b2b**: 체크 상자 형식(토글·라디오·투명)(23.3절).
- **UW4b2c**: 키 선택 스타일·고정 크기 지우기(23.4절).
- **UW4b2d**: 끄는 동안 커서 숨김·되돌림(23.5절) — 창 `SetCursorPosition`, UI 잠금 계약, 편집 텍스트 오른쪽 끌기 적용.
- **UW4b2e**: 스크롤 막대 표시 정책(23.6절).
- **UW4n**: 숫자 입력(24장 — 착수 때 쓴다).
- 리뷰어는 9.1절대로 UW4b 2차 끝(UW4n 포함)에 한 번 둔다.
