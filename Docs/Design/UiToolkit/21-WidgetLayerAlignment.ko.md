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
  - UiFramework `Controls/EditableText`가 글자·선택·조합·이력을 스스로 든다. 생성자는 셰이퍼, 클립보드(`Platform`), 여러 줄, 상자 모양을 받는다. `Initialize(글자, 글꼴 크기)`가 레이블을 만든다. 힌트·오류·비밀번호·읽기 전용·초점 때 전체 선택·확정 때 전체 선택·Esc 되돌리기는 setter다(그 전에 정해도 된다). 최대 길이는 UE에 없는 기능이라 두지 않는다.
  - 사건은 delegate다:
    - `onTextChanged(StringView)` — UE `OnTextChanged`. 사용자 편집만 부르고, 조합 중에는 부르지 않는다.
    - `onTextCommitted(StringView, TextCommitReason)` — UE `OnTextCommitted`. 사유는 지금의 Enter·FocusLost 둘이다(UE `ETextCommit` OnEnter·OnUserMovedFocus). Default·OnCleared는 부르는 곳이 생길 때 더한다.
  - 글자 셰이핑은 UiFramework `TextShaper` 인터페이스로 받는다. 지금의 `WidgetTextShaper`가 이 이름으로 내려온다. 셰이핑이 화면 언어·글꼴·아틀라스에 묶이는 것은 Widget 층의 일이 아니라 호스트의 일이기 때문이다. 이름의 `Widget` 접두도 뺀다.
  - Widget 층 `EditableTextWidget`·`MultiLineEditableTextWidget`·`…BoxWidget`은 UE `UEditableText`처럼 Element를 만들고 속성을 내린다. Element 사건을 받아 자기 속성(`text`)을 바꾸고 통지한다. `BindLiveWidgetEvent`로 수명을 확인하는 방식은 지금의 다른 Widget과 같다.
- **동작 불변.** 이동 뒤에도 19.2절 IME 계약, undo 묶음, 비밀번호 마스킹, 오류 캡션이 같아야 한다. 지금의 `EditableTextWidgetTests` 전부가 그대로 통과해야 한다. Element 층 시험(Widget 없이 같은 장면)을 더한다.
- **UW4f2와의 관계.** 9.1절은 UW4f2(편집 텍스트 공개)를 UW9에서 하기로 했다. UW9b는 그중 병합 조건인 두 가지를 한다: 상태 소유를 Element로 옮기기, 셰이퍼 경계. 나머지는 병합 뒤 UW4f2에 남긴다.
  - `EditableTextStyle`·`EditableTextBoxStyle`
  - `EditableTextBox` Element와 오류 표지 팝업
  - 단어·줄 선택, undo 99단계, 힌트 불투명도, caret 깜박임
  - 여러 줄 세로 스크롤 막대
  - `TextShaper`는 지금의 `Shape(TextLabel&, const Text&, float)`를 그대로 내린다. UW4f2의 `FontStyle` 서명은 UW4 글꼴 자동 셰이핑과 함께 바꾼다.
- **버린 대안.** Element가 Widget 모델 인터페이스(`EditableTextModel`)를 받는 방식이 있다. 이렇게 하면 두 층의 상태가 하나로 남지만, 에디터가 그 인터페이스를 구현해야 하고 UE 구조(Element가 글자를 든다)와 다르다.

### 21.3 화면 입력 설정 적용(UE CommonUI `FUIInputConfig`)

- **지금.**
  - `ScreenInputConfig`(mode·showCursor·captureMouse·ignoreMove·ignoreLook)는 화면 속성으로 저장된다.
  - 그런데 `ScreenInputRouter`는 `mode`만 라우팅에 쓴다. 커서·마우스 붙잡기·이동·시선 무시는 어디에도 적용되지 않는다.
