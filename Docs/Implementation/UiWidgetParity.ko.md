# 언리얼 UMG와 Rocks 인게임 위젯 대조

대상은 로컬 Unreal Engine 5.8.4와 Rocks의 `feature/ui-framework` 작업 트리다. `Engine/Source/Runtime/UMG`의 C++ 헤더에서 `UWidget`과 `UPanelSlot`의 파생 관계를 각각 끝까지 따라갔다. UMG 자체에는 위젯 계층 55형식(기반형 포함)과 슬롯 계층 18형식이 있다. 위젯 중 UCLASS에 Abstract가 명시된 것은 7형식, 나머지는 48형식이다. UE 헤더 경로의 기준은 `Engine/Source/Runtime/UMG`다.

각 대응형의 Rocks 헤더, 구현 파일과 활성 `UiWidget.rocksids.json` 타입 엔트리를 함께 확인했다. 상태와 배치 동작을 기반형에 모두 위임하는 슬롯은 `AlignedWidgetSlot` 또는 `StackWidgetSlot`의 구현을 확인했다. **등록**은 이 소스 대조 결과다. 같은 이름이나 기반형의 존재만으로 UE의 모든 속성, 이벤트, 입력 장치와 렌더 경로를 검증한 것으로 간주할 수 없다. **공유**는 다른 Rocks 타입이 관련 기능을 맡는 경우다. **미확인**에는 추가 구현이나 실환경 검증이 필요한 행을 남겼다.

## 게임 위젯

