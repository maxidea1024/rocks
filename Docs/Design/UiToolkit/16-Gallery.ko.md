## 16. UI 갤러리 구성

UiDemo가 보여 주는 UI 갤러리의 페이지 구성이다(9.1절 UW4의 상세). 목적은 셋이다.

- **골라 쓰기**: 에디터를 지을 때 쓸 수 있는 부품과 스타일을 한눈에 보고 고르게 한다(사용자 요청 2026-11-25). 그래서 견본마다 UE 이름과 Rocks 이름(클래스·스타일 함수)을 함께 적는다.
- **UE 대조**: UE 5.8 `SStarshipGallery`(AppFramework/Private/Framework/Testing/SStarshipGallery.cpp)와 탭·행 순서를 맞춰, 나란히 놓고 룩과 동작을 비교한다.
- **빈칸 찾기**: 에디터 부품 중 없는 것을 드러낸다. 없는 부품은 갤러리에 흉내 내지 않고 15장 빈칸 목록에 두며, 만들면 이 장의 행이 생긴다.

### 16.1 원칙

- 견본은 실제 부품이다. 갤러리 전용 그리기 코드로 모양만 비슷하게 만들지 않는다. 에디터가 같은 클래스·스타일을 그대로 쓸 수 있어야 한다.
- 모든 위젯 행에 활성 체크 상자를 둔다(UE `NextSlot`). 끄면 그 행을 `SetEnabled(false)`로 바꿔 UE 비활성 효과(알파 0.45, 비활성 전용 brush)를 보인다.
- 이름 열은 위에 UE 이름표(UE 갤러리 문구), 아래에 Rocks 이름(작은 글꼴, 흰색 25%)을 둔다. 예: "Primary Button" / `Button + PrimaryButtonStyle()`.
- 상태가 있는 부품은 상태를 나란히 보인다(체크 상자 해제·체크·미정, 진행 막대 값·marquee).
- 동작은 실제로 된다. 누르면 눌리고, 고르면 바뀌고, 끌면 움직인다. 바뀐 값은 옆 레이블에 보인다(UE 갤러리의 콤보 레이블처럼).

### 16.2 탭

탭 묶음 하나에 아래 순서로 둔다. 처음 앞에 오는 탭은 UE처럼 "Widgets"다.

| 탭 | UE 대응 | 내용 | 단계 |
|---|---|---|---|
| Colors | "Colors" | 테마 slot 견본(BASE·FOREGROUND·ACCENT, 이름·sRGB 16진) | UW4a |
| Text | "Text"(확장) | 16.3절 | UW4a |
| Widgets | "Starship Widgets" | 16.4절 | UW4a–UW4d |
| Lists | "List Widgets" | 16.5절 | UW4a·UW4c |
| Layout | (없음 — Rocks 추가) | 16.6절 | UW4a·UW4c |
| Editor Parts | (없음 — Rocks 추가) | 16.7절 | UW4d1–UW4d3, 그 뒤 |

UE "Icons" 탭(엔진 아이콘 폴더 PNG 목록)은 두지 않는다. SVG·아이콘 이름표가 없다(15장). SVG 단계가 끝나면 이 탭을 다시 넣는다. 탭 역할은 UE 갤러리처럼 Panel이다.

### 16.3 Text

UE 에디터 글꼴 지정(14장 2절)을 모두 견본으로 보인다. 한 행은 [스타일 이름(UE 키)][typeface·크기][견본 문장]이다.

