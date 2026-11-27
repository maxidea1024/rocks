## 21. Widget 층 정렬(UW9)

UW9는 병합 전 마지막 재작업 단계다. 9.1절 리뷰가 Widget 층에서 찾은 다섯 가지를 고친다.
- UD12와 다른 이름
- Widget 층에 묶인 편집 텍스트 Element
- 적용되지 않는 화면 입력 설정
- Element 소멸자에서 도는 사용자 코드
- UV7 음성 시험 없음

손으로 쓴 속성 표는 UT0 CU 지정자가 생긴 뒤 바꾼다(그때까지 임시).

### 21.1 이름(UD12)

UD12 (2)는 UE 고유 합성어를 서술형 이름으로 바꾸라고 한다. (3)은 같은 이름의 Element를 감싸는 Widget에 `Widget` 접미를 붙이라고 한다. 9.1절 리뷰가 찾은 다섯 이름을 아래처럼 바꾼다.

| 지금(UE 이름 그대로) | 바꾼 이름 | 근거 |
|---|---|---|
| Element `RetainerBox` | `OffscreenLayer` | UE `SRetainerWidget`. 자식을 화면 밖 대상에 그려 두고 그 결과를 다시 그린다. "Retainer"는 UD12 (2)의 예다 |
| `RetainerBoxWidget` | `OffscreenLayerWidget` | 감싸는 Element 이름 + `Widget` |
| `CanvasPanelWidget`·`CanvasWidgetSlot` | `AnchorCanvasWidget`·`AnchorCanvasWidgetSlot` | Element가 `AnchorCanvas`다 |
| `ScaleBoxWidget`·`ScaleBoxWidgetSlot` | `ScaleLayerWidget`·`ScaleLayerWidgetSlot` | Element가 `ScaleLayer`다 |
| `WrapBoxWidget`·`WrapBoxWidgetSlot` | `WrapStackWidget`·`WrapStackWidgetSlot` | Element가 `WrapStack`다 |
| `InvalidationBoxWidget` | `CachePanelWidget` | Element가 `CachePanel`다(UE "Invalidation Panel") |

- **리플렉션 ID.** `ids rename`으로 바꾼다(`UiWidget.rocksids.json` — ID는 그대로이고 옛 이름은 별칭으로 남는다). 저장된 Widget 에셋은 ID로 형식을 찾으므로 다시 만들 필요가 없다.
- **바꾸지 않는 이름.** 업계 일반 명사(UD12 (1))인 `HorizontalBox`·`VerticalBox`·`Border`·`GridPanel`·`ScrollBox`·`SafeZone`·`SizeBox`·`Spacer`·`Throbber`는 Element 이름과 달라도 그대로 둔다. 게임 UI 작성자가 UMG에서 익힌 이름이고, UD12가 바꾸라고 한 범위 밖이다.
- **커밋.** 동작 불변 기계 변경이라 따로 커밋한다(파일 이름·클래스·include·문서 대응표·`Docs/Implementation` 표·ID 명세).

### 21.2 편집 텍스트 Element(UiFramework)

- **지금.**
  - `EditableTextElement`(UiWidget Private)가 한 줄·여러 줄 편집을 맡는다. IME 조합, 클립보드, 64단계 undo·redo, 비밀번호, 힌트, 오류 표시를 갖췄다.
  - 생성자가 `ObjectRegistry`·`ObjectHandle`을 받는다. 글자 상태는 `EditableTextWidget`(Object)에 있고, 편집할 때마다 등록부에서 Widget을 다시 찾아 바꾼다.
  - 그래서 에디터(Element 층)에서는 쓸 수 없다(15장 1절).
- **바꿈(UE `SEditableText`·`SMultiLineEditableText` ↔ `UEditableText`).**
  - UiFramework `Controls/EditableText`가 글자·선택·조합·이력을 스스로 든다. `Args`는 글자, 힌트, 글꼴 크기, 비밀번호, 읽기 전용, 여러 줄, 상자 모양, 최대 길이다.
  - 사건은 delegate다:
    - `onTextChanged(StringView)` — UE `OnTextChanged`. 조합 중에는 부르지 않는다.
    - `onTextCommitted(StringView, TextCommitMethod)` — UE `OnTextCommitted`, `ETextCommit` Default·OnEnter·OnUserMovedFocus·OnCleared.
    - `onFocusReceived`·`onFocusLost`.
  - 글자 셰이핑은 UiFramework `TextShaper` 인터페이스로 받는다. 지금의 `WidgetTextShaper`가 이 이름으로 내려온다. 셰이핑이 화면 언어·글꼴·아틀라스에 묶이는 것은 Widget 층의 일이 아니라 호스트의 일이기 때문이다. 이름의 `Widget` 접두도 뺀다.
  - Widget 층 `EditableTextWidget`·`MultiLineEditableTextWidget`·`…BoxWidget`은 UE `UEditableText`처럼 Element를 만들고 속성을 내린다. Element 사건을 받아 자기 속성(`text`)을 바꾸고 통지한다. `BindLiveWidgetEvent`로 수명을 확인하는 방식은 지금의 다른 Widget과 같다.
