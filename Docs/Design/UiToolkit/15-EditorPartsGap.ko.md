## 15. 에디터 부품 빈칸 목록

UW4(9.1절)의 산출물 바탕이다. UE 5.8 에디터가 실제로 쓰는 부품(Slate·ToolWidgets·EditorWidgets·PropertyEditor·AppFramework)과 Rocks Element 층(UiFramework·UiCore 공개 헤더를 직접 읽어 판정)을 2026-11-25에 대조했다. 목적은 에디터를 지을 때 빠질 부품과 틀을 미리 찾는 것이다(사용자 목적). 갤러리를 만들며 항목을 채우면 이 표를 고친다. 둥근 상자 brush는 UW3a에서 구현했다.

- 기준: UE 5.8.4 클론 `C:\github\UnrealEngine`(경로는 `Engine/Source/` 기준), Rocks worktree `C:\work\rocks\.claude\worktrees\ui-framework`(feature/ui-framework, 4d6d45ac)
- Rocks 상태는 `UiCore`·`UiFramework` 공개 헤더(필요 시 Private)를 읽어 판정했다. 이름이 비슷하다고 "있음"으로 치지 않았다
- 표기: **있음**(Rocks 이름) / **부분**(빠진 것) / **없음**. `?`는 확인하지 못한 항목
- Rocks 경로 약어: `UF/` = `Engine/Source/Runtime/UiFramework/Public/UiFramework/`, `UC/` = `Engine/Source/Runtime/UiCore/Public/UiCore/`, `UW/` = `Engine/Source/Runtime/UiWidget/`
- UE 경로 약어: `S/` = `Runtime/Slate/Public/`, `SC/` = `Runtime/SlateCore/Public/`, `TW/` = `Developer/ToolWidgets/Public/`, `EW/` = `Editor/EditorWidgets/Public/`, `PE/` = `Editor/PropertyEditor/`

## 1. 부품 목록

### 1.1 기본 입력

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `SButton` (`S/Widgets/Input/SButton.h`) | 모든 버튼, 툴바 버튼 | **있음** `Button` (`UF/Controls/Button.h`). 클릭 방식, 상태 brush. Primary·Secondary·Simple 같은 스타일 변형은 없다(스타일 체계가 없어서, 2절) |
| `SPrimaryButton`·`SPositiveActionButton`·`SNegativeActionButton`·`SSimpleButton`·`SSimpleComboButton`·`SActionButton` (`TW/`) | "+ Add"·"Save" 같은 강조 버튼, 아이콘만 있는 버튼 | **없음**. `Button` 위에 스타일과 아이콘+레이블을 조합해 만들어야 한다 |
| `SCheckBox` (`S/Widgets/Input/SCheckBox.h`) | 체크 상자, 토글 버튼, 라디오 버튼(`ESlateCheckBoxType::ToggleButton` + 라디오 스타일) | **부분** `CheckBox` (`UF/Controls/CheckBox.h`). 3상태 체크만 있다. 토글 버튼 모양과 라디오 묶음(하나만 선택)은 없다 |
| `SSegmentedControl<T>` (`S/Widgets/Input/SSegmentedControl.h`) | 보기 방식 전환, viewport 도구 모음, 정렬 방식 | **없음** |
| `SComboButton` (`S/Widgets/Input/SComboButton.h`) | 아래 화살표가 붙은 버튼 + 드롭다운 (도구 모음, 필터, "Add") | **부분** `MenuAnchor` (`UF/Popups/MenuAnchor.h`, Button 파생, 메뉴 생성기, 배치 방식 13가지). 화살표 장식과 ComboButton 스타일이 없다 |
| `SComboBox<T>`·`STextComboBox` (`S/Widgets/Input/`) | enum·옵션 선택 | **부분** `ComboBox` (`UF/Controls/ComboBox.h`). 인덱스 기반이고 행 생성기와 최대 높이가 있다. 템플릿 항목 형식과 스타일은 없다 |
| `SSearchableComboBox` (`S/Widgets/Input/`, `TW/`) | 항목이 많은 enum·클래스 선택 | **없음**. 검색 상자가 없다 |
| `SEditableComboBox` | 직접 입력도 받는 콤보 | **없음**. 우선순위 낮음 |
| `SSpinBox<T>` (`S/Widgets/Input/SSpinBox.h`) | 숫자 드래그 입력 | **부분** `SpinBox` (`UF/Controls/SpinBox.h`). **float만** 받는다. 드래그, 방향키, 직접 입력, 범위·단계·소수 자릿수, drag 시작·끝 통지는 있다. int·int64·double 형식, `NumericTypeInterface`(단위·서식·수식 입력)가 없다 |
| `SNumericEntryBox<T>` (`S/Widgets/Input/SNumericEntryBox.h`) | 디테일 패널의 숫자 행. 축 레이블 색 띠, 여러 객체 값이 다를 때(Undetermined) 표시, 스핀 없는 모드 | **없음** |
| `SNumericVectorInputBox`·`SVectorInputBox`·`SRotatorInputBox` (`S/Widgets/Input/SVectorInputBox.h`, `SRotatorInputBox.h`) | Transform·FVector·FRotator 편집, 비율 잠금 | **없음** |
| `SSlider` (`S/Widgets/Input/SSlider.h`) | 볼륨, 색 채널 | **있음** `Slider` (`UF/Controls/Slider.h`), 그 밖에 `RadialSlider`·`Timeline` |
| `SNumericDropDown` | viewport 카메라 속도 | **없음**. 우선순위 낮음 |
| `SHyperlink`·`SRichTextHyperlink` (`S/Widgets/Input/`) | 문서 링크, 로그·알림 안의 링크 | **없음** |
| `SSearchBox` (`S/Widgets/Input/SSearchBox.h`) | 모든 패널의 검색 (Outliner·Content Browser·Details·Palette) | **없음**. 편집 텍스트 Element부터 공개해야 한다(1.2절) |
| `SFilterSearchBox`·`SBasicFilterBar` (`TW/Filters/`), `SFilterBar` (`EW/Filters/SFilterBar.h`) | 필터 칩, 저장된 검색 | **없음** |
| `SInputKeySelector` (`S/Widgets/Input/`), `SInputChord` (`EW/SInputChord.h`) | 키 선택, 단축키 편집 | **있음** `InputKeySelector` (`UF/Controls/InputKeySelector.h`). 결과가 `KeyChord`라 단축키 조합 편집에도 쓸 수 있다. 단축키 편집 화면(목록·충돌 표시)은 없다 |
| `SVirtualJoystick`·`SVirtualKeyboardEntry`·`SVolumeControl`·`SExpandableButton` | 게임·모바일용 | 조사에서 뺐다 |

