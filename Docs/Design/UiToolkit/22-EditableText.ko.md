## 22. 편집 텍스트 공개(UW4f2 — 병합 뒤)

9.1절 UW4f2의 세부 설계다. UW9b(21.2절)가 상태 소유(`EditableText`가 글자·선택·조합·이력을 든다)와 셰이퍼 경계(`TextShaper`)를 이미 옮겼다. 이 장은 남은 부분을 정한다. `TextShaper`의 `FontStyle` 서명은 UW4 글꼴 자동 셰이핑과 함께 바꾸므로 여기서 다루지 않는다.

### 22.1 지금 상태(UW9b 뒤)

- `EditableText`(UiFramework) 생성자가 `boxed`를 받는다. 상자 모양이면 같은 Element가 안쪽 여백·바탕·오류 캡션까지 그린다. 편집 로직과 상자 모양이 한 클래스에 섞여 있다.
- 힌트·오류 캡션의 색이 코드에 박혀 있다(힌트 회색, 오류 빨강). 스타일 구조체가 없다.
- 편집 이력은 64단계다(UE 99).
- 더블클릭 단어 선택, 세 번 클릭 줄 선택, caret 깜박임이 없다.
- 여러 줄 상자에 세로 스크롤 막대가 없다. 휠·caret 따라가기 스크롤만 있다.
- 오류는 입력 칸 아래 빨간 글자 줄이다. UE는 오른쪽 "!" 표지와 오류 팝업이다.

### 22.2 나눔(UE `SEditableText`·`SMultiLineEditableText` ↔ `SEditableTextBox`·`SMultiLineEditableTextBox`)

- **`EditableText`**: 글자 편집만 맡는다. 글자·선택·caret·IME 조합·이력·힌트·클립보드·비밀번호·읽기 전용. 바탕이 없다. 생성자에서 `boxed`를 뺀다(하위호환 없이 바꾼다).
- **`EditableTextBox`**(새 Element, UiFramework `Controls/EditableTextBox`): `EditableText` 하나를 담는 상자다.
  - 상태(보통·hover·초점·읽기 전용)마다 바탕 brush·전경색을 고르고 여백을 둔다(`EditableTextBoxStyle`).
  - 오류 표지와 오류 팝업을 든다(22.6절).
  - 여러 줄이면 세로 `ScrollBar`를 오른쪽에 둔다(22.7절).
  - 초점은 안쪽 `EditableText`가 받는다. 상자 여백은 편집기의 내용 여백(`SetContentPadding`)으로 넘겨 편집기가 상자 안쪽 전체를 덮는다 — 여백을 눌러도 편집기가 맞아 초점을 받고, Tab 정지도 하나다. UE는 상자가 초점을 받아 안쪽으로 넘긴다(`SEditableTextBox::OnFocusReceived`, SEditableTextBox.cpp 309–317행). 코드가 상자에 초점을 주는 경우(화면의 초점 대상, `SetFocus(상자)`)는 `Element::FocusDelegate`(상자 → 편집기)로 넘긴다 — `Application::SetFocus`가 초점을 받지 못하는 요소의 위임을 하위 트리 안에서 따라간다. 상자는 `SupportsKeyboardFocus`가 거짓이라 Tab 정지가 아니다(UE처럼 상자가 초점을 받게 하면 상자와 편집기가 두 정지가 된다 — 버린 대안). 초점 외곽선은 그리지 않는다 — 초점은 상자 테두리 색으로 보인다(UE `SEditableText::GetFocusBrush` = null과 같은 이유).
  - 편집기는 초점·읽기 전용이 바뀌면 `SetOnStateChanged` 콜백으로 상자에 알린다 — 상자가 바탕·전경을 다시 고른다. 상자 소멸자가 이 콜백을 끊는다(편집기가 밖에 붙잡혀 상자보다 오래 살 수 있다).
  - 상태별 전경은 `CurrentForeground()`로 읽을 수 있다(그리기의 `PaintForegroundColor`가 같은 값을 쓴다).
  - 상자는 hover를 자기 것으로 본다(바탕 brush 선택). 상자가 비활성이면 안쪽도 비활성이다.
  - 사용자는 상자의 `Text()`(안쪽 `EditableText&`)로 편집 API를 쓴다. 상자가 같은 setter를 따로 두지 않는다(두 층의 API가 갈라지지 않게 — UE는 `SEditableTextBox`가 대부분을 다시 노출하지만 Rocks는 하나로 둔다).