- **동작 불변.** 이동 뒤에도 19.2절 IME 계약, undo 묶음, 비밀번호 마스킹, 오류 캡션이 같아야 한다. 지금의 `EditableTextWidgetTests` 전부가 그대로 통과해야 한다. Element 층 시험(Widget 없이 같은 장면)을 더한다.
- **버린 대안.** Element가 Widget 모델 인터페이스(`EditableTextModel`)를 받는 방식이 있다. 이렇게 하면 두 층의 상태가 하나로 남지만, 에디터가 그 인터페이스를 구현해야 하고 UE 구조(Element가 글자를 든다)와 다르다.

### 21.3 화면 입력 설정 적용(UE CommonUI `FUIInputConfig`)

- **지금.**
  - `ScreenInputConfig`(mode·showCursor·captureMouse·ignoreMove·ignoreLook)는 화면 속성으로 저장된다.
  - 그런데 `ScreenInputRouter`는 `mode`만 라우팅에 쓴다. 커서·마우스 붙잡기·이동·시선 무시는 어디에도 적용되지 않는다.
- **바꿈(UE `UCommonUIActionRouterBase::ApplyUIInputConfig`, CommonUIActionRouterBase.cpp 1866–1960행).**
  - 활성 화면의 설정이 바뀔 때만 적용한다(1868행). 다음 셋이 계기다: 화면 활성·비활성, 화면 스택 변화, 활성 화면의 `SetInputConfig`.
  - 적용은 호스트 인터페이스 `ScreenInputHost`로 넘긴다. UE에서 이 일은 PlayerController와 GameViewportClient가 한다. Rocks에서는 게임 쪽(World·Gameplay)이 구현하고, UiScreens는 Gameplay에 의존하지 않는다.
    - `SetIgnoreMoveInput(bool)`·`SetIgnoreLookInput(bool)`: 이전 값과 다를 때만 부른다. 이전에 true로 부른 적이 없으면 false를 부르지 않는다(1888–1899행).
    - `SetCursorVisible(bool)`·`SetMouseCaptured(bool)`: 1908–1960행의 capture 방식을 Rocks의 두 값으로 줄인 것이다.
    - `FlushPressedKeys()`: Menu로 바뀔 때 한 번 부른다(1901–1906행). 눌린 채 남은 게임 입력을 푼다. UE는 다음 tick으로 미루는데, Rocks는 라우터가 다음 입력 처리 앞에서 부른다(명시 순서 — 시간 경쟁 없음).
  - 게임패드 아날로그 반복과 눌린 UI 키는 Menu ↔ Game 전환 때 `Application::CancelGamepadKeys`로 푼다(20.8절).
- **플랫폼(PW 보강).**
  - `platform::Window`에 `SetCursorVisible(bool)`·`SetMouseLocked(bool)`을 더한다(UE `FReply::LockMouseToWidget`·`SetShowMouseCursor`).
  - Win64는 `ShowCursor` 계수와 `ClipCursor`(창 client 영역)로 한다. 다시 적용해야 하는 때가 있다: 창 이동·크기 변경·활성화. 잃는 때도 있다: 비활성·최소화.
  - 다른 backend는 기본 구현이 `Unsupported`다. Mac은 feature/platform-mac에서 맞춘다.
  - 고정밀 상대 이동(UE `UseHighPrecisionMouseMovement` — raw input)은 이번 범위 밖이다. 10장 위험 목록에 남긴다.
- **UiDemo.** 게임 HUD 표본이 `ScreenInputHost`를 구현해 창에 적용한다. self-test가 적용 횟수를 단언한다.

### 21.4 Element 소멸자의 사용자 코드

규칙: **Element·Widget 소멸자는 사용자 코드(가상 사용자 hook, 사용자 delegate, 사용자 원본 콜백)를 부르지 않는다.** 소멸은 SharedPtr 마지막 참조가 사라지는 아무 곳에서나 일어난다. 그 자리에서 사용자 코드가 돌면 세 가지 위험이 있다: 반쯤 부서진 부모, 등록부 재진입, GC 중 Object 접근.