### 1.2 텍스트

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `STextBlock` (`S/Widgets/Text/STextBlock.h`) | 모든 레이블 | **부분** `TextLabel` (`UF/Text/TextLabel.h`). 줄바꿈, 정렬, 말줄임, 밑줄·취소선, hit test, caret 좌표는 있다. 받는 것은 **이미 shaping한 결과**이고, `FontInfo`와 크기만 받아 최종 배율로 스스로 shaping하는 기능은 아직 없다(스펙 3.12절, UW4에서 할 예정). 검색어 강조(`HighlightText`)도 없다 |
| `SEditableText`·`SEditableTextBox` (`S/Widgets/Input/SEditableTextBox.h`) | 이름 입력, 검색, 디테일 문자열 값 | **부분**. 구현(`EditableTextElement`: IME 조합, 클립보드, undo·redo 64단계, 비밀번호, 힌트, 오류 표시)은 있다. 다만 `UW/Private/EditableTextElement.h`에 있고 생성자가 `ObjectRegistry`·`ObjectHandle`을 받아 Widget 층에 묶여 있다. **에디터(Element 층)에서는 쓸 수 없다.** UiFramework로 옮기는 일은 UW9 범위다 |
| `SMultiLineEditableText(Box)` | 설명, 여러 줄 문자열 | **부분**. 같은 Element의 multiline 모드이고, 같은 제약이 있다 |
| `SInlineEditableTextBlock` (`S/Widgets/Text/SInlineEditableTextBlock.h`) | 이름 바꾸기(F2): Outliner, Content Browser, 변수 목록 | **없음** |
| `SRichTextBlock` + `RichTextLayoutMarshaller` (`S/Widgets/Text/`, `S/Framework/Text/`) | 도움말, 경고 상자, 알림 본문 | **부분** `RichTextBlockWidget` (`UW/Public/UiWidget/RichTextBlockWidget.h`)이 Widget 층에만 있다. Element 층의 `RichTextLabel`(스펙 3.12절)은 없다 |
| `SyntaxHighlighterTextLayoutMarshaller`·`SyntaxTokenizer` (`S/Framework/Text/`) | 코드·셰이더 보기, 콘솔 | **없음**. 코드 편집 Element는 UT6 예정이다 |
| `SSuggestionTextBox` (`S/Widgets/Input/`), Output Log 콘솔 입력 | 콘솔 명령 자동 완성 | **없음** |
| `SErrorText`·`SErrorHint`·`SPopUpErrorText` (`S/Widgets/Notifications/`) | 이름 검증 오류 표시 | **없음**. 편집 텍스트의 오류 caption만 있다 |
| `SWarningOrErrorBox`·`SRichTextWarningOrErrorBox` (`TW/`) | 디테일·설정 창의 경고 띠 | **없음** |

### 1.3 목록·트리·표

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `SListView<T>` (`S/Widgets/Views/SListView.h`, `STableViewBase.h`) | 로그, 검색 결과, 목록 패널 | **부분** `ListView` (`UF/Lists/ListView.h`) + `TableRowSource` (`UF/Lists/TableRowSource.h`). 가상화, 행 재사용, 선택(None·Single·SingleToggle·Multi, 범위 선택), 키보드 이동, scroll into view는 있다. **행 높이가 고정**이다(`SetRowHeight`). 행 더블클릭, 오른쪽 클릭 메뉴(`OnContextMenuOpening`), 행 drag·drop 이벤트는 없다 |
| `STreeView<T>` (`S/Widgets/Views/STreeView.h`) | Outliner, Details 트리, 클래스·경로 트리 | **부분** `TreeView` (`UF/Lists/TreeView.h`). key별 펼침 상태, 모두 펼치기·접기, `TableItem.depth`가 있다. 펼침 화살표 부품, 트리 연결선(wires), 펼침 상태 변경 통지는 없다. 행 높이 고정 제약도 그대로다 |
| `STileView<T>` (`S/Widgets/Views/STileView.h`) | Content Browser 타일 | **있음** `TileView` (`UF/Lists/TileView.h`). 타일 너비 + 행 높이다 |
| `SHeaderRow`·`SMultiColumnTableRow`·`STableRow` (`S/Widgets/Views/SHeaderRow.h`, `STableRow.h`) | Outliner 열(이름·형식·보이기), Content Browser 목록 보기, 열 정렬·크기 조정·숨기기 | **없음**. 열 개념이 없다 |
| `SExpanderArrow` (`S/Widgets/Views/SExpanderArrow.h`) | 트리 행의 펼침 화살표 + 들여쓰기 | **없음** |
| `SBreadcrumbTrail` (`S/Widgets/Navigation/SBreadcrumbTrail.h`) | Content Browser 경로, 디테일 하위 객체 경로 | **없음** |
| `SCheckBoxList` (`TW/`) | 체크 목록 대화상자 | **없음**. 우선순위 낮음 |
| `IPropertyTable` (`PE/Public/IPropertyTable.h`) | 속성 행렬(Property Matrix) | **없음**. 참고로만 적는다 |