- **핵심 글꼴**: Normal 10, NormalItalic 10, NormalBold 10, NormalBoldItalic 10, Small 8, SmallBold 8, Large 14, LargeBold 14.
- **제목**: HeadingMedium 33, HeadingSmall 21, HeadingExtraSmall 15(BoldCondensed).
- **용도별**: ToolTip.Font 9, HintText(Italic 8, 흐린 전경색), MonospacedText 10·Log 9(Mono), Menu.Heading(Bold 7, 자간 250, White25), Menu.Keybinding 8, PrimaryButtonText(Bold 10), NotificationList Bold·Light 12, PropertyWindow Small·SmallBold 8, DetailsView.Category SmallBold 8, DetailsView.Name 11(흰색).
- **typeface 목록**: UE 기본 합성 글꼴의 typeface(Regular·Medium·Light·VeryLight·Bold·Black·Italic·BoldItalic·BlackItalic·BoldCondensed·BoldCondensedItalic·Mono)를 14 크기로 한 줄씩.
- **문자 체계**: 라틴, 한글(나눔고딕 보조 typeface), 아랍 문자(오른쪽에서 왼쪽, Noto Naskh), 섞인 방향(BiDi), 이모지(마지막 fallback).
- **문단 기능**: 줄바꿈 폭, 정렬(왼쪽·가운데·오른쪽), 말줄임, 밑줄·취소선, 그림자, 자간.
- **원래 UE 텍스트 탭**의 세 열(Normal·Small / Bold 두 크기 / 보조 typeface "SUB-TYPEFACES - MATH"·이모지, 591–605행)은 위쪽에 그대로 두고, Rocks 데모 글꼴의 한글·아랍 문자 열을 넷째 열로 더한다.
- **편집·강조**: rich text(굵게·기울임·색 구간 — Widget 층 `RichTextBlockWidget`, Element 층 공개 단계에서), 편집 텍스트의 선택 영역·caret·힌트 텍스트, 검색어 강조(NormalText HighlightShape = AccentGreen, 1257행), Mono 로그 견본(Log.Normal Mono 9, 901–906행, 수준별 색).
- 견본 문장(한글·아랍 문자·이모지 포함)은 소스 문자열이 아니라 데모 문자열 파일에서 읽는다(소스 문자열 영어 정책 — UW5).

글꼴 형식: `Typeface` 열거를 UE 합성 글꼴 typeface 전부(Regular·Medium·Light·VeryLight·Bold·Black·Italic·BoldItalic·BlackItalic·BoldCondensed·BoldCondensedItalic·Mono)로 늘린다(UW4a). `FontStyle`에 자간(1/1000 em, UE LetterSpacing)을 더한다.

글꼴 자료: 시험 글꼴 패키지 TestFontRoboto(Roboto 3.016 릴리스에서 byte 그대로 꺼냄)에 UE typeface에 맞는 파일을 둔다(Regular·Medium·Light·Bold·Black·Italic·BoldItalic·BlackItalic, RobotoCondensed-Bold·BoldItalic). VeryLight는 UE처럼 Light 파일이다. Mono는 UE가 DroidSansMono를 쓰므로 같은 글꼴을 같은 방식(출처·해시·라이선스 기록)으로 들여오고, 구하지 못하면 대체 글꼴과 그 이유를 이 절에 적는다. 엔진 기본 글꼴 에셋(글꼴 설계 FT3)이 생기면 그것으로 바꾼다.

### 16.4 Widgets

UE 갤러리 행 순서(824–1480행)를 따르고, UE 갤러리에 없는 Rocks 부품은 뒤에 둔다. 상태 열은 이 장을 쓸 때의 것이다.

