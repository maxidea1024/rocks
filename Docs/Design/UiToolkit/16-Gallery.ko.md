## 16. UI 갤러리 구성

UiDemo가 보여 주는 UI 갤러리의 페이지 구성이다(9.1절 UW4의 상세). 목적은 셋이다.

- **골라 쓰기**: 에디터를 지을 때 쓸 수 있는 부품과 스타일을 한눈에 보고 고르게 한다(사용자 요청 2026-11-25). 그래서 견본마다 Rocks 이름(부품 이름과 클래스·스타일 함수)을 적는다. 갤러리는 UE 에디터에서 쓰는 컨트롤을 Rocks 체계로 지어 보이는 곳이지 UE 클래스와 1:1 대응을 보이는 곳이 아니므로, 화면에 UE 클래스 이름(`SButton` 등)을 적지 않는다(사용자 지적 2026-11-26). UE 대응은 이 장의 표와 14·15장에만 둔다.
- **UE 대조**: UE 5.8 `SStarshipGallery`(AppFramework/Private/Framework/Testing/SStarshipGallery.cpp)와 탭·행 순서를 맞춰, 나란히 놓고 룩과 동작을 비교한다.
- **쓸 만한 부품 갖추기**(사용자 지시 2026-11-26 — "에디터나 인게임에서 필요한 것들은 최대한 다 보여줘야 한다. 구현에서 끝나는 게 아니라 잘 사용할 만한 것들을 구축해야 한다"): 완료 기준은 기능 유무가 아니라 에디터·게임 개발자가 바로 골라 쓸 수 있는 완성도(UE 에디터 수준의 룩, 모든 상태, 동작, 스타일 선택지, 쓰는 법)다. 에디터와 인게임에 필요할 부품은 요청을 기다리지 않고 15장 빈칸 목록에 올려 단계로 만든다.
- **빈칸 찾기**: 에디터 부품 중 없는 것을 드러낸다. 없는 부품은 갤러리에 흉내 내지 않고 15장 빈칸 목록에 두며, 만들면 이 장의 행이 생긴다.

### 16.1 원칙

- 견본은 실제 부품이다. 갤러리 전용 그리기 코드로 모양만 비슷하게 만들지 않는다. 에디터가 같은 클래스·스타일을 그대로 쓸 수 있어야 한다.
- 모든 위젯 행에 활성 체크 상자를 둔다(UE `NextSlot`). 끄면 그 행을 `SetEnabled(false)`로 바꿔 UE 비활성 효과(알파 0.45, 비활성 전용 brush)를 보인다.
- 행 묶음(체크·이름·위젯)은 가로로 둘까지만 둔다. 셋째 묶음은 창 오른쪽 밖이라 세로 스크롤로 닿지 않는다. 행이 30개를 넘으면 묶음마다 행 수를 늘린다(`GalleryGrid::MaxBands`, 배치는 `Finish`). 견본이 넓은 페이지(배치 — 도킹 520)는 묶음 하나다(`GalleryGrid` 묶음 수 인자 — 둘이면 둘째 묶음이 창 밖으로 잘렸다).
- 이름 열은 위에 부품 이름(사람이 읽는 Rocks 이름 — "Horizontal Stack", "Spin Box"), 아래에 쓰는 법(클래스·스타일 함수·설정, 작은 글꼴, 흰색 25%)을 둔다. 예: "Primary Button" / `Button + PrimaryButtonStyle()`, "Fit Box" / `FitBox (Fit, Cover, Fill)`.
- 상태가 있는 부품은 상태를 나란히 보인다(체크 상자 해제·체크·미정, 진행 막대 값·marquee).
- 동작은 실제로 된다. 누르면 눌리고, 고르면 바뀌고, 끌면 움직인다. 값이 있는 부품(슬라이더·숫자 입력·체크 상자·원형 슬라이더·타임라인·키 선택·콤보)은 바뀐 값을 옆 레이블에 보인다(UE 갤러리의 콤보 레이블처럼).
- **구현된 것은 모두 보인다**(사용자 요청 2026-11-25 — "구현해 놓고 몰라서 안 쓰는 것은 낭비"). 기준은 16.9절 전수 목록이고, 목록의 모든 행이 갤러리 행이거나 "못 보이는 이유와 푸는 단계"를 가진다. Widget 층(UMG 대응) 부품은 같은 Element를 감싼 것이면 Rocks 이름 칸에 "Widget: XWidget"을 함께 적고, Widget 층에만 있는 것은 Widget 호스트(16.10절)로 보인다.