| UE 타입 | 분류 | Rocks 대응 | 확인 내용 및 대조할 동작 | UMG 헤더 |
|---|---|---|---|---|
| `UBackgroundBlur` | 구체 | 등록 `BackgroundBlurWidget` | 강도·커널 반경·알파 반영·둥근 모서리와 저품질 대체 브러시를 적용한다. 장치별 결과는 렌더 테스트 범위를 참조한다. | `Public/Components/BackgroundBlur.h` |
| `UBorder` | 구체 | 등록 `BorderWidget` | 자식 하나의 브러시와 tint, 안쪽 여백을 적용한다. 자식 정렬은 BorderWidgetSlot이 맡는다. UE 머티리얼 자산 변환은 별도다. | `Public/Components/Border.h` |
| `UButton` | 구체 | 등록 `ButtonWidget` | 포인터·터치·키보드 입력 방식을 선택하고 눌림·해제·클릭·hover 이벤트를 연결한다. 상태별 브러시와 tint를 지정할 수 있다. | `Public/Components/Button.h` |
| `UCanvasPanel` | 구체 | 등록 `AnchorCanvasWidget` | 앵커, 위치, 크기, 정렬, z 순서를 전용 슬롯에 저장. | `Public/Components/CanvasPanel.h` |
| `UCheckBox` | 구체 | 등록 `CheckBoxWidget` | 2상·3상 상태, 자식 하나, 상태 변경 콜백을 제공한다. | `Public/Components/CheckBox.h` |
| `UCircularThrobber` | 구체 | 등록 `CircularThrobberWidget` | 조각 수·브러시·반경·회전 주기를 지정하고 애니메이션을 정지할 수 있다. | `Public/Components/CircularThrobber.h` |
| `UComboBox` | 구체 | 등록 `ComboBoxWidget` | 객체 참조 목록·선택·항목 생성기를 제공한다. 메뉴는 전면 Overlay에 붙는다. | `Public/Components/ComboBox.h` |
| `UComboBoxKey` | 구체 | 등록 `ComboBoxKeyWidget` | Name 옵션의 추가·삭제·선택과 공통 행 생성기를 사용한다. | `Public/Components/ComboBoxKey.h` |
| `UComboBoxString` | 구체 | 등록 `ComboBoxStringWidget` | 문자열 옵션 추가·삭제·초기화·선택, 조회를 제공한다. | `Public/Components/ComboBoxString.h` |
| `UContentWidget` | 추상 | 등록 `ContentWidget` | 현재 단일 내용이나 슬롯을 조회하고 실행 중 내용을 교체·제거한다. | `Public/Components/ContentWidget.h` |
| `UDynamicEntryBox` | 구체 | 등록 `DynamicEntryBoxWidget` | 전용 UserWidget 엔트리 생성 및 같은 클래스 재사용. 배치와 간격은 공통 기반형에서 선택한다. | `Public/Components/DynamicEntryBox.h` |
| `UDynamicEntryBoxBase` | 추상 | 등록 `DynamicEntryBoxBaseWidget` | 가로·세로 스택, 양 방향 줄바꿈, 겹침·원형의 여섯 배치, 항목 간격 패턴과 원형 배치 설정을 적용한다. | `Public/Components/DynamicEntryBoxBase.h` |
| `UEditableText` | 구체 | 등록 `EditableTextWidget` | UTF-8 원문, 읽기 전용·비밀번호, 클러스터 단위 이동·선택, 변경·확정. 플랫폼 조합 입력은 Win64 IMM 경로가 있으며 다른 플랫폼 및 실제 IME는 별도 검증. | `Public/Components/EditableText.h` |
| `UEditableTextBox` | 구체 | 등록 `EditableTextBoxWidget` | 배경과 여백을 가진 한 줄 편집기. 힌트·오류 표시, 포커스 또는 확정 시 전체 선택과 Escape 원문 복원을 제공한다. | `Public/Components/EditableTextBox.h` |
| `UExpandableArea` | 구체 | 등록 `ExpandableAreaWidget` | 헤더와 본문 슬롯, 접기·펼치기 및 상태 변경 콜백을 제공한다. | `Public/Components/ExpandableArea.h` |
| `UGridPanel` | 구체 | 등록 `GridPanelWidget` | 행·열·span·레이어·여백과 트랙별 Fill 가중치를 저장한다. 실행 중 행·열 가중치 갱신과 초기화를 지원한다. | `Public/Components/GridPanel.h` |
| `UHorizontalBox` | 구체 | 등록 `HorizontalBoxWidget` | 가로 스택에 자식을 추가. 슬롯별 Auto/Fill·여백의 게임 객체 노출은 슬롯 표 참조. | `Public/Components/HorizontalBox.h` |
| `UImage` | 구체 | 등록 `ImageWidget` | 논리 크기·tint와 Rocks Brush를 적용한다. UE의 soft texture·동적 머티리얼 자산 경로는 별도다. | `Public/Components/Image.h` |
| `UInputKeySelector` | 구체 | 등록 `InputKeySelectorWidget` | 일반 키·보조키·마우스 및 선택적 게임패드 입력. Escape/포커스 상실 때 취소한다. | `Public/Components/InputKeySelector.h` |
| `UInvalidationBox` | 구체 | 등록 `CachePanelWidget` | CachePanel 하위 트리의 그리기 결과를 무효화 사유에 따라 재사용. | `Public/Components/InvalidationBox.h` |
| `UListView` | 구체 | 등록 `ListViewWidget` | 객체 항목 추가·삽입·제거, ID로 표현한 선택 및 스크롤, 화면에 보이는 행의 재사용을 지원한다. | `Public/Components/ListView.h` |
| `UListViewBase` | 추상 | 등록 `ListViewBaseWidget` | 가시 행 가상화, 단일·다중 선택, 항목 탐색, 대상 스크롤 정렬과 취소, 휠 제어 및 선택 콜백. 관성·터치 이벤트 범위는 별도다. | `Public/Components/ListViewBase.h` |
| `UMenuAnchor` | 구체 | 등록 `MenuAnchorWidget` | 전면 Overlay의 메뉴 위치·창 안 맞춤, 열기·닫기·상태 통지 및 스코프 포커스 복귀를 제공한다. | `Public/Components/MenuAnchor.h` |
| `UMultiLineEditableText` | 구체 | 등록 `MultiLineEditableTextWidget` | Enter 문단 분리 및 자동 줄바꿈. 실제 플랫폼 IME 조합은 별도 검증. | `Public/Components/MultiLineEditableText.h` |
| `UMultiLineEditableTextBox` | 구체 | 등록 `MultiLineEditableTextBoxWidget` | 여러 줄 편집과 배경, 힌트·오류, 포커스 상실 시 확정을 제공한다. 독립 가로·세로 스크롤바는 별도 검증 대상이다. | `Public/Components/MultiLineEditableTextBox.h` |
| `UNamedSlot` | 구체 | 등록 `NamedSlotWidget` | UserWidget에서 자리를 이름으로 조회·교체·비울 수 있다. 중첩된 UserWidget은 별도 이름 범위를 가진다. | `Public/Components/NamedSlot.h` |
| `UNativeWidgetHost` | 구체 | 등록 `NativeWidgetHost` | 외부 Element를 연결하고 재구성 시 다시 부착. | `Public/Components/NativeWidgetHost.h` |
| `UOverlay` | 구체 | 등록 `OverlayWidget` | 자식을 같은 영역에 겹쳐 추가 순서대로 표시. | `Public/Components/Overlay.h` |
| `UPanelWidget` | 추상 | 등록 `PanelWidget` | 자식과 슬롯을 핸들·인덱스로 조회하고, 포함 여부 검사·제거·전체 비우기를 GC 참조와 화면 트리에 반영한다. | `Public/Components/PanelWidget.h` |
| `UPostBufferUpdate` | 구체 | 등록 `PostBufferUpdateWidget` | 여러 후처리 버퍼의 갱신 설정을 저장하고 그리기 순서에 맞춰 호스트에 제출한다. | `Public/Components/PostBufferUpdate.h` |
| `UProgressBar` | 구체 | 등록 `ProgressBarWidget` | 정규화된 진행률, 채움 방향·스타일·색·여백, marquee와 채움 브러시를 적용한다. | `Public/Components/ProgressBar.h` |
| `URetainerBox` | 구체 | 등록 `OffscreenLayerWidget` | 위상별 렌더 타깃 캡처, 명시적 갱신, 저장된 효과 머티리얼과 텍스처 파라미터·tint 합성을 지원한다. | `Public/Components/OffscreenLayer.h` |
| `URichTextBlock` | 구체 | 등록 `RichTextBlockWidget` | 이름 스타일·인라인 데코레이터의 문단 단위 셰이핑, 그림자·장식·말줄임을 제공한다. UE 머티리얼과 데이터 테이블의 직접 로딩은 별도다. | `Public/Components/RichTextBlock.h` |
| `USafeZone` | 구체 | 등록 `SafeZoneWidget` | 뷰포트 여백과 적용할 변을 지정. 실제 기기별 여백은 호스트가 제공. | `Public/Components/SafeZone.h` |
| `UScaleBox` | 구체 | 등록 `ScaleLayerWidget` | 정렬, 맞춤 방식, 확대·축소 허용 방향과 사용자 지정 배율. 상위 배율 무시 옵션은 별도. | `Public/Components/ScaleBox.h` |
| `UScrollBar` | 구체 | 등록 `ScrollBarWidget` | thumb 위치·크기와 방향, 드래그 입력과 스크롤 변경 콜백을 연결한다. | `Public/Components/ScrollBar.h` |
| `UScrollBox` | 구체 | 등록 `ScrollBoxWidget` | 가로·세로 스크롤, 뷰 비율, 자식으로 이동, 휠·터치·오른쪽 드래그 정책과 사용자 스크롤 이벤트를 제공한다. | `Public/Components/ScrollBox.h` |
| `USizeBox` | 구체 | 등록 `SizeBoxWidget` | 최소·최대 크기, 폭·높이 override 및 해제, 최소·최대 종횡비를 지원한다. | `Public/Components/SizeBox.h` |
| `USlider` | 구체 | 등록 `SliderWidget` | 값 범위·정규화·스텝·방향, 잠금·핸들 위치, 색과 캡처 수명 콜백을 적용한다. | `Public/Components/Slider.h` |
| `USpacer` | 구체 | 등록 `SpacerWidget` | 그리지 않는 고정 논리 크기의 영역. | `Public/Components/Spacer.h` |
| `USpinBox` | 구체 | 등록 `SpinBoxWidget` | 수치 범위·단위·표시 자리수, 스냅, 드래그·키보드 입력, 확정 및 드래그 수명 이벤트를 지원한다. | `Public/Components/SpinBox.h` |
| `UStackBox` | 구체 | 등록 `StackBoxWidget` | 실행 중 축 전환과 기존 자식 유지. | `Public/Components/StackBox.h` |
| `UTextBlock` | 구체 | 등록 `TextLabelWidget` | 지역화 본문, tint·글꼴 크기, 그림자·장식·최소 폭 및 말줄임을 적용한다. UE 폰트 머티리얼·윤곽과 별도로 대조한다. | `Public/Components/TextBlock.h` |
| `UTextLayoutWidget` | 추상 | 등록 `TextLayoutWidget` | 줄바꿈·정렬과 여백, 행 높이 및 마지막 줄 적용 여부, 최소 폭·그림자를 공통 배치에 보존한다. | `Public/Components/TextWidgetTypes.h` |
| `UThrobber` | 구체 | 등록 `ThrobberWidget` | 조각 수와 브러시, 가로·세로·알파 애니메이션을 제어한다. | `Public/Components/Throbber.h` |
| `UTileView` | 구체 | 등록 `TileViewWidget` | 목록 가상화에 타일 너비와 열 재배치 추가. | `Public/Components/TileView.h` |
| `UTreeView` | 구체 | 등록 `TreeViewWidget` | 안정 키로 노드 또는 전체 트리를 펼치고 접는다. 보이는 행만 만든다. | `Public/Components/TreeView.h` |
| `UUniformGridPanel` | 구체 | 등록 `UniformGridPanelWidget` | 균일한 셀 배치와 공통 여백·최소 셀 크기 저장 및 실행 중 변경을 지원한다. | `Public/Components/UniformGridPanel.h` |
| `UUserWidget` | 추상 | 등록 `UserWidget` | 생성 전후와 입력·포커스 훅, 뷰포트 추가·배치·제거, 이름 슬롯 교체, 안정 ID 애니메이션 클립 및 입력 모델/이벤트 바인딩이 있다. UE 시퀀스 플레이어 전체와 디자인 타임은 별도다. | `Public/Blueprint/UserWidget.h` |
| `UVerticalBox` | 구체 | 등록 `VerticalBoxWidget` | 세로 스택에 자식을 추가. 슬롯별 Auto/Fill·여백은 슬롯 표 참조. | `Public/Components/VerticalBox.h` |
| `UViewport` | 구체 | 등록 `ViewportWidget` | 별도 레지스트리·레벨·액터 수명을 갖는 뷰포트 월드를 생성한다. 액터 스폰, 카메라 위치·회전·시야각, 렌더러 콜백과 외부 장면 텍스처 경로가 있다. 조명·show flag의 개별 제어는 별도다. | `Public/Components/Viewport.h` |
| `UWidget` | 추상 | 등록 `Widget` | 변환·피벗·불투명도·가시성·휘발성·클리핑, 커서·hover·포커스·캡처, 지연 툴팁, 접근성 이름·요약 및 방향별 탐색 규칙을 제공한다. 함수 이름에 의한 동적 호출은 별도다. | `Public/Components/Widget.h` |
| `UWidgetSwitcher` | 구체 | 등록 `WidgetSwitcherWidget` | 활성 페이지 하나만 측정·그리기. 자식 제거 시 활성 인덱스 보정. | `Public/Components/WidgetSwitcher.h` |
| `UWindowTitleBarArea` | 구체 | 등록 `WindowTitleBarAreaWidget` | 네이티브 창 이동·제목 표시줄 더블클릭 동작을 호스트에 전달. | `Public/Components/WindowTitleBarArea.h` |
| `UWrapBox` | 구체 | 등록 `WrapStackWidget` | 줄바꿈과 간격, 자식별 한 줄 차지·잔여 공간 채움 및 두 축 정렬. 폭 경계 규칙은 슬롯 표 참조. | `Public/Components/WrapBox.h` |

