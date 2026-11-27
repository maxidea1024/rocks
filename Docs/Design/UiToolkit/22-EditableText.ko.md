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

### 22.7a 편집 문맥 메뉴와 오른쪽 끌기 스크롤(UW4f2e)

- **오른쪽 누름**(UE `FSlateEditableTextLayout::HandleMouseButtonDown` 1320–1366행): 조합 중이면 무시한다(지금 포인터 규칙). 선택이 있고 누른 자리가 선택 밖이면 선택을 풀고 caret을 누른 자리로 옮긴다. 선택 안을 누르면 그대로 둔다(그 선택을 잘라내기·복사한다). 포인터를 잡는다(capture). 초점은 Application 누름 규칙으로 편집기에 온다(UE `SetUserFocus(Mouse)`). 끈 양은 0으로 둔다.
- **오른쪽 끌기 스크롤**(UE `SMultiLineEditableText::OnMouseMove` 823–857행, `IsRightClickScrolling` 914–917행)
  - 여러 줄이고 넘칠 때만이다(ContentHeight > ViewHeight — UE `VScrollBar->IsNeeded`). 세로 이동량의 절댓값을 누적한다.
  - 누적이 `PointerDragThreshold`(5 px — UE `GetDragTriggerDistance`) 이상이면 스크롤 중이다. 스크롤을 포인터 이동만큼 반대로 옮긴다(`scroll -= dy` — 내용을 잡아 끄는 방향).
  - 커서: UE는 숨기고 놓을 때 누른 자리로 되돌린다(high precision mouse, software cursor). Rocks에는 Platform 커서 숨김·위치 hook이 아직 없어 커서를 그대로 둔다. hook은 UW4n(숫자 입력 끌기 — 같은 요구)에서 생기면 이 규칙에도 넣는다.
- **오른쪽 놓기**(UE `HandleMouseButtonUp` 1436–1470행, `SMultiLineEditableText::OnMouseButtonUp` 803–821행): capture를 놓는다. 스크롤 중이었으면 메뉴를 열지 않는다. 아니고 놓은 자리가 편집기 안이면 문맥 메뉴를 놓은 자리에 연다.
- **키보드로 열기(Rocks 보강)**: Shift+F10이면 caret 자리(caret 줄 아래)에 연다(Windows 관례 — UE 편집 텍스트에는 없다). 메뉴 키(Apps)는 `input::Key`에 없어 넣지 않는다.
- **메뉴 항목**(UE `BuildDefaultContextMenu` 3808–3852행): 묶음 "Modify Text"(제목 — ASCII 대문자로 보임)에 Undo, 다음 묶음 Cut·Copy·Paste·Delete, 다음 묶음 Select All. 단축키 표시는 Ctrl+Z·Ctrl+X·Ctrl+C·Ctrl+V·Delete·Ctrl+A다(macOS는 Ctrl 대신 Cmd — 호스트가 `Platform::commandIsSuper`로 알린다. UiFramework는 플랫폼 매크로를 쓰지 않는다. 키 처리는 Ctrl·Cmd 둘 다 받는다).
- **사용 조건**(UE `CanExecute*`)
  - Undo: 읽기 전용이 아니고 조합 중이 아니다(3359–3367행 — 이력이 비어도 켜져 있고, 실행은 이력이 있을 때만 한다).
  - Cut: 읽기 전용·비밀번호가 아니고 선택이 있다(1960–1983행).
  - Copy: 비밀번호가 아니고 선택이 있다(2013–2030행).
  - Paste: 읽기 전용이 아니고 클립보드에 글자가 있다(2054–2073행). UE는 항목을 그릴 때마다 클립보드를 읽는다. Rocks는 메뉴를 열 때 한 번 읽은 값으로 정한다(메뉴가 열린 동안 다른 앱이 클립보드를 바꾸는 경우만 다르다).
  - Delete: 읽기 전용이 아니고 선택이 있다(1741–1758행).
  - Select All: 늘 켜져 있다(1881–1884행).
  - 실행은 키 동작과 같은 함수다(사용자 편집 — 통지·이력). 항목을 실행하면 메뉴가 닫힌다.