- **바꿈(UE `UCommonUIActionRouterBase::ApplyUIInputConfig`, CommonUIActionRouterBase.cpp 1866–1960행).**
  - 활성 화면의 설정이 바뀔 때만 적용한다(1868행). 다음 셋이 계기다: 화면 활성·비활성, 화면 스택 변화, 활성 화면의 `SetInputConfig`.
  - 적용은 호스트 인터페이스 `ScreenInputHost`로 넘긴다. UE에서 이 일은 PlayerController와 GameViewportClient가 한다. Rocks에서는 게임 쪽(World·Gameplay)이 구현하고, UiScreens는 Gameplay에 의존하지 않는다.
    - `SetIgnoreMoveInput(bool)`·`SetIgnoreLookInput(bool)`: 이전 값과 다를 때만 부른다. 이전에 true로 부른 적이 없으면 false를 부르지 않는다(1888–1899행).
    - `SetMouseCapture(captured, cursorVisible)`: 1908–1960행의 capture 방식을 Rocks의 두 값으로 줄인 것이다. 붙잡지 않으면 커서는 늘 보인다(NoCapture → `SetShowMouseCursor(true)`). 붙잡으면 `showCursor`대로다. 다시 보일 때 커서를 가운데로 옮기는 UE 동작(1957–1980행)은 하지 않는다 — 입력 장치 종류 설정이 생길 때 더한다.
    - `FlushPressedKeys()`: Menu로 바뀔 때 한 번 부른다(1901–1906행). 눌린 채 남은 게임 입력을 푼다. UE는 다음 tick으로 미루는데, Rocks는 라우터가 다음 입력 처리 앞에서 부른다(명시 순서 — 시간 경쟁 없음).
  - 게임패드 아날로그 반복과 눌린 UI 키는 Menu ↔ Game 전환 때 `Application::CancelGamepadKeys`로 푼다(20.8절).
- **플랫폼(PW 보강).**
  - `platform::Window`에 `SetCursorVisible(bool)`·`SetMouseLocked(bool)`을 더한다(UE `FReply::LockMouseToWidget`·`SetShowMouseCursor`).
  - Win64 커서 숨김은 `WM_SETCURSOR`에서 빈 커서(`SetCursor(nullptr)`)를 둔다. `ShowCursor` 계수는 스레드 전체 값이라 쓰지 않는다. 잠금은 `ClipCursor`(창 client 영역, 화면 좌표)다. 창 이동·크기 변경·초점 얻음 때 다시 걸고, 초점 잃음·최소화·창 소멸 때 이 창이 건 잠금만 푼다.
  - 다른 backend는 기본 구현이 `Unsupported`다. Mac은 feature/platform-mac에서 맞춘다.
  - 고정밀 상대 이동(UE `UseHighPrecisionMouseMovement` — raw input)은 이번 범위 밖이다. 10장 위험 목록에 남긴다.
- **호출 시점.** `ScreenInputRouter::ApplyInputConfig`는 `Route` 맨 앞에서 돈다. 화면을 바꾼 프레임에는 호스트가 입력 전에 한 번 부른다. 마지막 적용 값과 같으면 아무것도 하지 않는다.
- **시험.** `ScreenInputConfigTests`가 적용 횟수·순서를 단언한다. UiDemo 게임 HUD는 `ScreenInputRouter`를 쓰지 않으므로 표본 호스트는 Gameplay 연결 때 만든다. Win64 잠금·숨김은 사람 확인이다(창 초점이 필요하다).

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
  - 대기열은 다음 때 한 번에 돈다: 프레임 시작, 그리고 호스트가 창을 부수기 전에 부르는 `Application::Shutdown()`.
  - 결과적으로 해체 통지는 항상 한 번, 소멸자 밖에서 돈다.
  - 호스트 묶음을 찾을 수 없는 Element(앱에 붙은 적 없음)는 생성 통지도 받은 적이 없으므로 넣을 것이 없다.
  - `Application` 소멸 때 남은 항목은 부르지 않고 경고 로그로 개수를 남긴다(소멸자 규칙).
