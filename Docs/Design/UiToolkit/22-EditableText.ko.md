## 22. 편집 텍스트 공개(UW4f2 — 병합 뒤)

9.1절 UW4f2의 세부 설계다. UW9b(21.2절)가 상태 소유(`EditableText`가 글자·선택·조합·이력을 든다)와 셰이퍼 경계(`TextShaper`)를 이미 옮겼다. 이 장은 남은 부분을 정한다. `TextShaper`의 `FontStyle` 서명은 UW4 글꼴 자동 셰이핑과 함께 바꾸므로 여기서 다루지 않는다.

### 22.1 지금 상태(UW9b 뒤)

- `EditableText`(UiFramework) 생성자가 `boxed`를 받는다. 상자 모양이면 같은 Element가 안쪽 여백·바탕·오류 캡션까지 그린다. 편집 로직과 상자 모양이 한 클래스에 섞여 있다.
- 힌트·오류 캡션의 색이 코드에 박혀 있다(힌트 회색, 오류 빨강). 스타일 구조체가 없다.
- 편집 이력은 64단계다(UE 99).
- 더블클릭 단어 선택, 세 번 클릭 줄 선택, caret 깜박임이 없다.
- 여러 줄 상자에 세로 스크롤 막대와 휠 스크롤이 없다. caret 따라가기 스크롤만 있다 — 배치할 때마다 caret을 보이는 곳에 끌어온다(밖에서 스크롤을 옮길 수 없다).
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
- 표지는 `MenuAnchor`다(UE `SPopupErrorText`가 `SComboButton`). 버튼 스타일은 모든 상태가 `errorMarker`이고 여백 0이다. 초점을 받지 않는다(`SetFocusable(false)` — UE `IsFocusable(false)`, Tab 정지가 아니다). 누르면 팝업을 여닫는다(`MenuAnchor::OnActivated` — 초점을 옮기지 않는 `Open`).
- 오류 문구는 표지 아래(`MenuPlacement::Below` — UE `MenuPlacement_BelowAnchor`, 기준은 표지) 팝업으로 띄운다. UE는 표지 바로 아래라 상자 아래쪽(표지 아래 여백)을 덮는다. Rocks는 팝업을 상자 아래 끝까지 내린다(`SetMenuOffset` — 입력 글자를 가리지 않는다, 바뀔 때만 다시 정한다).
  - 팝업 내용은 상자 전용 Element(`EditableTextErrorPopup`, Private)다: `errorMarker` 바탕 위 `errorText` 글자, 여백 (3, 0, 3, 0)(UE `SErrorText`의 `FMargin(3, 0)`). 바탕은 그릴 때 테마로 푼다(테마가 바뀌어도 따라간다 — `Frame`은 푼 `Brush`만 받으므로 쓰지 않는다).
  - 팝업 층은 바깥 누름을 받지 않는다(`SetCatchOutsidePresses(false)`) — 오류 팝업이 다른 입력을 막지 않는다.
  - 초점을 뺏지 않는다(UE `SetIsOpen(bHasError, false)` — 둘째 인자 `bFocusMenu = false`).
- 열고 닫는 때는 UE `SPopupErrorText::SetError`와 같다: `SetError`가 부를 때마다 오류가 있으면 열고 없으면 닫는다. 사용자가 표지를 눌러 닫으면 다음 `SetError`까지 닫혀 있다.
- **트리에 붙기 전에 정한 오류(Rocks 보강)**: UE는 창에 붙기 전 `SetError`의 팝업을 열지 못하고 버린다(Widget 층은 속성을 붙기 전에 넣는다 — 갤러리·UMG에서 팝업이 안 뜬다). Rocks는 상자가 호스트 묶음이 있는 트리에 붙을 때(`OnUiHostServicesChanged`) 오류가 있으면 연다.
  - 붙는 통지 안에서 바로 열지 않는다 — 통지는 트리 순회(루트 교체·목록 행 생성이 배치 중에 일어난다) 안에서 오므로 루트 Overlay 자식 목록을 바꾸면 순회가 깨진다. `UiHostServices::DeferFrameAction`에 넣어 다음 프레임 시작(`Application::StartFrame` — 해체 대기열 다음)에 연다.
  - 대기열 항목은 상자를 약하게 잡는다(그 사이 버려진 상자는 아무 일도 하지 않는다). 한 상자는 대기 항목을 하나만 둔다.
  - 상자가 화면에서 빠지면 팝업은 닫힌다(Overlay `OnDescendantDetached` — 팝업 층 규칙). 다시 붙으면 위 규칙으로 다시 연다.
  - **숨은 anchor의 팝업(Rocks 보강 — 모든 `MenuAnchor` 팝업)**: 트리에는 남았지만 보이지 않는 anchor(고르지 않은 탭 — `ActiveChildren` 밖, 접히거나 숨은 조상)의 팝업은 팝업 호스트(Overlay)가 배치할 때 층을 접고(Collapsed), 다시 보이면 처음 보이기로 되돌린다. 판정은 anchor에서 호스트까지 `IsInputPathActive(host, 키 경로, 활성 무시)`다. 오류 팝업이 다른 탭 위에 떠 있지 않고 탭으로 돌아오면 다시 보인다. UE는 숨은 anchor가 Tick되지 않아 팝업 창이 옛 자리에 남는다(편집기는 탭 누름으로 메뉴를 닫아 가린다).
  - 갤러리 self-test·스냅샷 루트도 실제 창처럼 Overlay(팝업 호스트)로 감싼다.