### 16.2 탭

탭 묶음 하나에 아래 순서로 둔다. 처음 앞에 오는 탭은 UE처럼 "Widgets"다.

| 탭 | UE 대응 | 내용 | 단계 |
|---|---|---|---|
| Colors | "Colors" | 테마 slot 견본(BASE·FOREGROUND·ACCENT는 UE 그대로, Rocks가 STATUS(Warning·Error·Success·Notifications)·OUTLINES(Panel·InputOutline·DropdownOutline)·USER(User1–16) 묶음을 더해 61 slot 전부), BRUSHES 묶음(둥근 상자 단일·네 반경, 알약, 외곽선 두께, 단색, 9분할 Box·Border — 절차 생성 텍스처), 테마 전환(다크 기본 ↔ 시험 JSON) | UW4a |
| Text | "Text"(확장) | 16.3절 | UW4a |
| Widgets | "Starship Widgets" | 16.4절 | UW4a–UW4d |
| Lists | "List Widgets" | 16.5절 | UW4a·UW4c |
| Layout | (없음 — Rocks 추가) | 16.6절 | UW4a·UW4c |
| Behavior | (없음 — Rocks 추가) | Element 공통 기능과 앱 기능(16.9.2): 렌더 변환·불투명도·자르기·가시성 5종·전경색 상속·커서 11종·초점 이동 규칙·접근성 트리·타이머·속성 바인딩 | UW4a(초점 표시가 필요한 행은 UW4b) |
| Effects | (없음 — Rocks 추가) | 표면·캐시·흐림: CachePanel(캐시 통계 레이블), OffscreenLayer(phase·tint), BackgroundBlur(강도·반경·저품질 대체), PostBufferUpdate(채널), Viewport(절차 생성 장면 텍스처) | UW4a |
| Icons | "Icons" | 등록한 SVG 아이콘 전부를 16·20·32 px로 키 문자열과 함께 보이고 tint(Foreground·Primary·Warning)를 바꿔 본다(17.4절). UE 탭은 엔진 아이콘 폴더 PNG 목록이지만 Rocks는 자체 제작 SVG 세트를 보인다 | UW4s |
| Drag & Drop | (없음 — Rocks 추가) | 드래그·드롭 틀(9.1절 UW4g)의 견본: 목록 행 → drop 칸(형식이 맞으면 받음·안 맞으면 거부 표시), 같은 목록 안 순서 바꾸기(끼워 넣을 자리 표시), 커서 옆 장식(아이콘 + "Move"/"Copy" 문구, Ctrl로 복사 전환), 문턱(5 px) 전 놓기 = 클릭, Escape 취소, 창 밖 놓기 취소, 받은 결과 레이블 | UW4g |
| Editor Parts | (없음 — Rocks 추가) | 16.7절 | UW4d1–UW4d3, 그 뒤 |
| Game UI | (없음 — Rocks 추가) | Widget 층 전용 부품과 UiScreens(16.10절): 화면 스택·대기열·전환·탭 목록, 입력 장치별 보이기, safe zone, 캐러셀 점 막대, 동적 항목 상자(원형 배치), UserWidget 애니메이션, MVVM 바인딩. 에디터 우선이라 맨 뒤에 두되 무엇이 있는지 알 수 있게 한다. 월드 위젯은 3D 장면이 필요해 갤러리에서 빼고 WorldWidgetSelfTest를 가리킨다 | UW4a2 |

Icons 탭은 SVG 경로(17장, UW4s)가 생기며 넣는다. 탭 역할은 UE 갤러리처럼 Panel이다.

### 16.3 Text

UE 에디터 글꼴 지정(14장 2절)을 모두 견본으로 보인다. 한 행은 [스타일 이름(UE 키)][typeface·크기][견본 문장]이다.