- **버린 대안**: `boxed` 인자를 유지한다. 상자 기능(hover 바탕·오류 팝업·스크롤 막대)이 편집 로직에 더 섞이고 UE 구조와 다르다.

### 22.3 스타일(UiCore, 14장 4.4절)

- `EditableTextStyle`(UE `FEditableTextStyle`, 키 `NormalEditableText`)
  - `text`: `TextStyle`(글꼴 Normal)
  - `selection`: `ColorRef`(Highlight) — 선택 바탕
  - `caret`: `ColorRef`(White)
  - `composition`: `ColorRef`(전경) — 조합 점선 밑줄. UE "Old/HyperlinkDotted" 테두리 이미지 대신 1 px 점선 brush로 그린다(UE 이미지는 클론에 없다).
  - `hintOpacity`: 0.35 — UE `HintTextOpacity` 기본값(SlateEditableTextLayout.h 79행).
- `EditableTextBoxStyle`(UE `FEditableTextBoxStyle`, 키 `NormalEditableTextBox`)
  - 바탕 `normal` RB(Input, 4, InputOutline, 1), `hovered` RB(Input, 4, Hover, 1), `focused` RB(Input, 4, Primary, 1), `readOnly` RB(Header, 4, InputOutline, 1)
  - `padding` (8, 3, 8, 4)
  - 전경 `foreground` Foreground, `focusedForeground` White, `readOnlyForeground` 상속(UseForeground)
  - `text`: `EditableTextStyle`(위 `NormalEditableText`)
  - `errorMarker`: RB(Error, 4) — "!" 표지 바탕. `errorText`: 글꼴 Normal, 색 Foreground. UE `SPopupErrorText`의 "Box.Error" 대응.
- CoreStyle에 둔다(UE `StarshipCoreStyle`). EditorStyle은 다시 정하지 않는다. GameWidgetStyle(UMG)의 `NormalEditableTextBox`는 이미지(PNG) 키라 20.11절 규칙대로 빈칸으로 두고 코어 값을 쓴다.
- 박힌 색(힌트 회색·오류 빨강)은 지운다.

### 22.4 선택

- **더블클릭 단어 선택**: `clickCount == 2`면 누른 곳의 단어를 고르고 caret을 끝에 둔다(UE `FSlateEditableTextLayout::HandleDoubleClick`·`SelectWordAt`, SlateEditableTextLayout.cpp 1909–1931행).
  - 단어는 UE `FTextLayout::GetWordAt`(TextLayout.cpp 2524–2569행) 규칙이다: 누른 위치 뒤 경계(끝이면 끝)부터 거꾸로 가며 공백이 아닌 글자가 있는 첫 구간이다. 단어 뒤 공백을 눌러도 앞 단어가 되고, 공백은 넣지 않는다.
  - 경계는 `TextShaper::FindWordBoundaries`가 준다. 글꼴 백엔드가 있는 호스트는 ICU 단어 경계(`text::TextBackend::FindBoundaries(Word)` — UE `FTextBreakIterator`)로 재정의한다. 기본 구현은 문자 부류(공백·문장부호·그 밖)가 바뀌는 곳과 문장부호 하나하나에서 나눈다(편집기가 백엔드 없이도 동작하게 — 시험·도구용).
  - 끌면 단어 단위로 넓힌다(처음 고른 단어는 늘 포함). UE는 더블클릭 뒤 끌기를 단어로 넓히지 않는다 — 편집기 관례를 따른 개선이다.