### 1.4 배치·분할·스크롤

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `SBox`·`SBorder` | 크기 고정, 배경 | **있음** `SizeBox`·`Frame` |
| `SHorizontalBox`·`SVerticalBox`·`SOverlay` | 기본 배치 | **있음** `HorizontalStack`·`VerticalStack`·`Overlay` |
| `SGridPanel`·`SUniformGridPanel` | 설정 표, 버튼 줄 | **있음** `Grid`·`UniformGrid` |
| `SWrapBox` | 도구 모음 줄바꿈, 필터 칩 | **있음** `WrapStack` |
| `SUniformWrapPanel` (`S/Widgets/Layout/`) | 아이콘 격자(갤러리도 7번 쓴다) | **없음** |
| `SScrollBox`·`SScrollBar` | 모든 스크롤 패널 | **있음** `ScrollView`·`ScrollBar` |
| `SScrollBorder` | 스크롤 시 위·아래 그림자 | **없음**. 우선순위 낮음 |
| `SSplitter`·`SSplitter2x2` (`S/Widgets/Layout/SSplitter.h`) | 패널 분할, 디테일 이름·값 열, viewport 4분할 | **없음**. 스펙 3.5절 표에는 `Splitter`가 있지만 헤더가 없다. `DockManager` 내부 분할 손잡이는 외부에서 쓸 수 없다 |
| `SExpandableArea` (`S/Widgets/Layout/SExpandableArea.h`) | 디테일 범주, 설정 묶음, 패널 접기 | **부분** `ExpandableAreaWidget` (`UW/`)이 Widget 층에만 있다. 내부는 `Button` 등을 조합한 것이다. Element는 없다 |
| `SSeparator`·`SSpacer`·`SHeader` (`S/Widgets/Layout/`) | 메뉴·도구 모음 구분선, 범주 제목 | **없음**. `SizeBox`·`Frame`으로 대신할 수는 있지만 스타일된 부품이 없다 |
| `SScaleBox`·`SDPIScaler`·`SConstraintCanvas`·`SWidgetSwitcher` | 배율, 앵커 배치, 전환 | **있음** `FitBox`·`ScaleLayer`·`AnchorCanvas`·`Switcher` |
| `SEnableBox` | 하위 비활성화 | **있음** `Element::SetEnabled` |
| `SInvalidationPanel` | 캐시 | **있음** `CachePanel` |
| `SResizeBox` (`TW/`) | 손잡이로 크기 조정 | **없음** |

### 1.5 독·탭·창

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `FTabManager`·`FGlobalTabmanager`·`SDockTab` (`S/Framework/Docking/TabManager.h`, `S/Widgets/Docking/SDockTab.h`) | 에디터 전체 배치 | **부분** `DockManager` (`UF/Docking/DockManager.h`). 탭 등록과 지연 생성기(표시 이름·그룹·메뉴 표시), 역할 4가지(Major·Panel·Global·Document), 분할 도킹, 창 사이 탭 이동, 떠 있는 탭 미리보기, 가운데 클릭으로 닫기가 있다. 없는 것: 탭 아이콘, 탭 오른쪽 클릭 메뉴(Private에서 오른쪽 버튼 처리를 찾지 못함), 탭 well 숨기기, 사이드바 drawer(`STabDrawer`), 자산 편집기별 중첩 tab manager의 공식 경로(`DockManager`가 Element라서 탭 안에 넣을 수는 있을 것 같다 — `?`), `FLayoutExtender`, 그룹에서 Window 메뉴를 자동으로 만드는 기능 |
| `FLayoutSaveRestore` (`S/Framework/Docking/LayoutService.h`) | 배치 ini 저장, 버전을 올리면 무효화 | **부분** `DockManager::SaveLayout`·`RestoreLayout`이 JSON과 버전을 다룬다. 파일 위치와 사용자 설정 경로는 호스트 몫이고 그 서비스는 없다 |
| `SWindow` + `FSlateApplication::AddWindow`·`AddModalWindow` (`SC/Widgets/SWindow.h`) | 떠 있는 창, 색 선택 창, 모달 대화상자 | **부분**. OS 창은 `PlatformWindow`와 호스트가 관리한다. UI 쪽 창 객체와 modal stack은 없다(스펙 3.10절에 계약만 있음) |
| `SWindowTitleBar`·`SWindowTitleBarArea` | 에디터가 직접 그리는 제목 표시줄(메뉴 바 + 창 버튼) | **부분** `WindowTitleBarArea` (`UF/Window/WindowTitleBarArea.h`)는 영역 판정만 한다. 제목 표시줄 부품은 없다 |
| `SCustomDialog`·`SMessageDialog` (`TW/Dialog/`) | 확인·저장 대화상자 | **없음** |
| `SSidebar`·`SSidebarContainer`·`SWidgetDrawer` (`TW/Sidebar/`, `TW/SWidgetDrawer.h`) | Content Drawer(Ctrl+Space), 상태 표시줄 drawer | **없음** |
| `SStatusBar` (Editor/StatusBar) | 아래 상태 표시줄 | **없음** |
| `SViewport`·`SEditorViewport` (`Editor/UnrealEd/Public/SEditorViewport.h`) | 레벨·자산 viewport | **부분** `Viewport` (`UF/Effects/Viewport.h`, `WidgetSurface`). viewport 도구 모음과 `SEditorViewport`에 해당하는 것은 EditorFramework 범위이고 없다 |