- **핵심 글꼴**: Normal 10, NormalItalic 10, NormalBold 10, NormalBoldItalic 10, Small 8, SmallBold 8, Large 14, LargeBold 14.
- **제목**: HeadingMedium 33, HeadingSmall 21, HeadingExtraSmall 15(BoldCondensed).
- **용도별**: ToolTip.Font 9, HintText(Italic 8, 흐린 전경색), MonospacedText 10·Log 9(Mono), Menu.Heading(Bold 7, 자간 250, White25), Menu.Keybinding 8, PrimaryButtonText(Bold 10), NotificationList Bold·Light 12, PropertyWindow Small·SmallBold 8, DetailsView.Category SmallBold 8, DetailsView.Name 11(흰색).
- **typeface 목록**: UE 기본 합성 글꼴의 typeface(Regular·Medium·Light·VeryLight·Bold·Black·Italic·BoldItalic·BlackItalic·BoldCondensed·BoldCondensedItalic·Mono)를 14 크기로 한 줄씩.
- **문자 체계**: 라틴, 한글(나눔고딕 보조 typeface), 아랍 문자(오른쪽에서 왼쪽, Noto Naskh), 섞인 방향(BiDi), 이모지(마지막 fallback).
- **문단 기능**: 줄바꿈 폭·정책(Word·WordOrCluster), 정렬(왼쪽·가운데·오른쪽), 말줄임, 밑줄·취소선·구간별 장식 색(`LabelDecoration`), 그림자, 자간, 문단 여백·줄 높이 배율(`ParagraphMetrics`), 최소 폭(`SetMinDesiredWidth`).
- **Rich text**(Widget 층 `RichTextBlockWidget` — Widget 호스트로 지금 보인다): `<tag>…</>` 구간 tint·밑줄·취소선, escape, `<img id/>` 인라인 이미지(절차 생성 텍스처).
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
| Toggle Button | `SCheckBox` "ToggleButtonCheckbox"·"ToggleButtonCheckboxAlt"·"TransparentCheckBox" | `CheckBox` + `DefaultToggleButtonCheckBoxStyle()`·`...AltStyle()`·`DefaultTransparentCheckBoxStyle()` | 있음(UW4b2b) |
| SegmentedControl / Alt | `SSegmentedControl` | 없음 | UW4c |
| Check Box | `SCheckBox` "Checkbox" | `CheckBox` + `DefaultCheckBoxStyle()` | 있음 |
| Radio Button | "RadioButton" | `CheckBox` + `DefaultRadioButtonStyle()`(묶음은 호출자) | 있음(UW4b2b) |
| SimpleComboBox / ComboBox | `SComboBox`·`STextComboBox` | `ComboBox`, Widget: `ComboBoxStringWidget`·`ComboBoxKeyWidget` | UW4b 스타일 |
| SHeader | `SHeader` | 없음 | UW4c |
| SHyperlink | `SHyperlink` | 없음 | UW4c |
| SBreadcrumbTrail | `SBreadcrumbTrail` | 없음 | UW4c |
| SCircularThrobber / SThrobber | `SCircularThrobber`·`SThrobber` | `ActivityIndicator`(원형 `circular = true`·선형) | UW4b 스타일 |
| SColorBlock | `SColorBlock` | 없음 | UW4c |
| SProgressBar / marquee | `SProgressBar` | `ProgressBar` | UW4b 스타일 |
| SSeparator | `SSeparator` | 없음 | UW4c |
| SSlider / no indentation | `SSlider` | `Slider` | UW4b 스타일 |
| SSpacer | `SSpacer` | `SizeBox` | 있음 |
| SSpinBox | `SSpinBox` | `SpinBox` | UW4b 스타일 |
| SNumericEntryBox / No Spin | `SNumericEntryBox` | 없음 | UW4c |
| SEditableText / Box, MultiLine / Box | `SEditableText(Box)` | UiFramework `EditableText`·`EditableTextBox`(여러 줄 포함 — Widget: `EditableTextWidget`·`EditableTextBoxWidget`·`MultiLineEditableText(Box)Widget`) | Widgets 탭 "Text Input" 행(UW4f2d — 22.9절): 힌트·값 레이블·읽기 전용·비밀번호·오류 팝업·여러 줄·스크롤 막대 |
| SInlineEditableText | `SInlineEditableTextBlock` | 없음 | UW4c |
| SSearchBox | `SSearchBox` | 없음 | UW4c |
| Menu Bar / Toolbar / Toolbar 2 | `FMenuBarBuilder`·`FToolBarBuilder` | 없음 — Editor Parts 탭 16.7절 | UW4d1 |
| (Rocks) Combo Button / Simple Combo Button | `SComboButton`·`SSimpleComboButton` | `MenuAnchor`(UE 콤보 버튼 스타일은 없음) | UW4b 스타일 |
| (Rocks) Primary·Positive Action Button | `SPrimaryButton`·`SPositiveActionButton`("+ Add") | 없음 | UW4c |
| (Rocks) Icon + Label Button | UE 갤러리 `LeftRightLabel` 아이콘 변형 | 없음(SVG 의존) | SVG 단계 |
| (Rocks) Editable / Text Combo, Suggestion Text Box | `SEditableComboBox`·`STextComboBox`·`SSuggestionTextBox` | 없음 | UW4c |
| (Rocks) Error Text / Warning Box | `SErrorText`·`SWarningOrErrorBox` | 없음 | UW4c |
| (Rocks) Focus Rectangle | "FocusRectangle"·InputFocus 4/1 | 없음(초점 표시 없음 — `FocusOutlineRadius`·`ControlOutlineThickness` 상수만 있다) | UW4b |
| (Rocks) No Border Button | "NoBorder" | `Button` + `NoBorderButtonStyle()` | 있음 |
| (Rocks) Button Click Methods | `EButtonClickMethod`·`EButtonPressMethod` | `Button` 클릭·터치·누름 방식(누른 횟수 레이블) | 있음 |
| (Rocks) Slider vertical / step | `SSlider` Orientation·StepSize | `Slider::SetVertical`·`SetStep`·`SetSnapToStep` | 있음 |
| (Rocks) Progress directions / mask fill | `SProgressBar` BarFillType | `ProgressBar` 방향 7종·채움 Scale·Mask | 있음 |
| (Rocks) Input Key Selector | `SInputKeySelector` | `InputKeySelector`(크기는 내용 + "Button" 여백 + `margin`) | 있음(UW4b2c) |
| (Rocks) Radial Slider | `SRadialSlider`(Runtime/AdvancedWidgets) | `RadialSlider` | UW4b 스타일 |
| (Rocks) Timeline | `STimeSlider`(Editor/Sequencer — 직접 대응은 아님) | `Timeline` | UW4b 스타일 |
| (Rocks) Scroll Bar | `SScrollBar` | `ScrollBar` | UW4b 스타일 |
| (Rocks) Image | `SImage` | `Image` | 있음 |
| (Rocks) Tool Tip | `SToolTip`·`SDocumentationToolTip` | `ToolTip` + `DefaultToolTipStyle()`(글자)·`ContentToolTipStyle()`(제목·단축키·설명), 키보드 초점으로도 열림·Ctrl+F1 다시 열기 | UW4t |