## 패널 슬롯

각 슬롯이 보존하는 값, 자식이 화면에 연결된 뒤의 변경 경로와 이전 애셋의 기본값을 대조했다.

| UE 슬롯 | Rocks 전용 타입 | 배치 속성 또는 확인할 항목 | UMG 헤더 |
|---|---|---|---|
| `UBackgroundBlurSlot` | `BackgroundBlurWidgetSlot` | SetLayout으로 여백·가로 및 세로 정렬을 바꾼다. 부착된 슬롯의 핸들과 내용을 유지한다. | `Public/Components/BackgroundBlurSlot.h` |
| `UBorderSlot` | `BorderWidgetSlot` | SetLayout으로 여백·가로 및 세로 정렬을 바꾼다. 부착된 슬롯의 핸들과 내용을 유지한다. | `Public/Components/BorderSlot.h` |
| `UButtonSlot` | `ButtonWidgetSlot` | SetLayout으로 여백·가로 및 세로 정렬을 바꾼다. 부착된 슬롯의 핸들과 내용을 유지한다. | `Public/Components/ButtonSlot.h` |
| `UCanvasPanelSlot` | `AnchorCanvasWidgetSlot` | 앵커·오프셋·정렬·AutoSize·z 순서를 실행 중 바꿀 수 있다. 자식을 유지하며 히트 테스트 순서도 바뀐다. | `Public/Components/CanvasPanelSlot.h` |
| `UGridSlot` | `GridWidgetSlot` | 행·열·span·layer·padding, 수평·수직 내용 정렬과 nudge. nudge는 트랙 측정에 영향을 주지 않는다. 부착 후 UpdateLayout으로 레이어를 바꿔도 기존 자식을 유지한다. | `Public/Components/GridSlot.h` |
| `UHorizontalBoxSlot` | `HorizontalBoxWidgetSlot` | 주축 Auto/Fill/FillContent, 가중치·최소/최대 크기·여백 및 두 축의 내용 정렬을 실행 중 갱신한다. | `Public/Components/HorizontalBoxSlot.h` |
| `UOverlaySlot` | `OverlayWidgetSlot` | SetLayout으로 여백·가로 및 세로 정렬을 바꾼다. 부착된 슬롯의 핸들과 내용을 유지한다. | `Public/Components/OverlaySlot.h` |
| `UPanelSlot` | `WidgetSlot` | 콘텐츠를 GC 참조로 보관한다. Parent는 현재 연결된 부모를 반환하며 제거된 슬롯은 빈 핸들을 반환한다. | `Public/Components/PanelSlot.h` |
| `USafeZoneSlot` | `SafeZoneWidgetSlot` | 타이틀/액션 안전 영역과 각 변의 배율, 내용 여백·정렬을 따로 지정한다. | `Public/Components/SafeZoneSlot.h` |
| `UScaleBoxSlot` | `ScaleLayerWidgetSlot` | SetLayout으로 여백·가로 및 세로 정렬을 바꾼다. 부착된 슬롯의 핸들과 내용을 유지한다. | `Public/Components/ScaleBoxSlot.h` |
| `UScrollBoxSlot` | `ScrollBoxWidgetSlot` | Auto/Fill 가중치와 여백·정렬을 실행 중 갱신한다. 애셋 복원 뒤에도 두 설정을 함께 적용한다. | `Public/Components/ScrollBoxSlot.h` |
| `USizeBoxSlot` | `SizeBoxWidgetSlot` | SetLayout으로 여백·가로 및 세로 정렬을 바꾼다. 부착된 슬롯의 핸들과 내용을 유지한다. | `Public/Components/SizeBoxSlot.h` |
| `UStackBoxSlot` | `StackBoxWidgetSlot` | 주축 Auto/Fill/FillContent, 가중치·최소/최대 크기·여백 및 두 축의 내용 정렬을 실행 중 갱신한다. | `Public/Components/StackBoxSlot.h` |
| `UUniformGridSlot` | `UniformGridWidgetSlot` | 행·열·수평 및 수직 내용 정렬. 공통 셀 여백은 UniformGrid가 소유한다. 부착 후 UpdateLayout으로 셀과 정렬을 갱신한다. | `Public/Components/UniformGridSlot.h` |
| `UVerticalBoxSlot` | `VerticalBoxWidgetSlot` | 주축 Auto/Fill/FillContent, 가중치·최소/최대 크기·여백 및 두 축의 내용 정렬을 실행 중 갱신한다. | `Public/Components/VerticalBoxSlot.h` |
| `UWidgetSwitcherSlot` | `WidgetSwitcherWidgetSlot` | SetLayout으로 여백·가로 및 세로 정렬을 바꾼다. 부착된 슬롯의 핸들과 내용을 유지한다. | `Public/Components/WidgetSwitcherSlot.h` |
| `UWindowTitleBarAreaSlot` | `WindowTitleBarAreaWidgetSlot` | SetLayout으로 여백·가로 및 세로 정렬을 바꾼다. 부착된 슬롯의 핸들과 내용을 유지한다. | `Public/Components/WindowTitleBarAreaSlot.h` |
| `UWrapBoxSlot` | `WrapStackWidgetSlot` | 네 변 여백·가로 및 세로 정렬. 강제 개행, 임계값 미만 한 줄 차지, 마지막 항목의 남은 공간 채우기. 기본 WidgetSlot으로 만든 이전 에셋을 읽을 수 있으며 전용 슬롯에서는 UpdateLayout으로 실제 배치도 갱신한다. | `Public/Components/WrapBoxSlot.h` |