- 문구 shaping은 호출자가 넘긴 `Text`를 상자가 `TextShaper`로 한다(지역화 — 지금 오류 캡션과 같다).
- **`UiHostServices` 프레임 동작 대기열**(새 계약): `Result<void> DeferFrameAction(Delegate<void()>&&)`·`usize RunFrameActions()`·`PendingFrameActionCount()`. 해체 대기열(21.4절)과 같은 방식(넣은 순서, 도는 중 넣은 항목은 다음 프레임, 재진입 0)이지만 사용자 hook 해체가 아니라 "트리 순회 밖에서 할 일"이다. `Shutdown`에서는 돌리지 않고 버린다(닫는 앱에서 팝업을 열지 않는다). 빈 delegate는 `InvalidArgument`, 할당 실패는 `OutOfMemory`다.

### 22.7 여러 줄

- `EditableText`(multiline): Enter = 줄바꿈, Up·Down 줄 이동(지금 있음).
- **caret 따라가기**는 UE `PositionToScrollIntoView`(SlateEditableTextLayout.cpp 3596–3631행) 규칙으로 바꾼다: caret이 움직이거나(이동·선택·타자·조합) 글자가 다시 셰이핑될 때만 다음 배치에서 한 번 따라간다. 가로는 caret 사각형(폭 = caret 폭), 세로는 caret이 있는 줄 전체를 보이게 한다. 지금처럼 배치마다 끌어오면 막대·휠로 옮긴 스크롤이 다음 배치에 되돌아간다.
- **clamp**: 배치마다 스크롤을 [0, max(0, 내용 − 보이는 크기)]로 자른다(UE `SetScrollOffset` 848–849행·막대 갱신 3636–3659행). 내용 크기는 줄 배치 크기(가로는 caret 폭을 더한다)다. 내용이 보이는 크기보다 작으면 0이다.
- **휠**: 여러 줄이고 넘칠 때만 `WheelScrollStep`(px, Rocks 공통 휠 거리 — UE `GetGlobalScrollAmount`)씩 세로로 옮긴다. 옮겼으면 처리하고, 끝이라 못 옮기면 처리하지 않는다(바깥 스크롤로 넘어간다 — UE 864–883행).
- **API**(여러 줄·한 줄 모두, 세로 값): `float ScrollOffset() const`, `float ContentHeight() const`(줄 배치 높이), `float ViewHeight() const`(배치 높이 − 위·아래 내용 여백), `void SetScrollOffset(float)`(clamp, 비유한 값은 무시). 마지막 배치 기준이다(배치 전에는 0).
- 오른쪽 단추 끌기 스크롤(UE `SMultiLineEditableText::OnMouseMove` 823–857행)은 편집 문맥 메뉴와 함께 둔다 — UE도 끈 양으로 문맥 메뉴와 가른다. 편집 문맥 메뉴는 아직 없다(UE `FSlateEditableTextLayout::BuildDefaultContextMenu` — 실행 취소·잘라내기·복사·붙여 넣기·지우기·전체 선택). 둘을 UW4f2e에서 같이 넣는다(22.11절).
- `EditableTextBox`(multiline): 오른쪽에 세로 `ScrollBar`(UE `SMultiLineEditableTextBox`의 `VScrollBar`, 스타일 "Scrollbar")를 둔다.
  - 내용이 보이는 높이를 넘을 때만 보인다(UE `AlwaysShowScrollbars` 기본 false, `IsNeeded`). 숨으면 자리를 차지하지 않는다(Collapsed).
  - 놓는 순서는 편집 칸 | 막대 | 오류 표지다(UE `SMultiLineEditableTextBox` 가로 상자 순서). 막대 세로는 위·아래 여백 안이다.
  - 배치: 편집기를 막대 없이 먼저 배치하고, 넘치면 막대 폭만큼 줄여 다시 배치한다(줄바꿈이 바뀌어도 더 넘칠 뿐이라 두 번이면 끝난다 — 결정적). UE는 다음 Tick에 막대를 보이고 한 프레임 흔들린다.
  - 동기화: 상자 배치가 편집기 스크롤로 막대 `SetState(offset / 내용, 보이는 / 내용)`을 맞춘다(편집기 스크롤이 바뀌면 배치 무효화가 상자까지 올라온다). 막대를 끌면 `onScrolled(fraction)` → `SetScrollOffset(fraction × 내용)`.