- **구현(UW9d).**
  - 대기열은 `UiHostServices::DeferTeardown`·`RunDeferredTeardown`(통지 안에서 넣은 통지도 같은 호출에서 돈다, 상한 64바퀴, 재진입은 0)이다. `Application`이 프레임 시작(`StartFrame` — `BeginFrame`과 암묵 프레임 모두)과 `Shutdown()`(루트를 떼고 돌림)에서 돌린다.
  - 소멸 때 남은 통지는 `DiscardDeferredTeardown`으로 버리고 개수를 경고로 남긴다. 소멸자는 루트를 먼저 놓아, 그때 넣어지는 통지까지 센다.
  - 소멸자는 부모 사슬이 이미 끊겼을 수 있어 묶음을 찾을 수 없다. 그래서 요소가 **붙을 때** 묶음을 기억한다. `Element::OnUiHostServicesChanged`는 요소가 묶음 있는 트리에 붙을 때 부르는 hook이다(묶음을 단 루트가 되거나, 묶음 있는 트리에 하위 트리로 들어갈 때 — 측정 여부와 무관). 그래서 비활성 Switcher 페이지·고르지 않은 탭도 받는다. `UserWidgetRoot`는 여기서 묶음의 약한 참조를 남긴다.
  - 붙을 때 그 하위 트리의 스타일 기억도 비운다. 다른 묶음이 같은 주소를 재사용해도 다시 받는다(20.11절 리뷰 반영 보강).
  - 앱에 붙은 적 없이, 또는 마지막으로 붙었던 앱이 부서진 뒤에 버려진 `UserWidget` Element는 통지할 자리가 없다(떨어질 때는 묶음 기억을 지우지 않으므로 앱이 살아 있으면 그 대기열로 간다). `OnDestruct`를 건너뛰고 경고를 남긴다. `OnConstruct`는 `TakeElement` 때 돌았으므로 짝이 깨지는 유일한 경우다. `TakeElement`를 부른 쪽이 명시 `ReleaseElements`를 쓰거나 앱을 끄기 전에 Element를 놓으라는 뜻이다(시험·데모도 이 규칙을 지켜 경고 없이 돈다 — 경고를 일부러 보는 시험 둘만 예외).
  - 미룬 해체는 루트 Widget의 `ReleaseElements`도 대기열에서 부른다. 중첩 UserWidget·목록 행 원본·화면 컨테이너의 사용자 hook이 소멸자 안에서 돌지 않게 하기 위해서다(`WidgetTree::DetachHostElements` — 엔진 정리 / `ReleaseRootWidget` — 사용자 hook). 소멸자에서는 엔진 정리만 한다.
  - 해체 순서는 UE와 같다: 바깥 `OnDestruct`를 먼저 부르고, 그다음 루트 Widget을 놓는다(중첩 `OnDestruct`). UE `SObjectWidget::ResetWidget`이 `NativeDestruct`(SObjectWidget.cpp 63행) 뒤에 자식 Slate 자원을 놓는 것과 같다 — 부모 `OnDestruct`가 아직 붙은 자식 Element를 볼 수 있다. 명시 해제(`Detach`)와 미룬 해체 모두 이 순서다(리뷰 2026-11-27 반영).
  - 미룬 해체가 돌기 전에 같은 UserWidget을 다시 구성하면(`BuildElement`) 남은 해체를 그 자리에서 먼저 끝낸다(옛 `OnDestruct` → 새 `OnConstruct`, UE 순서). UserWidget은 남은 해체 번호를 갖고, 대기열 항목은 자기 번호일 때만 해체한다 — 끝난 번호의 항목은 아무 일도 하지 않아 새 Element·루트 Widget Element를 떼지 않는다(리뷰 2026-11-27 — 번호 없이 돌면 다시 붙은 트리를 해체했다). 시험 `UiWidget_RebuildBeforeTheNextFrameFinishesThePendingDestructFirst`.
  - `Application` 소멸은 루트에서 묶음을 먼저 떼고 트리를 놓는다. 밖에서 루트를 붙잡아도 죽은 앱의 묶음을 쥐지 않는다. 대기열 항목이 `RootRef`를 쥐므로 `ObjectRegistry`는 `Application`보다 오래 살아야 한다.
  - `ScreenContainer`는 생성 때 받은 `Application`의 대기열을 쓴다.
  - `WorldWidgetComponent`(UiWorld)는 자기 창 `Application`을 갖는다. 창을 비울 때(위젯 교체·비활성·파괴) 가져온 Element를 위젯의 명시 `ReleaseElements`로 풀고(UE `UWidgetComponent::ReleaseResources` → `ReleaseSlateResources`) 파괴 때 창 `Application::Shutdown`을 부른다. 화면 공간 모드로 투영 전에 버린 Element도 이 경로라 경고 없이 `OnDestruct`가 짝을 맞춘다. 위젯을 바꿀 때는 참조를 바꾼 뒤 옛 위젯 handle로 푼다(옛 위젯은 그 사이 GC 단계가 끼지 않아 살아 있다 — 참조 변경이 실패하면 창을 건드리지 않는다).
  - 시험: `DeferredTeardownTests`가 Element·Application 수준을 본다(소멸자 안 0회 → 다음 프레임 1회, 중첩·재진입, Shutdown, 소멸 때 버림, 앱 사이 이동, ListView 명시 해제). `UserWidget`·`Screen` hook 횟수는 리플렉션 시험 클래스가 필요해 UW9e의 시험 fixture와 함께 본다.