- **초점**
  - 메뉴는 열 때 초점을 받는다(UE `PushMenu(bFocusImmediately = true)`). 마우스로 열면 Pointer(외곽선 없음), Shift+F10이면 Navigation(첫 항목 외곽선)이다.
  - 메뉴가 열린 동안 편집기의 초점 잃음·받음은 아무 일도 하지 않는다: 확정·선택 해제·IME 끄기·전체 선택·초점 글자 기억 모두 없다(UE `HandleFocusReceived`·`HandleFocusLost`의 ActiveContextMenu 검사 871·934행). caret은 그리지 않고 선택은 그대로 그린다.
  - 메뉴가 닫히면 초점은 편집기로 돌아온다(MenuAnchor 초점 복원 — UE `OnContextMenuClosed` 2219–2224행). 사용자가 메뉴 밖을 눌러 다른 요소로 초점을 옮겼으면 편집기는 그때 초점 잃음을 처리한다(확정).
- **메뉴 자리**: 편집기 안에 크기 0의 `MenuAnchor`를 메뉴가 열린 동안만 자식으로 두고 연 자리에 배치한다(`MenuPlacement::Below` — 메뉴 왼쪽 위가 연 자리, 창에 맞춤). 닫히면 뗀다(편집기 자식 수는 평소대로 레이블·힌트다).
- **Application 찾기**: 메뉴 문맥(`MenuContext`)에 Application이 필요하다. `Application::Find(const Element&)`(UiFramework 정적 함수)를 이번에 둔다. `UiHostServices`에 소유자 표지(`SetOwner(kind, owner)`·`FindOwner(kind)` — UiCore는 형식을 모르므로 표지 주소가 같을 때만 돌려준다)를 두고, Application이 자기 묶음에 자기를 단다(소멸 때 뗀다). 트리에 붙지 않았거나 Application이 없으면 null이고 메뉴를 열지 않는다. 16.5절 `Element::FindApplication`의 기반이다 — UiCore Element는 Application 형식을 모르므로 이름은 `Application::Find`다. 다른 API 이전(목록·화면 컨테이너 등)은 UW4a2 몫이다.
- 메뉴 글자는 편집기 셰이퍼로 shaping한다(메뉴 스타일 글꼴 크기 — 글꼴 종류는 셰이퍼 서명이 바뀔 때, 22장 첫 문단).
- **초점 받음의 스크롤(UE와 맞춤)**: 초점을 받을 때 전체 선택(`SetSelectAllTextWhenFocused`)으로 caret이 움직여도 스크롤하지 않는다(UE `HandleFocusReceived` 912–917행 `PositionToScrollIntoView.Reset`).
- 시험: 선택 밖·안 오른쪽 누름, 놓은 자리에 메뉴, 항목 사용 조건(읽기 전용·비밀번호·선택·클립보드·조합), 항목 실행(잘라내기·복사·붙여 넣기·지우기·전체 선택·되돌리기와 통지), 메뉴 초점과 복원, 메뉴 동안 초점 잃음이 확정하지 않음, 끌기 문턱(4 px는 메뉴, 6 px는 스크롤), 한 줄은 끌어도 메뉴, Shift+F10, `Application::Find`, 초점 받음 전체 선택이 스크롤하지 않음.

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

### 22.12 IME(UW4f3 — 9.1절 UW4f3을 대신한다)

9.1절 UW4f3 원안(2026-11-25)은 UW7h(19.2절) 전에 썼다. 그 뒤 바뀐 것과 남은 것은 아래와 같다.

- **이미 된 것(UW7h — 19.2절).**
  - 조합 창(`CFS_POINT`, caret 아래)·후보 창(`CFS_EXCLUDE`, caret 줄을 피함)은 `NotifyLayoutChanged` → 창이 문맥의 `GetTextBounds`를 당겨 놓는다(Win64Window `UpdateImmWindowPositions`). 원안의 `Platform::setCompositionArea`는 이 pull 방식으로 대신했다.
  - 초점 잃음은 조합을 확정한다(Deactivate가 `CPS_COMPLETE` 뒤 확정을 처리하고, 그래도 조합 중이면 미리보기를 확정한다).
  - 조합 글자는 본문에 끼워 그린다(미리보기 층 — 위젯 글자·이력·변경 알림 없음).
  - 확정·취소·빈 갱신 순서는 `TextInputSession` 규칙이다.