| 행(UE 문구) | UE 부품 | Rocks | 상태 |
|---|---|---|---|
| Primary Button | `SButton` "PrimaryButton" | `Button` + `PrimaryButtonStyle()` | 있음 |
| Default/Secondary Button | "Button" | `Button` + `DefaultButtonStyle()` | 있음 |
| Simple Button | "SimpleButton" | `Button` + `SimpleButtonStyle()` | 있음 |
| Toggle Button | `SCheckBox` "ToggleButtonCheckbox" | `CheckBox` + 토글 스타일 | UW4b |
| SegmentedControl / Alt | `SSegmentedControl` | 없음 | UW4c |
| Check Box | `SCheckBox` "Checkbox" | `CheckBox` + `DefaultCheckBoxStyle()` | 있음 |
| Radio Button | "RadioButton" | `CheckBox` + 라디오 스타일 | UW4b |
| SimpleComboBox / ComboBox | `SComboBox` | `ComboBox` | UW4b 스타일 |
| SHeader | `SHeader` | 없음 | UW4c |
| SHyperlink | `SHyperlink` | 없음 | UW4c |
| SBreadcrumbTrail | `SBreadcrumbTrail` | 없음 | UW4c |
| SCircularThrobber | `SCircularThrobber` | `ActivityIndicator` | UW4b 스타일 |
| SColorBlock | `SColorBlock` | 없음 | UW4c |
| SProgressBar / marquee | `SProgressBar` | `ProgressBar` | UW4b 스타일 |
| SSeparator | `SSeparator` | 없음 | UW4c |
| SSlider / no indentation | `SSlider` | `Slider` | UW4b 스타일 |
| SSpacer | `SSpacer` | `SizeBox` | 있음 |
| SSpinBox | `SSpinBox` | `SpinBox` | UW4b 스타일 |
| SNumericEntryBox / No Spin | `SNumericEntryBox` | 없음 | UW4c |
| SEditableText / Box, MultiLine / Box | `SEditableText(Box)` | Widget 층 `EditableTextElement` | UW4b(Element 층 공개 + 스타일) |
| SInlineEditableText | `SInlineEditableTextBlock` | 없음 | UW4c |
| SSearchBox | `SSearchBox` | 없음 | UW4c |
| Menu Bar / Toolbar / Toolbar 2 | `FMenuBarBuilder`·`FToolBarBuilder` | 없음 — Editor Parts 탭 16.7절 | UW4d1 |
| (Rocks) Combo Button / Simple Combo Button | `SComboButton`·`SSimpleComboButton` | `MenuAnchor`(UE 콤보 버튼 스타일은 없음) | UW4b 스타일 |
| (Rocks) Primary·Positive Action Button | `SPrimaryButton`·`SPositiveActionButton`("+ Add") | 없음 | UW4c |
| (Rocks) Icon + Label Button | UE 갤러리 `LeftRightLabel` 아이콘 변형 | 없음(SVG 의존) | SVG 단계 |
| (Rocks) Editable / Text Combo, Suggestion Text Box | `SEditableComboBox`·`STextComboBox`·`SSuggestionTextBox` | 없음 | UW4c |
| (Rocks) Error Text / Warning Box | `SErrorText`·`SWarningOrErrorBox` | 없음 | UW4c |
| (Rocks) Focus Rectangle | "FocusRectangle"·InputFocus 4/1 | 없음(초점 표시 없음) | UW4b |
| (Rocks) Input Key Selector | `SInputKeySelector` | `InputKeySelector` | UW4b 스타일 |
| (Rocks) Radial Slider | `SRadialSlider`(Runtime/AdvancedWidgets) | `RadialSlider` | UW4b 스타일 |
| (Rocks) Timeline | `STimeSlider`(Editor/Sequencer — 직접 대응은 아님) | `Timeline` | UW4b 스타일 |
| (Rocks) Scroll Bar | `SScrollBar` | `ScrollBar` | UW4b 스타일 |
| (Rocks) Image | `SImage` | `Image` | 있음 |
| (Rocks) Tool Tip | `SToolTip` | `Element::SetToolTipFactory` | UW4b 스타일 |

UW4a는 "있음"과 "UW4b 스타일" 행을 모두 넣는다(아직 UE 룩이 아닌 행도 보여서 남은 일을 드러낸다). UW4b–UW4d 행은 부품이 생기는 단계에 더한다. 체크 상자 행은 UE처럼 레이블 없는 상자 넷(해제·해제·체크·미정, 여백 8 — 1037–1041행)이고, 레이블 붙은 변형은 Rocks 행을 따로 둔다. UE 1439행 "Window Flash Button"은 창 깜빡임(작업 표시줄 주의) 견본이라 창 계약(UW7)에서 다룬다.

### 16.5 Lists

UE "List Widgets"(1499–1620행)처럼 목록·타일·트리를 나란히 둔다.

- 행 상태: 보통·올림·선택·비활성 창의 선택·부모 선택(Colors 탭 SELECT·SELECT HOVER·SELECT INACTIVE·SELECT PARENT slot), 선택 방식(없음·하나·하나 토글·여럿), 키보드 이동.
- 트리: 펼침 화살표와 들여쓰기 선(UE `SExpanderArrow` — UW4c), 모두 펼치기·접기.
- 행 끌어 놓기 표시, 행 문맥 메뉴(UW4d1).
- 열 머리(UE `SHeaderRow`·`SMultiColumnTableRow` — 정렬 화살표, 열 크기 조정·숨기기): 없음, UW4c 행으로 예약.

### 16.6 Layout

배치 부품을 실제 배치로 보인다. 각 견본은 테두리를 그린 칸 안에 색 상자들을 놓아 배치 규칙이 눈에 보이게 한다.