### 1.6 메뉴·도구 모음·명령

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `FMenuBuilder`·`FMenuBarBuilder`·`FToolBarBuilder`·`FSlimHorizontalToolBarBuilder` (`S/Framework/MultiBox/MultiBoxBuilder.h`) | 메뉴 바, 오른쪽 클릭 메뉴, 도구 모음 | **없음** |
| `UToolMenus`·`FToolMenuEntry` (`Developer/ToolMenus/Public/ToolMenus.h`) | 이름 붙은 메뉴 등록과 확장 | **없음**. 스펙에서는 EditorFramework `MenuRegistry`다 |
| `FExtender` (`S/Framework/MultiBox/MultiBoxExtender.h`) | 메뉴·도구 모음 확장 지점 | **없음** |
| `SMenuAnchor` + `FMenuStack` (`S/Framework/Application/MenuStack.h`), `SSubMenuHandler` | 드롭다운, 하위 메뉴, 커서 위치 context menu | **부분** `MenuAnchor`. 배치, 창 안에 맞추기, Esc·바깥 클릭으로 닫기, focus 복귀가 있다. 없는 것: 여러 단의 하위 메뉴 stack(메뉴 안에 `MenuAnchor`를 다시 넣었을 때 동작하는지 `?`), 커서 위치에 여는 context menu(`FSlateApplication::PushMenu`), 새 OS 창으로 여는 방식(UD4) |
| `FUICommandInfo`·`TCommands`·`FUICommandList`·`FInputBindingManager`·`FUIAction` (`S/Framework/Commands/`) | 단축키, 메뉴·도구 모음 항목의 동작 | **부분** `CommandSet`·`CommandInfo`·`CommandList`·`BindingManager`·`CommandAction` (`UF/Application/Commands.h`), `Application::SetCommands`(scope별 목록과 전역 목록). id, 레이블, 설명, 1차·2차 chord, 실행·실행 가능·체크·표시 delegate, 재지정 저장·복원이 있다. 2절에서 빠진 것을 적었다 |
| `FGenericCommands` (`S/Framework/Commands/GenericCommands.h`) | Cut·Copy·Paste·Duplicate·Delete·Rename·SelectAll·Undo·Redo | **없음** |

### 1.7 알림·tooltip

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `SToolTip` (`S/Widgets/SToolTip.h`) | 모든 tooltip | **부분**. `Element::SetToolTipFactory`, `Application`의 tooltip 상태(지연 0.15초, 등장 0.1초, 대화형 tooltip, 주입 시계)가 있다. 기본 tooltip 모양(테두리 + 텍스트 스타일) 부품은 없다 |
| `IDocumentation`·`SDocumentationToolTip` (`Editor/Documentation/Public/`) | Ctrl+Alt로 펼치는 긴 tooltip, 문서 링크 | **없음** |
| `SNotificationList`·`FSlateNotificationManager` (`S/Widgets/Notifications/SNotificationList.h`, `S/Framework/Notifications/NotificationManager.h`) | 오른쪽 아래 toast(진행·성공·실패, 버튼, 하이퍼링크) | **없음**. 테마 색 slot `Notifications`만 있다 |
| `SProgressBar` | 진행 표시 | **있음** `ProgressBar` (marquee 포함) |
| `SThrobber`·`SCircularThrobber` | 로딩 표시 | **있음** `ActivityIndicator` |
| `SSpinningImage` | 회전 아이콘 | **없음**. render transform으로 만들 수 있다 |

### 1.8 색

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `SColorBlock` (`S/Widgets/Colors/SColorBlock.h`) | 색 속성 견본(알파 체크무늬, sRGB 미리보기) | **없음** |
| `SColorWheel`·`SColorSpectrum`·`SColorGradingWheel` (`S/Widgets/Colors/`) | 색 선택 면 | **없음** |
| `SSimpleGradient`·`SComplexGradient` (`S/Widgets/Colors/`) | 채널 slider의 그라디언트 배경 | **없음**. 그리기 목록에 그라디언트가 없다(2절) |
| `SColorPicker`·`SColorThemes` (`Runtime/AppFramework/Public/Widgets/Colors/`) | 색 선택 창: 원·스펙트럼 전환, RGB·HSV slider, hex(sRGB/linear), 스포이드, 이전·새 색, 테마 견본 | **없음** |
| `SColorGradientEditor` (`Editor/UnrealEd/Public/`) | 색 커브 편집 | **없음**. 참고로만 적는다 |