## 속성·함수 대조

[UiWidgetMembers.ko.tsv](UiWidgetMembers.ko.tsv)에는 UMG 73형식에 직접 선언된 UPROPERTY 604개와 UFUNCTION 636개가 있다. 상속받는 멤버는 선언한 기반형 행에서 확인한다. UE 선언 위치, 조건부 플래그, 타입 및 Rocks 쪽 근거 위치를 필터링할 수 있다. 테스트 실행 여부와 별도로 소스에서 확인한 범위를 표시한다.

| TSV 상태 | 건수 | 판정 기준 |
|---|---:|---|
| 구현 근거 | 440 | 해당 동작을 담당하는 Rocks C++ 구현을 확인했다. UE 전체 동작과 Blueprint 동등성을 뜻하지 않는다. |
| 부분 대응 | 293 | 호출 형태·저장 단위·렌더 백엔드·이벤트 수명 등 차이가 있거나 네이티브 계층만 지원한다. |
| 동명 후보 | 2 | 이름 일치만 발견했다. 지원 여부 판정에는 사용하지 않는다. |
| 직접 대응 미확인 | 483 | 현재 대응 타입과 기반형·네이티브 소스에서 직접 대응을 확인하지 못했다. |
| 에디터 조건·추가 검토 | 22 | 에디터 조건부 선언으로 별도의 디자인 타임 동작 검토가 필요하다. |