- **ListView.** 소멸자는 행 원본을 부르지 않는다. 행 Widget의 풀 반환은 `ReleaseAllRows`나 `WidgetPool` 소멸이 맡는다(UE `ResetPool`).
- **policy.** UI 모듈 Element·Widget 소멸자 본문에서 사용자 hook 이름(`OnDestruct`·`OnDeactivated`·`ReleaseRow`·`Execute`)을 부르는 줄을 거부한다(`ui-destructor-user-code`). 엔진 내부 정리는 hook 이름을 쓰지 않으므로 걸리지 않는다.

### 21.5 UV7 음성 시험

8장 UV7(수명 사건 순서·GC 결과·통지 횟수·진단 코드) 중 지금 구현된 것을 시험으로 만든다.
- 명시 해제: `OnConstruct`·`OnDestruct`가 짝을 이룬다. 두 번 해제해도 통지는 한 번이다.
- 미룬 해체: 부모 트리를 버리면 소멸자 안에서는 통지가 0회다. 다음 `BeginFrame`에 정확히 1회 돌고, 그동안 Widget이 GC되지 않는다.
- `ScreenContainer`·`ListView` 소멸: 사용자 hook 0회, 명시 해제 뒤 1회.
- 결합 표·ID 검증 오류 진단 코드: 없는 속성, 형식 불일치, 같은 이름 둘.
- 화면 입력 설정: 같은 설정은 적용 0회, ignore 해제는 설정 뒤에만, Menu 진입 때 키 풀기 1회.

- **구현(UW9e).** 시험용 화면은 리플렉션 헤더 대신 native 클래스(`ROCKS_NATIVE_TYPE` + `RegisterNativeClass` — ObjectRuntime NativeClass.h)로 시험 파일 안에 둔다. 저장하지 않는 수명 hook 계수만 필요해서 fixture 모듈을 만들지 않는다.
  - `WidgetLifecycleTests`: 명시 해제 짝·두 번 해제, 미룬 해체(소멸자 안 0회 → 다음 프레임 1회, 그동안 `Collect` 뒤에도 살아 있음), 측정 전 버림(OnDestruct 없음), 결합 표 진단(없는 필드 NotFound·bool 필드 TypeMismatch·같은 필드 둘 AlreadyExists·빈 값 InvalidArgument).
  - `ScreenLifecycleTests`: 버려진 컨테이너의 OnDeactivated·OnDestruct가 다음 프레임에 그 순서로 한 번씩.
  - `DeferredTeardownTests`(UW9d)·`ScreenInputConfigTests`(UW9c)와 이벤트 결합 진단(`WidgetEventBindingTests` — 중복·없는 메서드·서명 불일치)이 나머지 항목이다.
  - 남은 UV7 항목(GC 결과·Widget 풀 재사용·Designer 미리보기 동일)은 각 기능 단계(UT5·Designer)에서 더한다.

### 21.5a Widget 사건을 리플렉션 이벤트로(UW9f — 최종 동기화 뒤)

- **지금.** UI Widget의 사건 30개가 `ROCKS_NATIVE() Delegate<…>`다(UiWidget·UiScreens 15개 헤더, 예: `ButtonWidget::clicked_`). C++ 콜백 하나에는 동작하지만 네 가지가 UE와 다르다.
  - 구독자가 하나다. `SetOnClicked`가 앞 구독을 덮어쓴다(UE `OnClicked`는 multicast다).
  - 리플렉션에 보이지 않는다. Sparks `on button.Clicked`와 WidgetAsset 바인딩 표(EventReflection EV3b — UE `UComponentDelegateBinding`)가 가리킬 수 없고, 에디터 이벤트 목록(EV5)에도 나오지 않는다.
  - 저장되지 않는다.
  - 대상이 파괴돼도 자동으로 풀리지 않는다. lambda가 잡은 대상이 먼저 죽으면 끊긴 참조가 남는다.