### 1.9 디테일 패널(속성 편집)

모든 항목이 **없음**이다. 기반으로 쓸 수 있는 것은 Core `PropertyPath`(`Core/Source/Runtime/ObjectRuntime/Public/Object/PropertyPath.h`: FieldId 체인 + index·Map key로 읽기·쓰기, 배열 넣기·빼기)와 리플렉션 `displayName`·`category`(`Meta/TypeSchema.h`)뿐이다.

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `IDetailsView`·`SDetailsView` (`PE/Public/IDetailsView.h`, `PE/Private/SDetailsView.cpp`) | Details 패널 본체: 검색, 여러 객체 동시 편집, 고급 펼침 | **없음** |
| `SDetailCategoryTableRow`·`SDetailSingleItemRow`·`SDetailExpanderArrow`·`SDetailRowIndent` (`PE/Private/`) | 범주 머리 행, 속성 행, 들여쓰기 | **없음** |
| `SDetailsSplitter`·`FDetailColumnSizeData` (`PE/Public/SDetailsSplitter.h`, `DetailColumnSizeData.h`) | 이름·값 열 경계를 모든 행이 함께 씀 | **없음**. Splitter도 없다 |
| `SPropertyEditorNumeric` (`PE/Private/UserInterface/PropertyEditor/`) → `SNumericEntryBox` | 숫자 속성(ClampMin·UIMin·Delta·Units) | **없음** |
| `SPropertyEditorBool` | bool 속성 | **없음**. `CheckBox`는 있다 |
| `SPropertyEditorCombo`·`SPropertyComboBox`, `SEnumCombo` (`EW/SEnumCombo.h`) | enum 속성 | **없음**. `ComboBox`는 있다 |
| `SPropertyEditorText`, `STextPropertyEditableTextBox` (`EW/`) | 문자열·FText 속성(지역화 키 포함) | **없음** |
| `SPropertyEditorAsset`·`SObjectPropertyEntryBox`·`SPropertyMenuAssetPicker` (`PE/Public/PropertyCustomizationHelpers.h:274`, `PE/Private/...`) | 객체·자산 참조: 썸네일, 콤보, Browse·Use Selected 버튼 | **없음** |
| `SPropertyEditorClass`·`SClassPropertyEntryBox` (`PropertyCustomizationHelpers.h:380`) | 클래스 선택 | **없음** |
| `SPropertyEditorColor` | 색 속성 → 색 선택 창 | **없음** |
| `SPropertyEditorArray`·`SPropertyEditorArrayItem`·`SPropertyEditorSet`·`SPropertyEditorMap` | "N Array elements", 추가·비우기, 원소별 삽입·삭제·복제 메뉴 | **없음** |
| `SPropertyEditorStruct`, 수학 구조체 사용자 정의(FVector → `SNumericVectorInputBox`) | 구조체 펼침, 벡터 한 줄 표시 | **없음** |
| `SPropertyEditorEditInline` | instanced 하위 객체의 클래스 선택 + 펼침 | **없음** |
| `SPropertyEditorOptional` | Optional 값 설정·해제 | **없음** |
| `SResetToDefaultPropertyEditor`·`SResetToDefaultMenu` (`PE/Public/`) | 기본값으로 되돌리기 화살표 | **없음** |
| `PropertyCustomizationHelpers::Make*Button` (`PropertyCustomizationHelpers.h:156-186`) | Add·Remove·Clear·Browse·UseSelected·InsertDeleteDuplicate 같은 작은 아이콘 버튼 | **없음** |
| `PropertyEditorCopyPaste`·`PropertyEditorClipboard` (`PE/Public/`) | 행 복사·붙여넣기 | **없음** |

### 1.10 드래그·드롭

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `FDragDropOperation`·`FReply::DetectDrag`·`OnDragEnter/Over/Leave/Drop` (`SC/Input/DragAndDrop.h`) | 모든 drag·drop의 기반 | **없음**. 스펙 3.10절(`DragOperation`, `EventReply::DetectDrag`)에 계약만 있다. `Element`에 drag 감지와 drop 대상 가상 함수가 없다. `DockManager` 탭 drag는 내부 전용이다 |
| `FDecoratedDragDropOp` (`Editor/UnrealEd/Public/DragAndDrop/DecoratedDragDropOp.h`) | 커서 옆 장식(아이콘 + "Move/Copy" 문구) | **없음**. OS 창 쪽 `PlatformWindow::DragImage`(Win64)는 있다 |
| `FAssetDragDropOp`·Actor·Class drag op (`.../DragAndDrop/AssetDragDropOp.h` 등) | Content Browser → viewport·디테일, Outliner 부모 바꾸기 | **없음** |
| `SDropTarget`·`SAssetDropTarget` (`EW/`) | 자산 drop 받는 칸(디테일 객체 행) | **없음** |
| `FDelayedDrag` (`S/Framework/DelayedDrag.h`) | 문턱을 넘어야 drag 시작 | **없음** |
| OS 파일 drop(탐색기에서 끌어 임포트) | 임포트 | **없음**. `PlatformWindow`에서 `WM_DROPFILES`·`IDropTarget`를 찾지 못했다 |

### 1.11 기타