UW4a는 "있음"과 "UW4b 스타일" 행을 모두 넣는다(아직 UE 룩이 아닌 행도 보여서 남은 일을 드러낸다). UW4b–UW4d 행은 부품이 생기는 단계에 더한다. 체크 상자 행은 UE처럼 레이블 없는 상자 넷(해제·해제·체크·미정, 여백 8 — 1037–1041행)이고, 레이블 붙은 변형은 Rocks 행을 따로 둔다. UE 1439행 "Window Flash Button"은 창 깜빡임(작업 표시줄 주의) 견본이라 창 계약(UW7)에서 다룬다.

### 16.5 Lists

UE "List Widgets"(1499–1620행)처럼 목록·타일·트리를 나란히 둔다.

선행(UW4a2): 지금 `ListView`·`TreeView`·`TileView`는 생성자에서 `Application&`를 받아 들고 있다(초점 이동, Shift·Ctrl 상태). 갤러리 탭처럼 창보다 먼저 만들어지거나 다른 창으로 옮겨지는 내용에서는 쓸 수 없고, 에디터에서도 같은 문제다(분리 창으로 옮긴 Outliner가 원래 창의 Application을 본다). UE는 어디서든 `FSlateApplication::Get()`으로 찾는다. Rocks는 전역 등록부를 두지 않으므로(4.1절), `Application::SetRoot`가 루트에 자기를 연결하고 요소가 부모를 따라 루트의 Application을 찾게 한다(`Element::FindApplication()`, 트리에 붙지 않았으면 없음). 같은 문제가 있는 API를 모두 이 방식으로 바꾼다: `ListView`·`TreeView`·`TileView` 생성자, `MenuAnchor::OpenFocused(Application&)`, `ListViewBaseWidget::SetSource(app, …)`, UiScreens `ScreenContainer` 생성자, `CarouselWidget::BeginAutoScrolling(app, …)`, `UserWidget::StartAnimation(…, app)`. Lists 탭과 Game UI 탭은 이 수정과 함께 넣는다(UW4a2). 찾는 기반은 UW4f2e가 먼저 둔다 — UiCore Element는 Application 형식을 모르므로 UiFramework 정적 함수 `Application::Find(const Element&)`이고, `UiHostServices` 소유자 표지로 찾는다(22.7a절).