- 가로·세로 상자(`HorizontalStack`·`VerticalStack` — Auto·Fill 비율, 정렬), 격자(`Grid` — 칸 합치기·채움 비율), 균등 격자(`UniformGrid`), 줄바꿈(`WrapStack` — 창 크기에 따라 줄이 바뀐다), 겹치기(`Overlay`), 크기 상자(`SizeBox` — 최소·최대·비율), 비율 맞춤(`FitBox`·`ScaleLayer`), 스크롤(`ScrollView` 가로·세로), 전환(`Switcher` — 단추로 바꾼다), 캐러셀(`Carousel`).
- 도킹(`DockManager`): 탭 묶음 셋을 좌우·상하로 나눈 영역. 분할선 끌기, 탭 끌어 다른 묶음·가장자리에 놓기, 창 밖에 놓아 분리 창을 연다(옛 궤도 화면 self-test가 보던 동작을 여기서 잇는다).
- 그 밖의 있는 부품: 앵커 배치(`AnchorCanvas` — UE `SConstraintCanvas`), 한 축 배치(`StackPanel`), 테두리 상자(`Frame` — UE `SBorder`), 캐시 영역(`CachePanel` — UE `SInvalidationPanel`), 리테이너(`RetainerBox`), 배경 흐림(`BackgroundBlur`), viewport(`Viewport` — UE `SViewport`), 제목 표시줄 영역(`WindowTitleBarArea`).
- UW4c에서 분할기(`SSplitter`)·펼침 영역(`SExpandableArea`)을 더한다.

### 16.7 Editor Parts

에디터 화면을 이루는 조합 부품이다. UW4d1–UW4d3에서 Element 부품으로 만들고 이 탭에 둔다. UW4d1은 메뉴·툴바 빌더와 메뉴 스택, UW4d2는 숫자·벡터·회전 입력과 컬러 피커, UW4d3은 디테일 패널이다(선행: d1, d2, 열 머리·분할선(UW4c), 속성 handle(리플렉션), Transaction — 15장).

- **디테일 패널**(UE `SDetailsView`·`IDetailCustomization`): 검색 상자, 범주 머리(펼침), 이름·값 두 열과 그 사이 분할선, 행 종류(bool 체크 상자, 정수·실수 숫자 입력, 벡터·회전(축 색 띠 X 빨강·Y 초록·Z 파랑), 색(색 막대 + 컬러 피커 열기), 열거(콤보), 문자열(편집 상자), 객체 참조(이름 + 고르기 단추), 배열 머리(개수, 더하기·비우기)), 값을 기본값으로 되돌리기 화살표, 여러 객체 값이 다를 때 표시(Undetermined).
- **컬러 피커**(UE `SColorPicker`): 색 바퀴·색 스펙트럼, 명도·알파 막대, RGB·HSV 숫자 입력, sRGB 16진 입력, 이전·새 색 견본, 확인·취소. 스포이트는 화면 캡처가 필요해 빈칸으로 둔다.
- **메뉴·툴바**: 메뉴 막대(펼침 메뉴, 하위 메뉴, 단축키 표시, 체크 항목, 구분선, 머리글), 툴바(아이콘·레이블 단추, 토글, 콤보 단추, 구분선), 오른쪽 클릭 문맥 메뉴.
- **예약 행(부품이 생기면 이 자리)**: Output Log(Mono, 수준별 색, 필터, 자동 완성 콘솔 입력), 알림 toast(진행·성공·실패, 버튼·하이퍼링크 — ProgressReporting 설계), 모달 대화상자(`SCustomDialog`·`SMessageDialog`), Content Browser 조각(썸네일 타일, 경로 트리, breadcrumb, 필터 칩 `SBasicFilterBar`), 열 있는 Outliner 트리, Transform 행(`SAdvancedTransformInputBox` — 잠금·기본값 되돌리기), 객체·클래스 고르기(`SObjectPropertyEntryBox` — 썸네일·드롭다운 picker), 단축키 편집(`SInputChord` — 충돌 표시), 창 틀(제목 표시줄, 상태 표시줄·drawer `SStatusBar`·`SWidgetDrawer`, 탭 sidebar `STabDrawer`, Major 탭 스타일 `Docking.MajorTab` 1902–1923행·탭 문맥 메뉴), viewport 위 도구 모음, 커브 편집기·NodeGraph `GraphView`(NG5–NG7)·시간 막대(각 설계 문서를 가리킨다).

### 16.8 확인

- `--self-test`: 탭 전환(탭 머리를 실제 좌표로 누름), 행 비활성(활성 체크 상자를 누르면 행이 비활성 brush·전경색으로 그려짐), 올림 상태 전경색(단순 버튼 레이블), 도킹 분할선 끌기·탭 이동을 단언한다.
- `--snapshot`: 탭마다 `Saved/UiDemo/Gallery-<탭>.ppm`을 쓴다.
- 사람 확인: UE 5.8 갤러리(SlateViewer 프로그램이 기본으로 연다 — Programs/SlateViewer/Private/SlateViewerApp.cpp 75–78행 `RestoreStarshipSuite`, 또는 Unreal Insights 메뉴 "Starship Test Suite" — InsightsMenuBuilder.cpp 161–171행)와 나란히 보고 다른 점을 이 장이나 15장에 적는다.