| UE 클래스 (파일) | 에디터 용도 | Rocks |
|---|---|---|
| `SImage`·`SLayeredImage` (`SC/Widgets/Images/`) | 아이콘, 겹친 아이콘 배지 | **부분** `Image` (`UF/Controls/Image.h`). 겹친 이미지는 없다 |
| `SCurveEditorPanel`·`FCurveEditor` (`Editor/CurveEditor/Public/`) | 곡선·타임라인 편집 | **없음**. 참고만(UT6). 선·스플라인 그리기가 먼저 있어야 한다 |
| `SGraphEditor`·`SGraphPanel` | 그래프 편집 | **없음**. 참고만. Rocks NodeGraph 설계 11절의 `GraphView`(NG5–NG7)가 다룬다 |
| `SSimpleTimeSlider`·`SScrubControlPanel` (`TW/`, `Editor/KismetWidgets/Public/`) | 애니메이션 미리보기 재생 막대 | **부분** `Timeline` (`UF/Controls/Timeline.h`, 눈금·마커가 있는 Slider). 재생 버튼 묶음은 없다 |
| `SOutputLog` (`Developer/OutputLog/Private/`) | 로그 패널 | **없음**. 목록·서식 텍스트·검색·필터가 먼저 있어야 한다 |
| Widget Reflector (`Developer/SlateReflector`) | UI 트리 검사 | **없음**. 스펙 6절에 "UI 트리 검사기"가 있다 |
| `SWizard` (`Runtime/AppFramework/Public/Widgets/Workflow/`) | 새 프로젝트·임포트 마법사 | **없음**. 우선순위 낮음 |

## 2. 틀(framework) 수준 빈칸