- **UW4f3a: 조합 중 포인터 누름 확정**(UE `FSlateEditableTextLayout::MoveCursor` 2386–2395행 — 마우스로 caret을 옮기면 문맥을 Deactivate·Activate)
  - 조합 중 왼쪽·오른쪽 누름은 먼저 조합을 확정한다. 서비스를 Deactivate한 뒤 다시 Activate한다. Deactivate가 OS 조합을 완료하고(`CPS_COMPLETE`) 확정을 적용한다. 한국어에서 취소하면 쓰던 음절이 사라지므로 취소가 아니라 확정이다.
  - 그다음 누름을 평소대로 처리한다(caret 이동·단어·줄 선택·문맥 메뉴). 지금은 조합 중 누름을 삼켜 아무 일도 하지 않는다.
  - 서비스가 없거나 Deactivate 뒤에도 조합 중이면 미리보기를 그대로 확정한다(`EndComposition` — 19.2절 Deactivate 규칙과 같다).
  - 휠은 지금처럼 조합 중에도 스크롤한다. 조합 중 키보드 caret 이동은 IME 몫이다(지금 규칙 — UE 2257행과 같다).
- **UW4f3a: 조합 점선 밑줄**(UE `FTextCompositionHighlighter::OnPaint` — SlateEditableTextTypes.cpp 170–194행, brush는 `FEditableTextStyle::CompositionBackground` "Old/HyperlinkDotted", 색은 전경 × tint)
  - 조합 구간의 글자 아래에 1 px 두께 점선을 긋는다(지금은 실선). 칠 1 px·빔 1 px를 되풀이한다(`CompositionDashLength`·`CompositionGapLength`).
  - 무늬 위상은 줄 왼쪽 끝 기준이다. 클러스터 경계에서 무늬가 끊기거나 겹치지 않는다.
  - 색은 `EditableTextStyle::composition`(전경)이다. UE 이미지는 클론에 없어 상자 조각으로 그린다(22.3절).
  - 시험: 조합 구간 폭 / 2개 조각, 위상, 색.
- **UW4f3b: TSF**(19.2절 "TSF" — 세부는 22.13절).
- 사람 확인 목록: 실제 한국어 IME(두벌식 조합·후보 창 위치·한자 변환·누름 확정), 일본어 IME 변환 구간.

### 22.13 TSF(UW4f3b — UE `FWindowsTextInputMethodSystem`·`FTextStoreACP`)

**왜.** 지금 Win64 IME는 IMM뿐이다. Windows 10·11의 기본 한국어·일본어·중국어 입력기는 TSF 입력기(TIP)이고, IMM 앱에는 CUAS 흉내로 붙는다. UE는 활성 입력기가 TIP이면 TSF로, 옛 IMM 입력기(키보드 배치 HKL)이면 IMM으로 받는다(WindowsTextInputMethodSystem.cpp `FTSFActivationProxy::OnActivated`). 재변환·음성·필기·이모지 패널 같은 TSF 기능은 TSF 문서가 있어야 제대로 된다.