- 행 상태: 보통·올림·선택·비활성 창의 선택·부모 선택(Colors 탭 SELECT·SELECT HOVER·SELECT INACTIVE·SELECT PARENT slot), 선택 방식(없음·하나·하나 토글·여럿), 키보드 이동.
- 트리: 펼침 화살표와 들여쓰기 선(UE `SExpanderArrow` — UW4c), 모두 펼치기·접기.
- 행 끌어 놓기 표시, 행 문맥 메뉴(UW4d1).
- 열 머리(UE `SHeaderRow`·`SMultiColumnTableRow` — 정렬 화살표, 열 크기 조정·숨기기): 없음, UW4c 행으로 예약.
- 그 밖에 보일 것: 행 재사용(`TableRowSource::BindRow`·`ReleaseRow`·`SetRowSelected`), 이동하면 선택(`SetSelectOnNavigation`), ScrollIntoView 정렬 4종(`ScrollIntoViewAlignment`), ScrollToEnd, 휠 배수.

### 16.6 Layout

배치 부품을 실제 배치로 보인다. 각 견본은 테두리를 그린 칸 안에 색 상자들을 놓아 배치 규칙이 눈에 보이게 한다.

- 가로·세로 상자(`HorizontalStack`·`VerticalStack` — Auto·Fill 비율, 정렬), 격자(`Grid` — 칸 합치기·채움 비율), 균등 격자(`UniformGrid`), 줄바꿈(`WrapStack` — 창 크기에 따라 줄이 바뀐다), 겹치기(`Overlay`), 크기 상자(`SizeBox` — 최소·최대·비율), 비율 맞춤(`FitBox`·`ScaleLayer`), 스크롤(`ScrollView` 가로·세로, 막대 표시 정책 넷 — Auto·Always·HideWhenNotInUse·OnHover, 23.6절), 전환(`Switcher` — 단추로 바꾼다), 캐러셀(`Carousel`).
- 도킹(`DockManager`): 탭 묶음 셋을 좌우·상하로 나눈 영역. 분할선 끌기, 탭 끌어 다른 묶음·가장자리에 놓기, 창 밖에 놓아 분리 창을 연다(옛 궤도 화면 self-test가 보던 동작을 여기서 잇는다).
- 그 밖의 있는 부품: 앵커 배치(`AnchorCanvas` — UE `SConstraintCanvas`, 점·늘이기 앵커·autoSize), 한 축 배치(`StackPanel` — FlowDirection 오른쪽에서 왼쪽, FillContent·shrinkWeight·min·max), 테두리 상자(`Frame` — UE `SBorder`), 슬롯 래퍼(`SlotPresenter` — 정렬 4×4), 캐러셀 lazy 페이지(`CarouselPage`), 제목 표시줄 영역(`WindowTitleBarArea`), 도킹 역할(Major·Panel·Global·Document)과 배치 저장·복원 JSON 견본. 표면·효과 부품은 Effects 탭에 둔다.
- 펼침 영역은 Widget 층 `ExpandableAreaWidget`이 이미 있으므로 Widget 호스트로 지금 보이고, UW4c에서 Element로 옮긴다. 분할기(`SSplitter`)는 UW4c에서 더한다.

### 16.7 Editor Parts

에디터 화면을 이루는 조합 부품이다. UW4d1–UW4d3에서 Element 부품으로 만들고 이 탭에 둔다. UW4d1은 메뉴·툴바 빌더와 메뉴 스택, UW4d2는 숫자·벡터·회전 입력과 컬러 피커, UW4d3은 디테일 패널이다(선행: d1, d2, 열 머리·분할선(UW4c), 속성 handle(리플렉션), Transaction — 15장).