| 영역 | UE 5.8 | Rocks 현재 | 빠진 것 |
|---|---|---|---|
| 스타일 집합 | `ISlateStyle`·`FSlateStyleSet`·`FSlateStyleRegistry`·`FAppStyle`, `FSlateWidgetStyle` 파생(`SlateTypes.h`), `FStarshipEditorStyle`, `FUMGCoreStyle`·`DefaultStyleCache` | `StyleSet`(형식별 표)·`StyleRegistry`(부모 사슬)·앱 스타일, 집합 `CoreStyle`·`EditorStyle`·`GameWidgetStyle`(UW8h — 20.11절) | UMG PNG 자원 키(`Checkbox`·`ProgressBar`·`Scrollbar`·`ToolTip.Background`·`Window`·`Docking.*`·`Menu.*`·`WindowMenuBar.*` — UE 콘텐츠 재배포 불가, 코어 값으로 대신), 슬라이더 손잡이 그림, 글자 그림자·강조색·강조 모양 칸, 에디터 위젯 구분(`EditorUtilityButton`), `Brushes.Title` tint |
| Brush 종류 | `FSlateRoundedBoxBrush`(모서리 반경 4개·외곽선 색·두께, `SC/Brushes/SlateRoundedBoxBrush.h`), Box·Border·Color·Image | `Brush`: None·Image·Box·Border 모드, UV·margin·tint·반전. `PaintBox`에 `cornerRadius`(Vec4)는 있다 | Brush의 rounded box 모드, **외곽선(색·두께)**, 반경 방식. Starship 룩은 거의 전부가 rounded box와 외곽선이다 |
| SVG·아이콘 | `FSlateVectorImageBrush`(`SC/Brushes/SlateImageBrush.h`, nanosvg로 래스터화), 아이콘 style 키 | 없다. 저장소에서 아이콘 자원도 찾지 못했다 | SVG 래스터화와 배율별 cache, 공개 라이선스 아이콘 세트 반입(UD10), 16·20 px 격자 |
| 그리기 primitive | `FSlateDrawElement::MakeLines`·`MakeSpline`·`MakeGradient`·`MakeRotatedBox`·`MakeCustomVerts` | `PaintList`: 사각형(`PaintBox`), 글리프 run, brush, backdrop blur만 있다 | 선·폴리라인, 스플라인(그래프 선, 곡선 편집기), **그라디언트**(색 선택, slider 배경), 점선(선택 사각형). 선택 사각형(`MarqueeRect.h`)도 없다 |
| 텍스트 Element | `STextBlock`이 `FSlateFontInfo`로 스스로 shaping, `SEditableText`, `SRichTextBlock`, marshaller | `TextLabel`이 shaping 결과를 받는다. 편집은 UiWidget Private이다 | `FontInfo` 기반 자동 shaping(UW4), 편집 Element를 UiFramework로 공개(UW9), Element 층 rich text, 구문 강조 marshaller, 검색어 강조 |
| MultiBox(메뉴·도구 모음 빌더) | `FMenuBuilder`·`FToolBarBuilder`·`FMenuBarBuilder`, section, separator, 하위 메뉴, 위젯 항목, 명령 항목(레이블·단축키 문자열·체크 표시 자동), 도구 모음 넘침(chevron), 레이블·아이콘 표시 | 없다 | 빌더 전체. 그 아래 깔리는 `MenuStack`(다단 하위 메뉴, 커서 위치 context menu, 바깥 창 방식)도 필요하다 |
| 메뉴 등록 | `UToolMenus`(이름 계층, 부모 상속, 동적 section, 소유자 단위 해제, 문맥 객체) | 없다 | EditorFramework `MenuRegistry`(스펙 6절) |
| 명령·단축키 | `TCommands<T>`(binding context, 부모 context), `FUICommandInfo`(아이콘, `EUserInterfaceActionType` Button·ToggleButton·RadioButton·Check), `FUICommandList::Append`, `FInputChord::GetInputText`, 충돌 검사, `FGenericCommands` | id·레이블·설명·chord 2개, delegate 4개, scope별 목록과 전역 목록, 재지정 저장 | 아이콘, 동작 형식, **context 계층**(같은 chord를 편집기마다 다르게), 목록 합치기(Append), chord 충돌 검사(`Commands.cpp`는 id 중복만 거부), chord 표시 문자열, 공용 명령 세트, 단축키 편집 화면 |
| 탭 배치 | `FTabManager::FLayout`·`FArea`·`FStack`, `FLayoutSaveRestore`(ini·버전), `FWorkspaceItem` 그룹 → Window 메뉴, `STabDrawer` sidebar, `FLayoutExtender`, 자산 편집기별 tab manager | `DockManager` JSON 저장·복원(버전), 생성기·그룹·listed | 코드로 쓰는 기본 배치 선언(`NewLayout()->AddArea()->Split()` 같은 빌더 — 지금은 `Dock()`을 차례로 부른다), 파일 서비스, sidebar, 확장 지점, 탭 메뉴·아이콘, 편집기별 tab manager 수명 |
| 창·modal | `SWindow`, `AddModalWindow`, popup 창, `SCustomDialog` | 호스트 + `PlatformWindow`. 떠 있는 탭은 `SetOnExternalDrop`로 호스트에 넘긴다 | UI 쪽 창 추상, modal stack(다른 창 입력 막기), 대화상자 부품 |
| 드래그·드롭 | `FDragDropOperation` 형식 ID, `DetectDrag`, drag·drop 이벤트, 장식 창, cursor 질의 | 없다(스펙 계약만) | 틀 전체: Element 가상 함수, Application 라우팅, 장식(`DragImage` 활용), 형식 있는 payload(자산·객체·탭·명령), OS 파일 drop |
| 알림 관리자 | `FSlateNotificationManager`, `FNotificationInfo`(진행 상태, 버튼, hyperlink, 사라지는 시간), `SNotificationList` | 없다 | 관리자, toast Element, 진행·취소 연결(ProgressReporting 설계와 맞물림) |
| 속성 handle | `IPropertyHandle`(`PE/Public/PropertyHandle.h`): 형식별 Get·Set, 텍스트 왕복, 여러 객체(값이 다름), `NotifyPreChange`·`NotifyPostChange`, 배열·Map handle, 기본값 비교·되돌리기, 메타데이터 조회 | Core `PropertyPath`(경로 읽기·쓰기, 배열 편집)와 `displayName`·`category`가 있다. 편집 메타데이터 CU1(`ClampMin`·`UIMin`·`EditCondition` 등)은 Core 헤더에서 찾지 못했다 — 미구현으로 본다 | `FieldHandle`(스펙 6절): 여러 객체, 변경 통지, 기본값, 메타데이터. CU1·CU4·CU6이 먼저다 |
| 디테일 사용자 정의 | `IDetailCustomization`·`IPropertyTypeCustomization`·`IDetailLayoutBuilder`·`IDetailCategoryBuilder`·`FDetailWidgetRow`·`IDetailChildrenBuilder`·`IPropertyRowGenerator`(헤드리스) | 없다 | 클래스별·형식별 등록, 행 builder, 이름·값 위젯 칸, 헤드리스 행 생성기 |
| Transaction·Undo | `FScopedTransaction`, `PreEditChange`·`PostEditChangeChainProperty`, `EPropertyChangeType`(Interactive 등), `FEditorUndoClient` | 일반 Object Transaction은 없다. NodeGraph의 `GraphChange`·`GraphPatch`만 있다 | Transaction 버퍼, 변경 종류 플래그, drag 한 번에 Undo 하나(SpinBox drag 시작·끝 통지는 이미 있다) |
| 숫자 형식 인터페이스 | `TNumericTypeInterface`·`TNumericUnitTypeInterface`(단위 표시·변환), `FBasicMathExpressionEvaluator`(입력란 수식) | 없다 | 단위(cm·deg·%)·서식·수식 입력. 디테일 숫자 행에 꼭 필요하다 |
| 검색·필터 | `TTextFilter`·`FTextFilterExpressionEvaluator`, `SFilterBar` | 없다 | 문자열 필터 식, 필터 칩 틀 |
| UI 애니메이션 곡선 | `FCurveSequence`(펼침·알림 fade) | `UiTimer`·frame timer(`UF/Application/UiTimer.h`, `Application::StartFrameTimer`)만 있다 | ease 곡선 sequence(작은 틀) |
| 레이아웃 보조 | 가변 높이 행(`STableViewBase`가 생성된 행을 측정), 다중 열 표 | 고정 행 높이 | **가변 높이 행**이 없으면 디테일 패널(여러 줄 문자열, 썸네일 자산 행, 배열 자식)을 `TreeView`로 만들 수 없다 |

## 3. 우선순위

### 3.1 갤러리에 먼저 보일 것 (UE `SStarshipGallery`: Colors·Text·Icons·Starship Widgets·List Widgets 탭, `Runtime/AppFramework/Private/Framework/Testing/SStarshipGallery.cpp`)