- **단어 이동·삭제**: 같은 경계로 Ctrl(macOS Cmd)+←·→는 단어 단위로 옮긴다(Shift면 선택). 한 글자 옮긴 뒤 줄 처음·끝이나 단어 시작(`GetWordAt`의 시작)에 닿을 때까지 간다(UE `ScanForWordBoundary`·`IsAtWordStart`, SlateEditableTextLayout.cpp 3139–3191행). 선택이 없으면 Ctrl+Backspace는 앞 단어 멈춤까지, Ctrl+Delete는 뒤 단어 멈춤까지 지운다(1176–1255행). 비밀번호는 단어 대신 줄 처음·끝이다.
- **세 번 클릭 줄 선택**: `clickCount >= 3`이면 그 논리 줄(줄바꿈 사이, 한 줄 편집은 전체)을 고른다. UE에는 없다 — Windows·macOS 편집기 관례를 따른 개선이다.
- 비밀번호면 단어 경계가 없으므로 더블클릭도 전체를 고른다(가린 글자의 단어 길이를 드러내지 않는다 — UE도 비밀번호는 단어 이동이 전체 이동이다).

### 22.5 이력·힌트·caret

- **이력**: 99단계(UE `EditableTextDefs::MaxUndoLevels`, TextEditHelper.h 15행). 이름 있는 상수 `MaxUndoSteps = 99`.
- **힌트**: 본문이 비었을 때 상자 전경색 × `hintOpacity`로 그린다(UE SlateEditableTextLayout.cpp 3683행).
- **caret 폭**: clamp(0.08 × 줄 높이, 1, 2) px(UE `CalculateCaretWidth`, TextEditHelper.cpp 15–19행). 지금 1 px 고정을 바꾼다.
- **caret 깜박임**: UE 규칙(SlateEditableTextTypes.cpp 127–130행) 그대로다.
  - 마지막 상호작용(타자·IME·붙여 넣기·caret 이동·포인터 누름) 뒤 0.1초(`CaretBlinkPauseTime`)는 항상 보인다.
  - 그 뒤는 `round(0.5 + 0.5·sin(π/2 + 2π·t))`가 1일 때만 보인다(`MakePulsatingValue(t, BlinksPerSecond = 1)` — 켜짐·꺼짐 사각파, 밝기 감쇠 없음). t는 마지막 상호작용부터 잰 초다.
  - 9.1절 원안의 "밝기 제곱 감쇠"는 UE와 달라 버린다.
  - 시각은 `FindFrameTime().current`(UW8a 프레임 시계 — 시험은 시계를 주입한다)다. 초점 동안만 volatile로 매 프레임 다시 기록한다(20.12절 — volatile 변경은 그 요소만 무효화). UE는 0.5초 active timer로 다시 그리는데, Element는 Application 타이머에 닿지 않으므로 volatile을 쓴다(버린 대안: 타이머 — UiCore 계약에 Element 타이머가 생길 때 다시 본다).

### 22.6 오류 표지와 팝업(UE `SPopupErrorText`)

- 오류가 있으면 상자 오른쪽 안쪽에 "!" 표지(정사각, `errorMarker` 바탕, 흰 "!")를 둔다. 편집 칸은 그만큼 줄어든다.
- 오류 문구는 상자 아래에 붙는 팝업(MenuAnchor 팝업 층)으로 띄운다. 초점을 뺏지 않는다(`SetFocusOnOpen(false)` — UE `SMenuAnchor` `bFocusMenu = false`). 오류가 있는 동안 열려 있다(UE `SPopupErrorText::SetError`가 `SetIsOpen(HasError)`). 상자가 화면에서 빠지면 닫힌다(팝업 층 규칙).
- 문구 shaping은 호출자가 넘긴 `Text`를 상자가 `TextShaper`로 한다(지역화 — 지금 오류 캡션과 같다).