**구조 — 거울 문서(19.2절 Mac 설계와 같은 원칙).**
- TSF는 문서를 동기로 읽고 고친다(`ITextStoreACP` — 잠금 안의 `GetText`·`SetText`·`GetSelection`·`InsertTextAtSelection`). 이 호출에 UI 문맥을 바로 고치면 입력 큐에 남은 키·문자와 순서가 어긋난다(IMM 기록이 지키는 19.2절 규칙).
- 그래서 창이 문맥의 **거울 문서**(`TextInputDocument` — PlatformWindow Private/Common, OS 호출 없음, EngineTests로 시험)를 두고 TSF에 그것으로 답한다. TSF가 고친 내용은 잠금을 풀 때 `TextInputSession` 기록(아래 TSF 기록)으로 바꿔 입력 큐 표지에 싣는다. 게임 lane이 표지 자리에서 문맥에 적용한다(IMM과 같은 길).
  - 거울은 OS 콜백 안에서 할당하지 않도록 고정 용량이다(`MaxTextInputDocumentCodeUnits` 16384 code unit). 문맥 글자가 더 길면 그 문맥에는 TSF 문서를 만들지 않고 IMM으로 받는다(경고 로그 한 번).
  - 글자 위치(`GetTextExt`·`GetScreenExt`·`GetACPFromPoint`)는 문맥에 바로 묻는다(읽기만 — UI 상태를 바꾸지 않는다). 거울이 문맥보다 앞서 있으면(적용 전 기록이 있으면) 조합 시작 위치로 근사한다.
  - 그래서 19.2절의 "TSF 동기 질의는 펌프 중에만, lane 대기 펌프면 `TS_E_SYNCHRONOUS`" 규칙은 필요 없다 — 거울과 읽기 전용 위치 질의는 언제 불려도 UI를 고치지 않는다. 19.2절을 이 절로 고친다. `TS_E_SYNCHRONOUS`는 잠금 안에서 다시 동기 잠금을 청할 때만 쓴다(`ITextStoreACP` 규약).
- **거울 맞추기.** 게임 lane이 Activate·`NotifyTextInputLayoutChanged`(편집기가 배치할 때마다)·표지 처리 뒤에 문맥의 글자·선택을 읽어 거울과 비교한다. 다르면 거울을 바꾸고 TSF에 알린다(`ITextStoreACPSink::OnTextChange`·`OnSelectionChange` — UE `NotifyTextChanged`·`NotifySelectionChanged`). 배치가 바뀌면 `OnLayoutChange(TS_LC_CHANGE)`다. 적용 전 기록이 있으면(거울이 앞섬) 맞추지 않는다.
  - `platform::TextInputClient`에 `GetTextLength`·`GetText(범위)`·`GetCharacterIndexFromPoint`·`GetScreenBounds`를 더한다(`ui::TextInputContext`에 이미 있다 — 데모 어댑터가 넘긴다).
- **기록으로 바꾸기**(잠금을 풀 때 — 거울의 바뀐 것을 그대로 옮긴다). IMM 기록(조합·확정·끝)은 "선택을 조합 범위로 잡아 통째로 바꾼다"는 IMM 모델이라 TSF 편집(조합 밖 고침·재변환·조합 안 부분 고침)을 담지 못한다. 그래서 TSF용 기록 다섯을 더한다(위치는 앞 기록을 적용한 뒤의 글자 기준).
  - `CompositionBegin`: 조합을 시작한다(이미 조합 중이면 없음).
  - `TextReplace(범위, 글자)`: 조합 중이면 미리보기를 고치고, 아니면 Begin·SetText·End로 확정 편집 하나다(이력·변경 알림 한 번 — 자동 고침·이모지 패널).
  - `CompositionRange(범위)`: 조합 범위(밑줄)다. 조합 중이 아니면 시작한다.
  - `CompositionCommit`: 조합을 확정하고 끝낸다.
  - `Selection(범위)`: 선택(조합 중이면 미리보기 안 caret)을 옮긴다.
  - 잠금을 풀 때 거울을 기준(문맥이 기록 적용 뒤 가질 상태)과 견주어 차례로 낸다: 이번에 조합이 시작됐으면 `CompositionBegin` → 글자가 달라졌으면 공통 앞·뒤를 뺀 한 구간 `TextReplace` → 조합 중이면 범위가 달라졌을 때 `CompositionRange` → 이번에 조합이 끝났으면 `CompositionCommit` → 선택이 기준과 다르거나 글자가 달라졌으면 `Selection`. 그다음 기준을 거울로 옮긴다.
  - 넘치면(기록 버퍼) 조합을 끝내고(`TerminateComposition`) 거울을 문맥으로 다시 맞춘다(그 동안의 편집은 잃는다 — IMM 넘침 규칙과 같다).