- **디테일 패널**(UE `SDetailsView`·`IDetailCustomization`): 검색 상자, 범주 머리(펼침), 이름·값 두 열과 그 사이 분할선, 행 종류(bool 체크 상자, 정수·실수 숫자 입력, 벡터·회전(축 색 띠 X 빨강·Y 초록·Z 파랑), 색(색 막대 + 컬러 피커 열기), 열거(콤보), 문자열(편집 상자), 객체 참조(이름 + 고르기 단추), 배열 머리(개수, 더하기·비우기)), 값을 기본값으로 되돌리기 화살표, 여러 객체 값이 다를 때 표시(Undetermined).
- **컬러 피커**(UE `SColorPicker`): 색 바퀴·색 스펙트럼, 명도·알파 막대, RGB·HSV 숫자 입력, sRGB 16진 입력, 이전·새 색 견본, 확인·취소. 스포이트는 화면 캡처가 필요해 빈칸으로 둔다.
- **메뉴·툴바**: 메뉴 막대(펼침 메뉴, 하위 메뉴, 단축키 표시, 체크 항목, 구분선, 머리글), 툴바(아이콘·레이블 단추, 토글, 콤보 단추, 구분선), 오른쪽 클릭 문맥 메뉴.
- **명령·단축키(지금 가능)**: `CommandSet`·`BindingManager`·`CommandList`로 명령 표(주·보조 chord, 실행 횟수), `InputKeySelector`로 다시 지정, 기본값 되돌리기, 저장·불러오기 — UE `SInputChord`·단축키 편집기의 앞 단계다.
- **예약 행(부품이 생기면 이 자리)**: Output Log(Mono, 수준별 색, 필터, 자동 완성 콘솔 입력), 알림 toast(진행·성공·실패, 버튼·하이퍼링크 — ProgressReporting 설계), 모달 대화상자(`SCustomDialog`·`SMessageDialog`), Content Browser 조각(썸네일 타일, 경로 트리, breadcrumb, 필터 칩 `SBasicFilterBar`), 열 있는 Outliner 트리, Transform 행(`SAdvancedTransformInputBox` — 잠금·기본값 되돌리기), 객체·클래스 고르기(`SObjectPropertyEntryBox` — 썸네일·드롭다운 picker), 단축키 편집(`SInputChord` — 충돌 표시), 창 틀(제목 표시줄, 상태 표시줄·drawer `SStatusBar`·`SWidgetDrawer`, 탭 sidebar `STabDrawer`, Major 탭 스타일 `Docking.MajorTab` 1902–1923행·탭 문맥 메뉴), viewport 위 도구 모음, 커브 편집기·NodeGraph `GraphView`(NG5–NG7)·시간 막대(각 설계 문서를 가리킨다).

### 16.8 확인

- `--self-test`: 탭 전환(탭 머리를 실제 좌표로 누름), 행 비활성(활성 체크 상자를 누르면 행이 비활성 brush·전경색으로 그려짐), 올림 상태 전경색(단순 버튼 레이블), 도킹 분할선 끌기·탭 이동을 단언한다.
- `--snapshot`: 탭마다 `Saved/UiDemo/Gallery-<탭>.ppm`을 쓴다.
- 사람 확인: UE 5.8 갤러리(SlateViewer 프로그램이 기본으로 연다 — Programs/SlateViewer/Private/SlateViewerApp.cpp 75–78행 `RestoreStarshipSuite`, 또는 Unreal Insights 메뉴 "Starship Test Suite" — InsightsMenuBuilder.cpp 161–171행)와 나란히 보고 다른 점을 이 장이나 15장에 적는다.

### 16.9 전수 목록

갤러리가 빠짐없이 덮어야 하는 구현 부품과 기능의 목록이다(리뷰어 전수 조사 2026-11-25 — UiCore·UiFramework·UiWidget·UiScreens·UiWorld·UiRender·TextLayout 공개 헤더 전부). 행을 고르는 기준이고, 부품이 생기거나 없어지면 이 목록을 먼저 고친다.

#### 16.9.1 부품