WidgetFieldBindings가 프로퍼티 경로와 안정 필드 ID로 값을 갱신한다. WidgetAsset은 ViewModel 바인딩과 이벤트 연결을 컴파일하며 WidgetEventCatalog가 등록된 메서드·이벤트 ID와 시그니처를 확인한다. 임의 메서드를 이름으로 탐색해 호출하는 기능은 별도다. 접근성 트리는 창 좌표·역할·포커스·이름·요약을 제공한다. Windows UIA 및 macOS NSAccessibility와의 OS 연결은 미구현이다. 편집 입력은 Win64 IMM 경로를 제공하며 다른 플랫폼과 실제 한글 후보 창은 별도 검증 대상이다.

## 테스트 근거

Windows Debug에서 `EngineTests`와 `UiDemo`를 빌드하고 `EngineTests --filter Ui` 61개 모두 통과했다. `UiDemo --surface-test`와 `UiDemo --self-test`는 모두 종료 코드 0이었다. D3D11 WARP에서 렌더 타깃의 픽셀을 읽어 보관 위상 사이 프레임 유지, 블러 경계의 혼합 색, 출력과 후처리 버퍼의 바이트 일치를 확인했다. 하드웨어 GPU, 다른 그래픽 백엔드, 실제 IME 후보 창의 시각 동작은 이 실행의 검증 범위 밖이다.

전체 `EngineTests` 4,581개 중 4,578개가 통과했다. 실패한 `Process_ParentHandlesAreNotInherited`, `Process_UnicodeExecutableAndWorkingDirectory`, `ProjectLayout_NativePathLimitOnDisk`는 이 워크트리의 기존 전체 실행 기록에서도 실패한 동일한 세 시험이다. 각각 자식 프로세스 종료 코드, 실행 파일 하드 링크 생성, 긴 경로 파일 생성에서 실패했다.

UI 모듈과 UiDemo의 C++ 헤더 및 소스 377개가 저장소의 `clang-format --dry-run --Werror` 검사를 통과했다.