- **UE.** `UButton::OnClicked`는 `UPROPERTY(BlueprintAssignable) FOnButtonClickedEvent`(dynamic multicast — Button.h 18·76행)다. UMG 위젯 사건은 모두 이 모양이다.
- **바꿈.**
  - 사건 필드를 `ROCKS_PROPERTY(Params = "…") Event<Args…>`(ObjectRuntime `Object/Event.h`, EventReflection ED1·ED14)로 바꾼다. Sparks 노출 지정자 `ScriptAssignable`은 EV4가 생기면 붙인다.
  - 공개 API는 `Event<…>& OnClicked()` 접근자다. C++ 엔진 코드는 `TryAddLambda`로 native 구독, 에셋·Sparks는 `TryBind`로 리플렉션 바인딩한다. `SetOnX(Delegate)`는 없앤다(하위호환 없음).
  - Element 사건에서의 발행은 `Broadcast({registry, access, Handle()}, …)`다. 발행자 수명은 Event가 root로 붙잡는다(ED6). 지금의 `BindLiveWidgetEvent` 생존 확인(재구성된 Element의 늦은 사건 버림)은 그대로 둔다.
  - 인자 규칙(ED1)에 맞춘다. `StringView` 인자는 `const String&`로 바꾼다(편집 텍스트 `changed`·`committed` 등). 값 인자는 nothrow 복사, struct는 `const T&`다.
  - 이벤트 필드를 가진 클래스는 `T(Allocator&)` 생성자가 있어야 한다(ED14). Widget은 이미 그렇다.
  - 인자 형식은 리플렉션 형식이어야 한다(ED1·FD4 — 바인딩 frame). UiCore·UiFramework는 Object 층에 의존하지 않아 리플렉션이 없으므로 다음처럼 싣는다.
    - `CheckState`·`TextCommitReason`·`SpinCommitReason`(UiFramework enum)은 Widget 층의 리플렉션 enum `WidgetCheckState`·`WidgetTextCommitReason`·`WidgetSpinCommitReason`으로 넘긴다. UE UMG가 Slate `EVisibility`에 대응하는 `ESlateVisibility`를 두는 방식이다. 값은 같은 번호이고, 헤더마다 변환 함수와 번호 일치 static_assert를 둔다. Binds 미러는 CoreTypeBindings 모듈에서만 쓸 수 있어(메타 컴파일러 RMC2004 — 두 모듈이 한 형식을 싣지 않게) 쓰지 않는다.
    - `KeyChord`는 UE `FInputChord`처럼 리플렉션 struct `InputChordValue`(키 이름 + Shift·Ctrl·Alt·Super)로 넘긴다. 키 선택기 위젯이 이미 키를 이름으로 저장한다. 키 enum 전체를 미러로 옮기지 않는다.
    - `StringView`는 `const String&`이다(발행 때 한 번 복사).
  - 바인딩은 모두 `Transient`다. 에셋 바인딩은 EV3b 템플릿 바인딩 표가 맡고, 실행 중 바인딩을 Widget 에셋 바이트에 넣지 않는다.
  - 사건이 아닌 `DelegateHandle` 3개(구독 handle)는 그대로 `ROCKS_NATIVE`다.
  - 프로그램 setter의 발행은 UE 위젯마다 따른다(리뷰 2026-11-27 정정 — 처음 판은 ComboBox도 발행하지 않는다고 잘못 적었다).
    - `UExpandableArea::SetIsExpanded`는 `BroadcastFieldValueChanged`만 부른다(ExpandableArea.cpp 51–62). `ExpandableAreaWidget::SetExpanded`는 필드 통지만 하고 사건은 머리 버튼을 누를 때 보낸다.
    - `UComboBoxString::SetSelectedIndex`는 바뀌었으면 늘 `OnSelectionChanged(Item, ESelectInfo::Direct)`를 보낸다: Slate 위젯이 없으면 `HandleSelectionChanged(Direct)`, 있으면 `SComboBox::SetSelectedItem`의 목록 선택 통지다(ComboBoxString.cpp 226–247·453–468). 해제(`ClearSelection`)는 위젯이 있을 때만 `SListView::ClearSelection`의 Direct 통지로 빈 항목을 보낸다(SListView.h 1993–2002) — 옵션 삭제로 선택 항목이 사라질 때도 같다(`RemoveOption` → `ClearSelection`).
    - 그래서 콤보 선택 사건은 `(int32 index, WidgetSelectionCause cause)`다. 까닭은 Element 층 `SelectionCause`(UE `ESelectInfo` — Key·Navigation·Pointer·Direct)의 미러다. 행 클릭은 Pointer, 열린 목록의 확인 키 확정은 Direct(UE SComboBox.h 487행), 닫힌 콤보의 위·아래 키 선택은 Key(Rocks 확장), 프로그램 선택·선택 항목 삭제는 Direct다. Element 없이 고를 때도 발행해야 하므로 선택 API는 등록부를 받는다(`SetSelectedIndex(objects, index)`·`ClearSelection(objects)`·`SelectOption(objects, …)`).
