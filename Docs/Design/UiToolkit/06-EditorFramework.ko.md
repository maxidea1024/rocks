## 6. 에디터 프레임워크

| 영역 | Rocks | UE 5.8 대응 | 계약 |
|---|---|---|---|
| 독 배치 | `DockManager` · `DockTab` · 배치 트리(영역·분할·탭 묶음) | `FTabManager` · `FGlobalTabmanager` · `SDockTab` · `FLayout`/`FArea`/`FStack`(`Slate/Public/Framework/Docking/TabManager.h:241-1797`) | UiFramework 소속. 탭 생성기 등록(표시 이름·그룹·메뉴 표시), 배치 복원·저장, 탭 역할(주·패널·전역·문서). 배치는 이름에 버전을 붙여 저장하며 이름을 올리면 저장된 사용자 배치를 무시한다(`LayoutService.h:19-35`) |
| 자산 편집기 | `AssetEditor` · 모드형 자산 편집기 | `FAssetEditorToolkit` · `FWorkflowCentricApplication`(`Editor/UnrealEd/Public/Toolkits/AssetEditorToolkit.h:115-548`) | 탭 생성기 등록·해제, 기본 배치, 명령 결합, 도구 모음 확장. 모드마다 배치와 허용 탭 집합 |
| 명령 | `CommandSet` · `CommandInfo` · `CommandList` · `KeyChord` · `BindingManager` | `TCommands` · `FUICommandInfo` · `FUICommandList` · `FInputChord` · `FInputBindingManager`(`Slate/Public/Framework/Commands`) | UiFramework 소속. 1차·2차 단축키, 실행·실행 가능·체크 상태·표시 여부 delegate, 사용자 재지정 저장. 단축키는 Element bubble에서 각 Element가 명령 목록을 먼저 조회하고(`UICommandList.h:170-188`), 처리되지 않은 키는 전역 명령 목록으로 넘긴다(UE `SetUnhandledKeyDownEventHandler`, `Editor/MainFrame/Private/Frame/MainFrameActions.cpp:97, 275-290`) |
| 메뉴 | `MenuRegistry` · 메뉴 · section · 항목 | `UToolMenus`(5.8 현행, `Developer/ToolMenus/Public/ToolMenus.h:100-531`). `FMenuBuilder`/`FExtender`는 UToolMenus가 내부에서 사용하며 공개 런타임 API로도 남음 | EditorFramework 소속. 점으로 구분한 계층 이름, 부모 메뉴 상속, 확장 지점, 동적 section·항목, 삽입 위치, 소유자 단위 해제, 명령 목록·문맥 객체 전달. 에디터 밖 도구는 UiFramework의 메뉴 구성 Element와 `CommandList`로 메뉴를 만든다 |
| Inspector | `Inspector` · 클래스별 배치 사용자 정의 · 형식별 편집기 · `FieldHandle` | `IDetailsView` · `IDetailCustomization` · `IPropertyTypeCustomization` · `IPropertyHandle`(`Editor/PropertyEditor/Public`) | 다중 객체 편집(다른 값 표시), 범주·고급 표시, 행 생성기(헤드리스), 단일 속성 view, 구조체 view. 편집 메타데이터(CU1)를 소비 |
| 변경·Undo | Transaction · 변경 전후 통지 · 변경 종류 | `FScopedTransaction` · `PreEditChange`·`PostEditChangeChainProperty` · `EPropertyChangeType`(`CoreUObject/Public/UObject/UnrealType.h:6944-6975`) | 변경 종류는 비트 플래그다: 미지정·배열 추가·제거·비우기·값 설정·복제·대화형(drag 중)·이름 변경 후 재지정·편집 가능 전환·배열 이동·기본값 복원. slider drag는 시작 시 Transaction 시작 → drag 중 대화형·비기록 설정 → 끝에 값 설정 후 Transaction 종료로 Undo 1개(`SPropertyEditorNumeric.h:492-596`) |
| viewport | `EditorViewport` · viewport client | `SEditorViewport` · `FEditorViewportClient` · `FSceneViewport` | 에디터 viewport는 별도 render target 합성, Tryout은 viewport 교체 또는 새 창. 마우스 capture 모드(없음·영구·누름 중 등)와 capture 해제 명령(UE 기본 Shift+F1) |
| 에디터 스타일 | 에디터 `StyleSet` · 테마 · 앱 배율 | `FStarshipEditorStyle` · `USlateThemeManager` · `UEditorStyleSettings::ApplicationScale` | 4.2절 기본값 |
| 자산 선택 | asset picker · path picker | `IContentBrowserSingleton::CreateAssetPicker` | 필터·선택 모드 |
| 그래프 편집 | `GraphView` 모듈(Editor 계층, EditorFramework와 별도) · 도메인 시각 등록 · overlay | `SGraphEditor` · `SGraphPanel` · `SGraphNode` · `UEdGraph`/`UEdGraphSchema` | [노드 그래프](../NodeGraph/11-ModulesAndView.ko.md) 11절. 머티리얼(전체 계획 14.5절)·오디오·애니메이션 등 그래프 편집기가 함께 쓰는 화면이다. 데이터 모델·변경·undo는 `NodeGraph`, 도메인은 schema·노드 서술과 시각 등록·overlay만 넣는다. 화면은 노드 시각을 `NodeId`로 찾고 통지 단위로 증분 갱신한다 |
| 곡선·타임라인 | 곡선 편집기 · 컷씬 타임라인 | `FCurveEditor` · `FCurveModel` · `SCurveEditorPanel` · `ISequencer` | 곡선 편집기를 타임라인이 재사용(UE와 같다) |
| 로그·검사 | 로그 패널 · UI 트리 검사기 | `OutputLog` · `SlateReflector` | UI 트리 검사기는 Element 트리·geometry·무효화 사유를 표시하며 개발 빌드 전용 |

**모듈 경계:** UE는 독·명령·배치 저장·메뉴 기반·스타일·viewport 합성을 런타임 Slate/SlateCore에 두고, Object 모델에 닿는 Inspector·Transaction·자산 편집기를 Editor 전용으로 둔다. Rocks도 같은 경계로 UiFramework와 EditorFramework를 나눈다. 메뉴 등록(`UToolMenus`)은 UE에서 Object에 의존하는 Developer 모듈이므로 Rocks는 EditorFramework에 둔다.

**Designer:** UE는 미리보기 인스턴스를 편집한 뒤 템플릿으로 속성 사슬을 옮긴다(`UMGEditor/Private/Details/SWidgetDetailsView.cpp:664-700`). Rocks는 다음 규칙으로 템플릿을 Transaction으로 편집한다.

- drag 중 대화형 값은 미리보기에만 적용하고, 끝에 템플릿에 Transaction 1개를 기록한다(UE도 대화형 변경은 옮기지 않는다).
- 애니메이션 기록 모드에서는 편집을 키로 기록한다.
- 구조 변경과 재생성 표지(UE `DesignerRebuild`)가 있는 속성은 미리보기를 다시 만든다. 그 밖의 속성은 같은 속성 경로(CU4)를 미리보기 인스턴스에 적용한다.
- 사전 구성(Designer 실행)이 덮어쓴 값은 Inspector에서 템플릿 값과 구분하여 표시한다.

**UE와 다른 점:** UE의 편집 조건(`EditCondition`)은 에디터가 실행 중 해석하는 문자열 식이다(`EditConditionParser.cpp`). Rocks는 메타 컴파일러가 빌드 시 검증하는 제한된 식으로 한다(UD8).