- **UiCore**: Brush(모드 None·Image·Box·Border·RoundedBox, 네 반경, 알약, 외곽선, mirror, uv), StyleBrush, ThemeColors(61 slot, JSON 세 단계 Load), ColorRef(Specified·Theme·Foreground·SubduedForeground·Style), ButtonStyle·CheckBoxStyle·DockTabStyle·TextStyle·FontStyle(자간)·Typeface 12종, StyleSet·AddCoreStyles(UE 키 8개), 선언형 Make·Build·Bind·Bindable·ElementAttribute, KeyChord, NavigationRule, HitGrid(회전된 요소 hit), CacheRoot·PaintCacheStats, WidgetSurface·WidgetSurfaceScene.
- **UiFramework 컨트롤**: Button(클릭·터치·누름 방식, 상태 이벤트, tint·전경·내용 여백), CheckBox, ComboBox, MenuAnchor(배치 13종·창 안 맞춤·Esc·바깥 누름), ActivityIndicator(원형·선형, 조각 수·반경·주기·축·brush), ProgressBar(방향 7종, Scale·Mask, marquee, BindValue), Slider(세로·step·snap·잠금·컨트롤러 잠금·손잡이 들여쓰기), SpinBox(범위·소수 자릿수·드래그 끄기·확정 사유), InputKeySelector(보조 키·게임패드·Esc 취소), RadialSlider(각·오프셋·손잡이·바늘), Timeline(눈금·마커), ScrollBar, Image, tooltip(대화형·지연·등장 시간).
- **텍스트**: TextLabel(줄바꿈 정책·정렬·말줄임·장식·그림자·자간·ParagraphMetrics·최소 폭·HitText·CaretPosition), 문자 체계(라틴·한글·아랍 RTL·BiDi·이모지·수학), 글꼴 렌더링 선택(방향·shaping 방식·hinting — 데모가 노출할 때).
- **목록**: ListView(선택 방식 넷, 행 높이, 휠, 포인터 스크롤, 이동 선택, ScrollIntoView, ScrollToEnd, 터치 팬), TreeView(펼침·모두), TileView(타일 너비), TableRowSource.
- **배치**: HorizontalStack·VerticalStack·StackPanel, Grid(span·layer·nudge·채움 비율), UniformGrid, WrapStack(줄바꿈 선택 사항), Overlay, SizeBox(최소·최대·고정·종횡비), Frame, SlotPresenter, AnchorCanvas, FitBox(맞춤 7 × 방향 3), ScaleLayer, ScrollView(휠 정책·막대 표시 정책·Reserve·Overlay 배치·scroll padding·터치·오른쪽 드래그), Switcher, Carousel·CarouselPage, DockManager(역할·분할·탭 끌기·분리 창·spawner·배치 JSON), WindowTitleBarArea.
- **표면·효과**: CachePanel, OffscreenLayer, BackgroundBlur, PostBufferUpdate, Viewport.
- **앱·입력·명령**: CommandSet·BindingManager·CommandList·SetCommands, 타이머(StartTimer·StartFrameTimer), 초점(SetFocus·MoveFocus·Navigate·게임패드), tooltip 상태, AccessibleTree, 커서(SetCursorOverride), SetTheme, InputAdapter·TouchPan.
- **Widget 층 전용**: RichTextBlockWidget(+RichTextStyle·Decorator·ImageDecorator), EditableText·EditableTextBox·MultiLineEditableText(Box)Widget, ExpandableAreaWidget, ComboBoxString·KeyWidget, CarouselWidget(자동 넘김)·CarouselNavBarWidget, SafeZoneWidget, DynamicEntryBoxWidget(+RadialBoxSettings — 원형 배치는 private `EntryPatternPanel`), ListViewWidget::UseObjectItems·WidgetPool, UserWidget·UserWidgetAnimation·WidgetAnimation(채널·재생 방식), WidgetTransform, Widget 공통(SetToolTipText·SetAccessibleText·SetNavigationRule·SetCursor), MVVM 바인딩(WidgetFieldBindings·WidgetModelSource·WidgetPropertyBinding·WidgetEventBinding).
- **Widget 층 감싼 것(이름 대응)**: Button·CheckBox·ComboBox·MenuAnchor·Image·ProgressBar·Slider·SpinBox·RadialSlider·ScrollBar·InputKeySelector·Throbber·CircularThrobber·TextLabel·Border(Frame)·SizeBox·Spacer·NamedSlot·NativeWidgetHost·Overlay·CanvasPanel(AnchorCanvas)·GridPanel·UniformGridPanel·HorizontalBox·VerticalBox·StackPanel·StackBox·WrapBox·ScaleBox(FitBox)·WidgetSwitcher·ScrollBox·InvalidationBox(CachePanel)·OffscreenLayer·BackgroundBlur·PostBufferUpdate·Viewport·WindowTitleBarArea·ListView·TileView·TreeView Widget.
- **UiScreens**: Screen(입력 모드), ScreenStack·Queue·Switcher(초점 복구·플레이어별 초점)와 Widget 판, ScreenTabListWidget, HardwareVisibilityWidget, ScreenInputRouter·ScreenDeviceState.
- **UiWorld·UiRender**: 월드 위젯(갤러리 밖 — WorldWidgetSelfTest), 렌더 기반(Effects가 간접으로 보인다).