### 22.8 Widget 층

- `EditableTextWidget`·`MultiLineEditableTextWidget`은 `EditableText`를, `EditableTextBoxWidget`·`MultiLineEditableTextBoxWidget`은 `EditableTextBox`를 만든다. 속성(글자·힌트·오류·비밀번호·읽기 전용·글꼴 크기·전체 선택 설정)을 밀어 넣는 방식은 지금과 같다.
- 오류(`SetError`)는 상자 위젯만 받는다(UE `UEditableTextBox::SetError`). 상자가 아닌 위젯의 오류는 지금처럼 `Unsupported`다.

### 22.9 갤러리(Widgets 탭 "Text Input" 묶음, 16장)

Editable Text, Editable Text Box(힌트), Read Only, Password, Error(표지·팝업), Multi-Line Editable Text(힌트 — UE 갤러리 행), Multi-Line Editable Text Box(스크롤 막대가 생기는 긴 글), 값 레이블(바뀐 값·확정 사유 — 힌트 상자 행 옆). 부품 이름은 Rocks 이름으로 적는다(16장 규칙 — UE 클래스 이름을 쓰지 않는다). 순서는 UE 갤러리대로 숫자 입력 다음이다(SStarshipGallery.cpp 1365–1405행).

- 셰이퍼는 데모 `DemoTextShaper`(`ui::TextShaper` 구현 — DemoText 글꼴 조합, 크기는 96 DPI 포인트, 단어 경계는 ICU `FindBoundaries(Word)`)다. 클립보드는 창 세션(`platform::ReadClipboardText`·`WriteClipboardText`)이다.
- self-test(`VerifyGallery`, 실패 번호 54–60): 오류 팝업이 열리고 안에 Error 바탕, 여러 줄 상자 막대 보임, 상자를 누르면 안쪽 편집기 초점·Primary 외곽선 바탕·흰 caret, "Hi" 타자가 값 레이블 "Changed: Hi", Enter가 "Committed (Enter): Hi".

### 22.10 시험

- 나눔: 상자의 상태별 바탕(보통·hover·초점·읽기 전용)·전경, 여백, 상자를 누르면 안쪽 초점, 초점 외곽선 없음.
- 스타일: CoreStyle 키 둘과 값, 앱 스타일 교체 뒤 다시 받음.
- 선택: 더블클릭 단어(LTR·한글·숫자 섞임), 끌어 단어 확장, 세 번 클릭 줄(여러 줄의 가운데 줄·한 줄 전체), 비밀번호 더블클릭 전체.
- 이력: 99단계에서 가장 오래된 것이 빠짐.
- 힌트 색 = 전경 × 0.35.
- caret: 폭(작은·큰 글꼴 clamp), 주입 시계로 0.1초 고정·0.25초 경계 켜짐·꺼짐(시각을 정해 두고 단언 — 시간 경쟁 없음), 초점 잃으면 volatile 해제.
- 오류: 표지 유무·편집 칸 폭, 팝업 열림·닫힘(`SetError` 마다), 팝업이 초점을 뺏지 않음, 표지가 Tab 정지 아님, 표지 눌러 닫기, 붙기 전 오류는 다음 프레임에 열림, 떼면 닫히고 다시 붙이면 열림, 프레임 동작 대기열(순서·재진입·버린 상자·Shutdown 버림).
- 스크롤: 막대·`SetScrollOffset`으로 옮긴 스크롤이 다음 배치에 유지됨, caret 이동이면 따라감, clamp, 휠(넘칠 때만·끝에서 미처리).
- 여러 줄 스크롤 막대: 넘칠 때만 보임(숨으면 편집 칸 폭 그대로), 막대 끌기 ↔ 편집기 스크롤 동기화, 표지와 함께일 때 순서.
- Widget 층: 기존 `EditableTextWidgetTests` 전부와 상자 위젯 오류·스타일.

### 22.11 단계

- **UW4f2a**: 스타일 둘(UiCore)·`EditableTextBox` 나눔·힌트 불투명도·이력 99·caret 폭. Widget 층이 상자를 만든다.
- **UW4f2b**: 더블·세 번 클릭 선택·caret 깜박임.
- **UW4f2c**: 오류 표지·팝업·여러 줄 스크롤 막대.
- **UW4f2d**: 갤러리 "Text Input" 묶음·self-test 화소 확인.
- **UW4f2e**: 편집 문맥 메뉴(오른쪽 단추 — UE 기본 메뉴 항목·활성 조건)와 여러 줄 오른쪽 단추 끌기 스크롤(끈 양이 문턱 아래면 메뉴, 넘으면 스크롤 — UE 규칙). 상세는 착수 때 22.7절 뒤에 쓴다.

리뷰어는 9.1절대로 UW4f(f2·f3) 끝에 한 번 둔다.