- **시점.** `Event`는 engine-runtime에 있고(EV1b e12c17c5·EV2b 5778c89c) 이 브랜치에는 아직 없다. 최종 동기화 바로 뒤, 병합 전에 한다. 손으로 쓴 속성 표·descriptor 시험의 이벤트 필드 건너뛰기(EV2a `IsEventField`)를 같은 단계에서 점검한다.
- **구현(UW9f).**
  - 30개 필드를 `ROCKS_PROPERTY(Transient, Name = …, Params = …) Event<…>`로 바꿨다. 공개 접근자는 `OnX()`다. 생성자가 없던 6개(`ComboBoxWidgetBase`·`ListViewBaseWidget`·`RadialSlider`·`ScrollBar`·`Slider`·`SpinBox`)에 `T(Allocator&)`를 더했고, 파생(ComboBox String·Key, ListView)은 기반에 allocator를 넘긴다.
  - 발행은 `BindLiveWidgetEvent`의 `Event` 판과 Element 사건 lambda의 `Broadcast({objects, access, owner}, …)`다.
  - 편집 텍스트는 발행마다 `String` 사본을 넘긴다. 키 선택기는 `InputChordValue`로 바꿔 넘긴다.
  - ExpandableArea 펼침의 프로그램 발행을 없앴다(머리 버튼을 누를 때 보낸다). 콤보 선택은 UE처럼 프로그램 선택도 Direct로 보낸다(위 항목).
  - Widget 층 미러 enum(`WidgetCheckState`·`WidgetTextCommitReason`·`WidgetSpinCommitReason`·`WidgetSelectionCause`)은 값 번호와 함께 개수도 대조한다(Element 층 enum 끝 표지 `Count` — 한쪽에만 값이 생기면 컴파일 오류).
  - ID 명세에 이벤트 필드 31개, `InputChordValue`(필드 5개), Widget enum 셋을 등록했다.
  - 도구 이름(`Name`)은 UE 이벤트 이름에서 `On`을 뗀 것이다(`checkStateChanged`·`textChanged`·`textCommitted`·`valueChanged`·`valueCommitted` — UE `OnCheckStateChanged`·`OnTextChanged`·`OnTextCommitted`·`OnValueChanged`·`OnValueCommitted`). 맨 이름 `changed`는 쓰지 않는다: Sparks 이벤트 이름은 도구 이름 첫 글자를 대문자로 쓰고 `Changed`는 예약 이름이다(EventReflection EV4·SparksBinding 9절). CheckBox의 세 상태 이벤트 `stateChanged`(`WidgetCheckState`)는 UE에 없는 확장이라 따로 둔다. Sparks 노출(`ScriptAssignable`)은 병합 뒤 정한다.
- **시험.** 구독 둘이 모두 불림, 대상 파괴 뒤 자동 해제, 재구성된 Element의 늦은 사건 버림. 바인딩 저장(EV3a1 — `Transient`가 아니면)은 그래프 저장 시험 하나로 본다.

### 21.6 단계

| 단계 | 범위 | 완료 판정 |
|---|---|---|
| UW9a | 이름 바꾸기(21.1절, 기계 변경) | 옛 이름 0(코드·문서 대응표 밖), ID 명세 별칭 |
| UW9b | 편집 텍스트 Element를 UiFramework로, `TextShaper`(21.2절) | 기존 편집 시험 통과, Element 층 편집 시험 |
| UW9c | 화면 입력 설정 적용, `platform::Window` 커서·잠금(21.3절) | 적용 횟수 시험, UiDemo self-test |
| UW9d | 소멸자 사용자 코드 제거, 해체 대기열, policy(21.4절) | UV7 수명 시험 |
| UW9e | UV7 음성 시험 묶음(21.5절) | 21.5절 항목 전부 |
| UW9f | Widget 사건 30개를 리플렉션 `Event`로(21.5a절 — 최종 동기화 뒤, 병합 전) | 구독 둘·자동 해제·늦은 사건 버림 시험, 옛 `SetOnX` 0 |