### 22.7 여러 줄

- `EditableText`(multiline): Enter = 줄바꿈, Up·Down 줄 이동(지금 있음), caret 따라가기·휠 세로 스크롤(지금 있음). 스크롤 위치·내용 높이·보이는 높이를 읽는 API(`ScrollOffset`·`ContentHeight`·`ViewHeight`)와 `SetScrollOffset`을 공개한다.
- `EditableTextBox`(multiline): 오른쪽에 세로 `ScrollBar`(UE `SMultiLineEditableTextBox`의 `VScrollBar`, 스타일 "Scrollbar")를 둔다. 내용이 보이는 높이를 넘을 때만 보인다(UE `AlwaysShowScrollbars` 기본 false). 막대를 끌면 `SetScrollOffset`, 편집기가 스크롤하면 막대를 맞춘다.

### 22.8 Widget 층

- `EditableTextWidget`·`MultiLineEditableTextWidget`은 `EditableText`를, `EditableTextBoxWidget`·`MultiLineEditableTextBoxWidget`은 `EditableTextBox`를 만든다. 속성(글자·힌트·오류·비밀번호·읽기 전용·글꼴 크기·전체 선택 설정)을 밀어 넣는 방식은 지금과 같다.
- 오류(`SetError`)는 상자 위젯만 받는다(UE `UEditableTextBox::SetError`). 상자가 아닌 위젯의 오류는 지금처럼 `Unsupported`다.

### 22.9 갤러리(Widgets 탭 "Text Input" 묶음, 16장)

Editable Text, Editable Text Box(힌트), Read Only, Password, Error(표지·팝업), Multi-Line Box(스크롤 막대가 생기는 긴 글), 값 레이블(바뀐 값·확정 사유). 부품 이름은 Rocks 이름으로 적는다(16장 규칙 — UE 클래스 이름을 쓰지 않는다).

### 22.10 시험

- 나눔: 상자의 상태별 바탕(보통·hover·초점·읽기 전용)·전경, 여백, 상자를 누르면 안쪽 초점, 초점 외곽선 없음.
- 스타일: CoreStyle 키 둘과 값, 앱 스타일 교체 뒤 다시 받음.
- 선택: 더블클릭 단어(LTR·한글·숫자 섞임), 끌어 단어 확장, 세 번 클릭 줄(여러 줄의 가운데 줄·한 줄 전체), 비밀번호 더블클릭 전체.
- 이력: 99단계에서 가장 오래된 것이 빠짐.
- 힌트 색 = 전경 × 0.35.
- caret: 폭(작은·큰 글꼴 clamp), 주입 시계로 0.1초 고정·0.25초 경계 켜짐·꺼짐(시각을 정해 두고 단언 — 시간 경쟁 없음), 초점 잃으면 volatile 해제.
- 오류: 표지 유무·편집 칸 폭, 팝업 열림·닫힘, 팝업이 초점을 뺏지 않음.
- 여러 줄 스크롤 막대: 넘칠 때만 보임, 막대 끌기 ↔ 편집기 스크롤 동기화.
- Widget 층: 기존 `EditableTextWidgetTests` 전부와 상자 위젯 오류·스타일.

### 22.11 단계

- **UW4f2a**: 스타일 둘(UiCore)·`EditableTextBox` 나눔·힌트 불투명도·이력 99·caret 폭. Widget 층이 상자를 만든다.
- **UW4f2b**: 더블·세 번 클릭 선택·caret 깜박임.
- **UW4f2c**: 오류 표지·팝업·여러 줄 스크롤 막대.
- **UW4f2d**: 갤러리 "Text Input" 묶음·self-test 화소 확인.

리뷰어는 9.1절대로 UW4f(f2·f3) 끝에 한 번 둔다.