#### 16.9.2 공통 기능(Behavior 탭)

비활성(행 체크 상자 — Widgets), 렌더 변환(회전·배율·기울기·pivot과 변환 뒤 hit — 각도 슬라이더로 돌린 버튼을 계속 누를 수 있음), 불투명도, 자르기, 가시성 5종(자리 차지·hit 차이), 전경색 상속(올린 버튼·흐린 전경), 커서 11종, 초점 이동 규칙(Wrap·Stop·Explicit — 초점 표시 UW4b 뒤), 접근성 트리 표, 타이머 카운터, 속성 바인딩(Bind로 슬라이더 → 색·진행 값), volatile·캐시(Effects), 테마 전환(Colors), 명령(Editor Parts), 오른쪽에서 왼쪽 흐름(Layout·Text), 배율(Layout), 터치 팬·게임패드(설명과 사람 확인 목록).

### 16.9a 빠진 행(UW4b 리뷰 2026-11-26)

16.4·16.6절 표에 "있음"인데 아직 갤러리에 없는 행이다. UW4b 2차(콤보·숫자 입력·tooltip·토글·라디오)를 넣을 때 함께 넣는다.
- Widgets: 선형 throbber(UE SThrobber — 조각 이미지는 UW4s 뒤), Combo Button(`MenuAnchor`), No Border Button, Button Click Methods(누름·뗌·정확한 클릭), 세로·단계 슬라이더, 진행 방향 7종·Mask 채움, 값 레이블(슬라이더·원형 슬라이더·타임라인·체크 상자 — 16.1절), 타임라인 눈금·마커 설정.
- Effects: PostBufferUpdate 행, CachePanel 캐시 통계 레이블, OffscreenLayer phase.
- Layout: SizeBox(최소·최대·비율), Frame(SBorder), SlotPresenter, StackPanel 오른쪽에서 왼쪽, 도킹 역할(Panel·Document)과 배치 JSON 견본.

### 16.10 막힌 것과 푸는 방법

| 대상 | 막힌 이유 | 방법 | 단계 |
|---|---|---|---|
| 목록 셋, MenuAnchor 열기, 화면 컨테이너, 캐러셀 자동 넘김, UserWidget 애니메이션, 타이머·접근성 트리·테마 견본 | `Application`을 생성자·인자로 받는다 | `Element::FindApplication()`(16.5절) | UW4a2 |
| Widget 층 전용 부품 | Element 공개판이 없고, Widget 층은 ObjectRegistry·WidgetTree·`WidgetTextShaper`가 필요하다 | 갤러리 Widget 호스트: GalleryContext에 ObjectRegistry·WidgetTree를 두고, DemoText로 `WidgetTextShaper`를 구현해 `TakeElement` 결과를 행에 넣는다 | UW4a2 |
| 이미지·9분할 Box·Border·ProgressBar Mask·throbber 조각 brush·rich text 이미지·Viewport 장면 | 등록된 텍스처가 없다 | 시작할 때 절차 생성 텍스처(체크무늬·9분할 테두리)를 BrushTexture 표에 넣어 GalleryContext로 넘긴다 | UW4a |
| 초점 이동·Focus Rectangle | Behavior 탭 Focus Outline 행(UW4f1) | 편집 텍스트 억제 견본은 UW4f2 | UW4f2 |
| CachePanel 효과 | 캐시 통계를 읽을 길이 없다 | 호스트가 `CacheRoot::LastUpdate()`를 레이블로 넘기는 콜백 | UW4a |
| 터치 팬·게임패드 컨트롤러 잠금 | 기기가 필요하다 | 행에 설명, 사람 확인 목록 | — |