UE는 `SObjectWidget` 소멸자가 `NativeDestruct`를 부르지만, Rocks는 이 점을 따르지 않는다(개선).
- **찾은 곳.**
  - `UserWidgetRoot` 소멸자 → `Detach` → `UserWidget::OnDestruct`
  - `ScreenContainer` 소멸자 → `Screen::OnDeactivated`
  - `ListView` 소멸자 → `TableRowSource::ReleaseRow`
  - 나머지(`MenuBar`·`MenuAnchor`·`DockManager`·편집 텍스트·`ComboBoxWidgetBase`·`MenuAnchorWidget`)는 엔진 내부 정리라 남긴다.
- **명시 경로.** 사용자 해체 통지는 명시 해제 경로에서 바로 부른다(UE `ReleaseSlateResources`).
  - `UserWidget::ReleaseElements` → `OnDestruct`(지금과 같다)
  - `ScreenContainerWidget::ReleaseElements` → 활성 화면 `OnDeactivated`
  - `ListViewBaseWidget::ReleaseElements` → `ListView::ReleaseAllRows`(행마다 `ReleaseRow` — UE `HandleRowReleased`)
- **미룬 해체 통지.** 명시 해제 없이 Element가 부서지면 소멸자는 통지를 `UiHostServices`의 해체 대기열에 넣고 끝낸다.
  - 대기열 항목은 대상 Object의 `RootRef`를 들어, 통지가 돌 때까지 GC가 수거하지 않는다.
  - 대기열은 다음 때 한 번에 돈다: `Application::BeginFrame` 맨 앞, 그리고 호스트가 창을 부수기 전에 부르는 `Application::RunDeferredTeardown()`.
  - 결과적으로 해체 통지는 항상 한 번, 소멸자 밖에서 돈다.
  - 호스트 묶음을 찾을 수 없는 Element(앱에 붙은 적 없음)는 생성 통지도 받은 적이 없으므로 넣을 것이 없다.
  - `Application` 소멸 때 남은 항목은 부르지 않고 경고 로그로 개수를 남긴다(소멸자 규칙).
- **ListView.** 소멸자는 행 원본을 부르지 않는다. 행 Widget의 풀 반환은 `ReleaseAllRows`나 `WidgetPool` 소멸이 맡는다(UE `ResetPool`).
- **policy.** UI 모듈 Element·Widget 소멸자 본문에서 사용자 hook 이름(`OnDestruct`·`OnDeactivated`·`ReleaseRow`·`Execute`)을 부르는 줄을 거부한다(`ui-destructor-user-code`). 엔진 내부 정리는 hook 이름을 쓰지 않으므로 걸리지 않는다.

### 21.5 UV7 음성 시험

8장 UV7(수명 사건 순서·GC 결과·통지 횟수·진단 코드) 중 지금 구현된 것을 시험으로 만든다.
- 명시 해제: `OnConstruct`·`OnDestruct`가 짝을 이룬다. 두 번 해제해도 통지는 한 번이다.
- 미룬 해체: 부모 트리를 버리면 소멸자 안에서는 통지가 0회다. 다음 `BeginFrame`에 정확히 1회 돌고, 그동안 Widget이 GC되지 않는다.
- `ScreenContainer`·`ListView` 소멸: 사용자 hook 0회, 명시 해제 뒤 1회.
- 결합 표·ID 검증 오류 진단 코드: 없는 속성, 형식 불일치, 같은 이름 둘.
- 화면 입력 설정: 같은 설정은 적용 0회, ignore 해제는 설정 뒤에만, Menu 진입 때 키 풀기 1회.

### 21.6 단계

| 단계 | 범위 | 완료 판정 |
|---|---|---|
| UW9a | 이름 바꾸기(21.1절, 기계 변경) | 옛 이름 0(코드·문서 대응표 밖), ID 명세 별칭 |
| UW9b | 편집 텍스트 Element를 UiFramework로, `TextShaper`(21.2절) | 기존 편집 시험 통과, Element 층 편집 시험 |
| UW9c | 화면 입력 설정 적용, `platform::Window` 커서·잠금(21.3절) | 적용 횟수 시험, UiDemo self-test |
| UW9d | 소멸자 사용자 코드 제거, 해체 대기열, policy(21.4절) | UV7 수명 시험 |
| UW9e | UV7 음성 시험 묶음(21.5절) | 21.5절 항목 전부 |