- **잠금**(`RequestLock`, UE FTextStoreACP::RequestLock): 잠겨 있지 않으면 바로 허락하고(`OnLockGranted`) 풀 때 기록을 낸다. 잠긴 채 동기 청은 `TS_E_SYNCHRONOUS`, 비동기 청은 허락 뒤에 한 번 더 준다(`TS_S_ASYNC`, 읽기 → 쓰기 올림 포함).
- **TSF와 IMM 고르기**(UE와 같다)
  - 창을 만드는 스레드(게임 lane)에서 COM STA를 연다(`CoInitializeEx(COINIT_APARTMENTTHREADED)`). 이미 다른 모드면(`RPC_E_CHANGED_MODE`) TSF 없이 IMM만 쓴다.
  - 스레드마다 `ITfThreadMgr`(Activate)·입력기 프로필 sink(`ITfInputProcessorProfileActivationSink`)를 둔다. 활성 입력기가 TIP이면 TSF, 키보드 배치(옛 IMM 입력기)면 IMM이다.
  - 문맥이 없을 때는 빈 문서 관리자(`disabled`)에 초점을 둔다. 그래서 TSF 입력기도 게임 문자 입력에 `WM_CHAR`를 보낸다(지금 IMM 경로와 같은 결과).
  - TSF로 받는 동안 IMM 메시지(`WM_IME_COMPOSITION` 등)는 기록하지 않고 OS 기본 처리로 넘긴다.
- **문맥마다**(Activate·Deactivate): `CreateDocumentMgr` → `CreateContext(store)` → `Push` → `SetFocus(문서)`. Deactivate는 조합을 확정하고(`ITfContextOwnerCompositionServices::TerminateComposition` — IMM `CPS_COMPLETE`와 같은 결과), 그 잠금이 낸 기록을 처리한 뒤 `Pop`·`SetFocus(disabled)`다. 비밀번호·읽기 전용 문맥은 TSF 문서를 만들지 않는다(IME 끔 — IMM과 같다). Cancel(앱이 조합 중에 글자를 바꿈)은 조합을 끝내고 남은 기록을 버린 뒤 거울을 다시 맞춘다.
- **조합 sink**(`ITfContextOwnerCompositionSink`): `OnStartComposition`은 허락하고 조합 범위를 거울에 표시한다. `OnUpdateComposition`은 새 범위(`ITfRangeACP::GetExtent`), `OnEndComposition`은 끝 표시다. 셋 다 잠금 안에서 오므로 풀 때 기록이 된다.
- **속성·embedded**: 지원 속성은 없음(`RequestSupportedAttrs` 등은 빈 결과), embedded는 `E_NOTIMPL`(UE와 같다).
- **시험**: `TextInputDocument`(OS 없음) — 거울 편집(바꿈·삽입·선택·조합 범위 따라가기·잘못된 범위·용량), 조합 시작·갱신·끝·취소, 조합 밖 삽입, 재변환(선택한 글자 위 조합), 선택만 바뀜이 기록 순서로 나오는지, 거울 맞추기 통지, 용량 넘침. `TextInputSession` TSF 기록 다섯의 적용(조합 중·밖). 실제 TSF(한국어·일본어 TIP, 재변환, 이모지 패널, IMM 입력기 전환)는 사람 확인 목록에 둔다.

### 22.11 단계

- **UW4f2a**: 스타일 둘(UiCore)·`EditableTextBox` 나눔·힌트 불투명도·이력 99·caret 폭. Widget 층이 상자를 만든다.
- **UW4f2b**: 더블·세 번 클릭 선택·caret 깜박임.
- **UW4f2c**: 오류 표지·팝업·여러 줄 스크롤 막대.
- **UW4f2d**: 갤러리 "Text Input" 묶음·self-test 화소 확인.
- **UW4f2e**: 편집 문맥 메뉴와 여러 줄 오른쪽 단추 끌기 스크롤(22.7a절), `Application::Find`.
- **UW4f3a**: 조합 중 누름 확정·조합 점선 밑줄(22.12절).
- **UW4f3b**: TSF(22.12절).

리뷰어는 9.1절대로 UW4f(f2·f3) 끝에 한 번 둔다.