- `UiWidget_StackAndAlignedSlotsSurviveTemplateInstantiation`과 `UiWidget_WrapSlotsReflowAfterTemplateInstantiation`은 애셋 복원 후 여백·정렬·가용 폭 경계의 화면 좌표를 검사한다.
- `UiFramework_ComboPopupUsesOuterOverlayAndSelectsByPointer`와 `UiWidget_ComboModelsKeepSelectionWhenAnEarlierOptionIsRemoved`는 메뉴의 부모, 포인터 선택 및 항목을 제거한 뒤의 선택 상태를 검사한다.
- `UiWidget_EditableTextUsesClusterBoundariesAndPersistsInput`과 `UiWidget_RichTextStylesAndInlineImagePreserveClusterLayout`은 글자 클러스터의 경계와 텍스트 재구성을 검사한다.
- `UiFramework_SpinBoxCommitsTextAndRejectsInvalidEntry`, `UiFramework_KeySelectorCapturesDeviceButtonsAndPersistsNames`, `UiFramework_RetainedTextureReplacesOnlyChildPainting`은 수치 입력·장치 버튼·캡처 텍스처의 표시 경로를 각각 검사한다.
- `UiFramework_GridNudgeOffsetsContentWithoutChangingTrackSize`는 그리드 이동량이 측정값을 바꾸지 않는지, 셀 정렬과 레이어 변경 뒤에도 같은 자식인지 검사한다.
- `UiWidget_NamedSlotsReplaceLiveContentAndKeepOwnershipOnFailure`와 `UiWidget_FieldBindingUpdatesLiveElementAfterNotificationAndDetaches`는 이름 있는 슬롯 교체와 필드 변경 구독 해제를 검사한다.
- `UiWidget_GridFillAndScrollSizeSurviveRebuild`는 그리드 Fill·스크롤 슬롯 크기·여백의 애셋 복원과 라이브 재배치를 검사한다. 캔버스 테스트는 겹친 두 버튼의 z 순서를 변경한 뒤 같은 위치의 히트 대상을 검사한다.
- `UiWidget_CanvasSlotSurvivesTemplateInstantiation`은 앵커·오프셋을 에셋에 기록하고 복제된 화면의 실제 좌표를 검사한다.
- `UiWidget_DynamicEntriesReuseObjectsInsideSafeZone`은 슬롯에서 제거한 동적 엔트리의 재사용과 안전 영역 너비를 검사한다.
- `UiFramework_ListRecyclesVisibleRowsAndReleasesFocus`와 `UiFramework_TilesReflowAndTreeFlattensOnlyExpandedNodes`는 네이티브 목록의 재사용·포커스·가상화를 검사한다. 리플렉션 타입별 애셋 저장 검증은 별도다.
- `UiWidget_TextShaperFailurePreservesPublishedText`는 모의 셰이퍼를 사용한다. 실제 한글 조합·아랍어·컬러 이모지·폰트 대체를 포함하는 텍스트 검증은 폰트 백엔드와 플랫폼 입력까지 연결해야 한다.

## 엔진 및 플러그인의 추가 파생형

`Engine/Source`·`Engine/Plugins`·`Engine/Platforms`에서 대소문자를 구분하지 않고 `.Build.cs` 파일을 찾아 모듈 소유 헤더를 조사했다. `UCLASS`와 C++ 상속 선언을 대조하고, 상속을 여러 단계 거쳐 UWidget 또는 UPanelSlot에 도달하는 형식만 포함했다. 플러그인은 `.uplugin`의 모듈 HostType으로, 엔진 모듈은 Source 하위의 Runtime·Editor 구분으로 분류했다. 에디터 모듈과 런타임 모듈을 함께 검사하되, 런타임 모듈의 클래스라도 실제로는 특정 도구·프로젝트에서만 쓰일 수 있다.

모듈 디렉터리 2,508개에서 C++ 헤더 45,871개를 읽었고 그중 UCLASS가 있는 헤더는 11,601개였다. UMG 이외의 런타임 모듈에서 위젯 76형식과 슬롯 2형식, 에디터 모듈에서 위젯 32형식과 슬롯 0형식을 찾았다. 스캔은 호스트의 이 엔진 체크아웃에 있는 C++ 선언 기준이다. Blueprint 콘텐츠, 프로젝트 전용 플러그인, Slate 전용 SWidget, 외부 SDK, 바이너리로만 설치한 모듈은 이 수에 포함하지 않는다.