UE 갤러리의 "Starship Widgets" 탭이 보여 주는 것은 다음과 같다: Primary·Secondary·Simple·Toggle Button, SegmentedControl, Check Box, Radio Button, ComboBox, SHeader, SHyperlink, SBreadcrumbTrail, Throbber, SColorBlock, ProgressBar(marquee 포함), SSeparator, SSlider, SSpinBox, SNumericEntryBox, 편집 텍스트 4종, SInlineEditableText, SSearchBox, Menu Bar, Toolbar.

| 순위 | 항목 | 이유 | 막는 것 |
|---|---|---|---|
| 1 | 스타일 체계(StyleSet + 에디터 스타일 값) + rounded box·외곽선 brush | 모든 탭의 룩이 여기에 달려 있다 | 2절 스타일·Brush |
| 2 | 테마 색 탭 + 글꼴 크기·굵기 탭 | 이미 있는 `ThemeColors`와 글꼴 시스템으로 바로 만들 수 있다. `TextLabel` 자동 shaping이 같이 필요하다 | UW4 TextLabel |
| 3 | 편집 텍스트 Element 공개(한 줄·여러 줄) + `SearchBox` + `InlineEditableText` | 에디터 입력의 절반이다. 구현은 있으니 옮기면 된다 | UW9 이동 |
| 4 | 버튼 변형(Primary·Simple·Toggle·Radio 묶음) + `SegmentedControl` + ComboButton 모양 | 갤러리 첫 줄이고 스타일만 있으면 대부분 조합으로 만든다 | 1번 |
| 5 | `Splitter` | 패널 분할, 디테일 열 | 없음(헤더 0) |
| 6 | `SpinBox<T>` 형식 확장 + `NumericEntryBox` + Vector·Rotator 입력 | 디테일·Transform의 핵심 | 숫자 형식 인터페이스 |
| 7 | `ExpandableArea` Element + `ExpanderArrow` + Separator·Spacer·Header | 디테일 범주, 트리 행 | 작음 |
| 8 | 메뉴 바·도구 모음·context menu(MultiBox 빌더 + 다단 `MenuStack`) | 갤러리의 Menu Bar·Toolbar 칸, 에디터 셸 전체 | 2절 MultiBox |
| 9 | 목록 탭: 다중 열 `HeaderRow` + 가변 높이 행 + 트리 연결선 | Outliner, Content Browser, 디테일 | `ListView` 고정 높이 |
| 10 | SVG 아이콘 brush + 아이콘 탭 | UE 갤러리 Icons 탭. 도구 모음·트리 아이콘 | SVG·아이콘 세트 |
| 11 | 색: `ColorBlock` → 그라디언트 primitive → `ColorPicker`(창 또는 popup) | 사용자 요청(UW4 에디터 부품) | 그라디언트, (창이면) modal |
| 12 | 디테일 패널 부품 진열: 범주 행·속성 행(이름·값 열 공유 splitter, 기본값 되돌리기 화살표)·bool·enum·숫자·벡터·색·객체 참조 행 | 사용자 요청. 갤러리에서는 가짜 데이터로 모양만 보여 줄 수 있다 | 5·6·7·9·11 |
| 13 | Hyperlink·Breadcrumb·tooltip 기본 모양·`WarningOrErrorBox` | 갤러리 칸 채우기, 작음 | 1번, rich text(링크) |
| 14 | 알림 toast(`NotificationList`) | 에디터 피드백 | 알림 관리자 |
| 15 | 드래그·드롭 예시(목록 행 → drop 칸) | 에디터 상호작용의 기반 | 2절 drag·drop 틀 |

### 3.2 에디터를 가장 크게 막는 빈칸

1. **스타일 체계와 brush(rounded·외곽선·SVG)** — 이것이 없으면 UE 에디터처럼 보일 수 없다.
2. **Element 층 편집 텍스트** — 이름 입력, 검색, 디테일 문자열, 숫자 직접 입력이 모두 여기에 기댄다. 지금은 Widget 층에 묶여 있다.
3. **디테일 틀**: 속성 handle, 사용자 정의 등록, 편집 메타데이터(CU1), Transaction. UI 부품을 다 만들어도 Inspector는 Core 계약(UT0)과 Transaction 없이는 동작하지 않는다.
4. **MultiBox + MenuStack + 명령 보강(context·아이콘·동작 형식)** — 메뉴 바, 도구 모음, 오른쪽 클릭 메뉴가 모든 편집기에 있다.
5. **드래그·드롭 틀** — Content Browser → viewport·디테일, Outliner 부모 바꾸기, 자산 drop 칸.
6. **표 기반 보강**: 가변 높이 행, 다중 열 머리, 행 오른쪽 클릭·더블클릭·drag.
7. **Splitter와 창·modal**: 패널 분할, 색 선택 창, 저장 확인 대화상자.
8. **그리기 primitive(선·스플라인·그라디언트)** — 색 선택, 곡선 편집기, 그래프 편집(NodeGraph `GraphView`), 선택 사각형.

### 3.3 확인하지 못한 항목

- `MenuAnchor` 안에 `MenuAnchor`를 넣었을 때 하위 메뉴로 동작하는지는 실행해서 확인하지 않았다.
- `DockManager`를 탭 안에 넣어 자산 편집기별 tab manager로 쓸 수 있는지는 코드 경로만 보고 짐작했다.
- `SpinBox`가 Ctrl·Shift 배율 같은 drag 보조 키를 지원하는지는 헤더에 나오지 않아 보지 못했다.
- 아이콘 자원이 다른 브랜치(engine-runtime 등)에 있는지는 이 worktree만 찾아봤다.