| 런타임 모듈 | UE 파생형 | Rocks 대조 범위 |
|---|---|---|
| `AdvancedWidgets` | `URadialSlider` | `RadialSliderWidget`·`RadialSlider`가 각도 기반 포인터 입력, 원호, 노브, 범위·스텝 및 키보드를 제공한다. 사용자 지정 값 커브·태그·수직 드래그 모드는 미구현. |
| `AudioWidgets` | `UAudioFrequencyRadialSlider` · `UAudioFrequencySlider` · `UAudioMaterialButton` · `UAudioMaterialEnvelope` · `UAudioMaterialKnob` · `UAudioMaterialMeter` · `UAudioMaterialSlider` · `UAudioMeter` · `UAudioOscilloscope` · `UAudioRadialSlider` · `UAudioSlider` · `UAudioSliderBase` (추상) · `UAudioVectorscope` · `UAudioVolumeRadialSlider` · `UAudioVolumeSlider` | 공통으로 쓸 수 있는 값 입력은 `SliderWidget`, `RadialSliderWidget`, `SpinBoxWidget`이다. 파형·벡터스코프·미터는 Audio 모듈의 분석 샘플, 갱신 시계 및 시각화가 필요하다. 오디오 머티리얼 스타일과 로그 주파수·데시벨 변환은 별도. |
| `CommonUI` | `UAnalogSlider` · `UCommonActionWidget` · `UCommonActivatableWidget` · `UCommonActivatableWidgetContainerBase` (추상) · `UCommonActivatableWidgetQueue` · `UCommonActivatableWidgetStack` · `UCommonActivatableWidgetSwitcher` · `UCommonAnimatedSwitcher` · `UCommonBorder` · `UCommonBoundActionBar` · `UCommonBoundActionButton` (추상) · `UCommonButtonBase` (추상) · `UCommonButtonInternalBase` · `UCommonCustomNavigation` · `UCommonDateTimeTextBlock` · `UCommonHardwareVisibilityBorder` · `UCommonHierarchicalScrollBox` · `UCommonLazyImage` · `UCommonLazyWidget` · `UCommonListView` · `UCommonLoadGuard` · `UCommonNumericTextBlock` · `UCommonRichTextBlock` · `UCommonRotator` · `UCommonTabListWidgetBase` (추상) · `UCommonTextBlock` · `UCommonTileView` · `UCommonTreeView` · `UCommonUserWidget` · `UCommonVideoPlayer` · `UCommonVisibilitySwitcher` · `UCommonVisibilitySwitcherSlot` (슬롯) · `UCommonVisualAttachment` · `UCommonWidgetCarousel` · `UCommonWidgetCarouselNavBar` · `UDEPRECATED_UCommonVisibilityWidgetBase` · `ULoadGuardSlot` (슬롯) | `Screen`·`ScreenContainer`·`ScreenInputRouter`가 활성화, 스택·큐, 뒤로 가기와 포커스 복원을 제공한다. `UCommonWidgetCarousel`과 `UCommonWidgetCarouselNavBar`는 `CarouselWidget`·`CarouselNavBarWidget`이 페이지 전환과 탐색 버튼을 제공한다. 탐색 버튼은 네이티브 팩토리로 지정한다. 목록은 UiWidget의 가상화 모델을 재사용할 수 있다. 플랫폼별 액션 글리프, 비동기 에셋 로드, 동영상 플레이어, 전용 애니메이션·전환 정책은 별도로 대조한다. |
| `CustomizableObject` | `UDGGUI` (추상) | 커스터마이저 디버그 GUI인 UDGGUI는 인스턴스 생성·파라미터 갱신을 제공하는 CustomizableObject 서비스에 의존한다. 일반 UserWidget이 전용 서비스를 제공하지 않는다. |
| `DisplayCluster` | `UDisplayClusterLabelWidget` (추상) | 클러스터 노드·뷰포트 식별 및 nDisplay 출력 배치가 필요하다. TextLabelWidget은 노드 라벨 그리기에서 사용할 수 있다. |
| `LevelSequence` | `ULevelSequenceBurnIn` | 재생 중인 시퀀스 플레이어의 프레임·타임코드·촬영 정보를 오버레이에 공급해야 한다. UserWidget은 오버레이 내용의 기반이다. |
| `MetaHumanLocalLiveLinkSource` | `UMetaHumanVideoBaseLiveLinkSubjectMonitorWidget` | LiveLink 영상 프레임, 소스 연결 상태 및 MetaHuman 처리 결과를 공급하는 모니터가 필요하다. ViewportWidget은 호스트 텍스처를 표시한다. |
| `ModularViewports` | `USceneViewport` | 씬 뷰포트 소유권, 카메라 제어와 입력의 월드 전달이 필요하다. ViewportWidget은 외부 장면 텍스처와 자식을 합성한다. |
| `MovieRenderPipelineCore` | `UMovieGraphBurnInWidget` (추상) · `UMovieGraphRenderPreviewWidget` (추상) · `UMovieRenderDebugWidget` (추상) | 렌더 작업과 샷별 상태, 프레임 출력·미리보기 텍스처가 필요하다. ViewportWidget은 전달받은 텍스처의 표시를 맡을 수 있다. |
| `MovieRenderPipelineSettings` | `UMoviePipelineBurnInWidget` (추상) | 샷·프레임별 오버레이 데이터를 제공하는 시네마틱 출력 파이프라인과 합성 단계가 필요하다. |
| `NamingTokensUI` | `UNamingTokensEditableText` | 편집 컨트롤은 EditableTextWidget과 대응한다. 명명 토큰 해석·자동 완성 및 컨텍스트별 후보 공급은 별도다. |
| `NiagaraUIRenderer` | `UNiagaraUIWidget` | 파티클 시뮬레이션의 스프라이트·리본 버텍스를 UI 좌표와 배치에 연결하는 렌더러가 필요하다. 일반 ImageWidget의 단일 브러시와 구분한다. |
| `SubtitlesAndClosedCaptions` | `USubtitleWidget` (추상) | 재생 시각에 따라 자막 큐를 선택하고 화자·언어·접근성 설정을 적용하는 공급자가 필요하다. TextLabelWidget과 RichTextBlockWidget은 텍스트 표시를 담당할 수 있다. |
| `Synthesis` | `USynth2DSlider` · `USynthKnob` | 합성기 파라미터 매핑, 두 축 동시 입력 및 회전 노브가 필요하다. SliderWidget은 한 축의 값 제어를 제공한다. |
| `TakeRecorder` | `UTakeRecorderOverlayWidget` | 녹화 소스와 테이크 번호·타임코드·녹화 수명 이벤트를 공급할 전용 서비스가 필요하다. |
| `UIFramework` | `UUIFrameworkButtonWidget` · `UUIFrameworkCustomButtonWidget` · `UUIFrameworkTouchActionWrapperWidget` | UE 플러그인의 네트워크 복제 UI 모델 및 터치 액션 래퍼를 확인해야 한다. Rocks ButtonWidget의 일반 입력·클릭만으로 복제 수명을 지원할 수 없다. |
| `VCamCore` | `UVCamStateSwitcherWidget` · `UVCamWidget` (추상) | 가상 카메라 컴포넌트, 연결 지점·상태별 입력과 라이브 카메라 연동이 필요하다. WidgetSwitcherWidget은 페이지 전환을 담당할 수 있다. |
| `VPUtilities` | `UVPAssetThumbnailWrapperWidget` | 에셋 미리보기 텍스처를 얻고 수명을 보장하는 썸네일 서비스가 필요하다. ImageWidget은 전달된 리소스를 표시한다. |
| `WebBrowserWidget` | `UWebBrowser` | 탐색, 문서·자바스크립트 통신, 팝업과 IME를 제공하는 플랫폼별 브라우저 엔진이 필요하다. NativeWidgetHost는 네이티브 UI Element를 받아 붙인다. |
| `XRCreative` | `UXRCreativePalette` · `UXRCreativePaletteTab` · `UXRCreativePaletteToolTab` | XR 도구 등록과 월드 공간 팔레트, 컨트롤러 포인터·레이 입력이 필요하다. 탭과 일반 패널은 UI 레이아웃 단계만 맡을 수 있다. |

에디터 모듈에서도 같은 상속 계층이 쓰인다. 인게임 대응 수를 계산할 때 다음 형식은 별도로 집계한다.

| 에디터 모듈 | UE 파생형 |
|---|---|
| `Blutility` | `UEditorUtilityButton` · `UEditorUtilityCheckBox` · `UEditorUtilityCircularThrobber` · `UEditorUtilityComboBoxKey` · `UEditorUtilityComboBoxString` · `UEditorUtilityDialogWidget` (추상) · `UEditorUtilityEditableText` · `UEditorUtilityEditableTextBox` · `UEditorUtilityExpandableArea` · `UEditorUtilityInputKeySelector` · `UEditorUtilityListView` · `UEditorUtilityMultiLineEditableText` · `UEditorUtilityMultiLineEditableTextBox` · `UEditorUtilityProgressBar` · `UEditorUtilityScrollBar` · `UEditorUtilityScrollBox` · `UEditorUtilitySlider` · `UEditorUtilitySpinBox` · `UEditorUtilityThrobber` · `UEditorUtilityTreeView` · `UEditorUtilityWidget` (추상) · `UToolMenuWidget` |
| `LearningAgentsTrainingEditor` | `ULearningAgentsDepthMapWidget` |
| `ModelViewViewModelEditor` | `UMVVMWidgetFieldPathHelperTest` (추상) |
| `ObjectMixerEditor` | `UObjectMixerEditorUWidget` |
| `ScriptableEditorWidgets` | `UDetailsView` · `UPropertyViewBase` (추상) · `USinglePropertyView` |
| `UMGEditor` | `UAssetThumbnailWidget` |
| `UMGToolSet` | `UUMGTestWidgetWithBindings` |
| `UserToolBoxCore` | `UUTBCommandUMGUI` |
| `VREditor` | `UVREditorBaseUserWidget` (추상) |

## 교차검증 방법

1. UMG 전체 헤더에서 UCLASS 메타가 없어도 클래스 선언과 첫 번째 U 접두 기반형을 추출했다. 파생 클래스를 재귀 추적해 위젯과 슬롯의 이름 집합을 구했다.
2. 엔진 전체에서는 주석을 제거한 UCLASS 선언을 모듈별로 수집하고 다시 상속을 추적했다. UMG에 대해 두 집합이 55개 위젯과 18개 슬롯의 이름 단위로 일치했다. CommonUI 헤더에 사용 예로 적힌 `UMyCustomListView`처럼 주석에만 등장한 선언은 제외했다.
3. UMG 각 타입 본문에서 주석과 다른 구조체의 선언을 제외하고 UPROPERTY와 UFUNCTION의 소속·이름·소스 위치·조건부 컴파일 범위를 얻었다. 별도 줄 단위 스캐너가 73개 클래스 안의 1,240개 매크로 위치를 원본 경로와 줄 번호까지 재확인했다. `UNativeWidgetHost`에는 자체 UPROPERTY 또는 UFUNCTION 선언이 없다.
4. Rocks 쪽은 공용 헤더, 소스, 활성 리플렉션 ID를 대조했다. UI 클래스의 속성 지원은 실제 공개 함수, 위젯 생성 경로, 슬롯 재구성, 테스트가 커버하는 동작을 따로 확인한다.
